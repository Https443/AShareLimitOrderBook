#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_MPSC_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_MPSC_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include "detail.h"

// MPSC zero-copy RingBuffer:
// - 支持多生产者 + 单消费者。
// - 写侧通过 tail CAS 抢占逻辑位置；读侧单消费者无需 CAS。
// - 提供 acquire/commit 风格接口，把 payload 读写直接交给调用方。
template<typename T>
class MpscRingBufferZC
{
    static_assert(std::is_default_constructible<T>::value,
                  "T must be default constructible");

private:
    using Slot = ringbuffer_detail::SlotStorageImpl<T>;
    static constexpr size_t kInvalidPosition = std::numeric_limits<size_t>::max();

public:
    struct WriteReservation
    {
        T* data = nullptr;
        size_t position = kInvalidPosition;
    };

    struct ReadReservation
    {
        const T* data = nullptr;
        size_t position = kInvalidPosition;
    };

private:
    size_t _cap = 0;
    size_t _mask = 0;
    std::unique_ptr<Slot[]> _buf;

    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _head;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _tail;

    static inline void resetWriteReservation(WriteReservation& reservation)
    {
        reservation.data = nullptr;
        reservation.position = kInvalidPosition;
    }

    static inline void resetReadReservation(ReadReservation& reservation)
    {
        reservation.data = nullptr;
        reservation.position = kInvalidPosition;
    }

    inline bool tryAcquireWriteImpl(size_t& tail, WriteReservation& reservation)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (true)
        {
            Slot& slot = _buf[tail & _mask];
            const size_t seq = slot.sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) -
                                  static_cast<intptr_t>(tail);

            if (diff == 0)
            {
                if (_tail.compare_exchange_weak(tail, tail + 1,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed))
                {
                    reservation.data = &slot.data;
                    reservation.position = tail;
                    return true;
                }

                backoff.pause();
                continue;
            }

            if (diff < 0)
            {
                return false;
            }

            tail = _tail.load(std::memory_order_relaxed);
            backoff.pause();
        }
    }

    inline bool tryAcquireReadImpl(ReadReservation& reservation)
    {
        const size_t head = _head.load(std::memory_order_relaxed);
        Slot& slot = _buf[head & _mask];
        const size_t seq = slot.sequence.load(std::memory_order_acquire);

        if (seq != head + 1)
        {
            return false;
        }

        reservation.data = &slot.data;
        reservation.position = head;
        return true;
    }

public:
    explicit MpscRingBufferZC(size_t cap = 4096)
        : _cap(ringbuffer_detail::round2(cap)),
          _mask(_cap - 1),
          _buf(std::make_unique<Slot[]>(_cap)),
          _head(0),
          _tail(0)
    {
        for (size_t i = 0; i < _cap; ++i)
        {
            _buf[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MpscRingBufferZC(const MpscRingBufferZC&) = delete;
    MpscRingBufferZC& operator=(const MpscRingBufferZC&) = delete;

    inline size_t capacity() const
    {
        return _cap;
    }

    // 这里只能给近似值：tail 包含已 reserve 但尚未 commit 的写入。
    inline size_t size() const
    {
        const size_t head = _head.load(std::memory_order_acquire);
        const size_t tail = _tail.load(std::memory_order_acquire);
        const size_t used = tail - head;
        return used > _cap ? _cap : used;
    }

    inline bool empty() const
    {
        const size_t head = _head.load(std::memory_order_acquire);
        const Slot& slot = _buf[head & _mask];
        return slot.sequence.load(std::memory_order_acquire) != head + 1;
    }

    inline bool tryAcquireWrite(WriteReservation& reservation)
    {
        resetWriteReservation(reservation);
        size_t tail = _tail.load(std::memory_order_relaxed);
        return tryAcquireWriteImpl(tail, reservation);
    }

    inline T* acquireWrite(WriteReservation& reservation)
    {
        resetWriteReservation(reservation);

        size_t tail = _tail.load(std::memory_order_relaxed);
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryAcquireWriteImpl(tail, reservation))
        {
            backoff.pause();
        }
        return reservation.data;
    }

    inline void commitWrite(WriteReservation& reservation)
    {
        if (reservation.data == nullptr)
        {
            return;
        }

        _buf[reservation.position & _mask].sequence.store(
            reservation.position + 1, std::memory_order_release);
        resetWriteReservation(reservation);
    }

    inline bool tryAcquireRead(ReadReservation& reservation)
    {
        resetReadReservation(reservation);
        return tryAcquireReadImpl(reservation);
    }

    inline const T* acquireRead(ReadReservation& reservation)
    {
        resetReadReservation(reservation);

        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryAcquireReadImpl(reservation))
        {
            backoff.pause();
        }
        return reservation.data;
    }

    inline void commitRead(ReadReservation& reservation)
    {
        if (reservation.data == nullptr)
        {
            return;
        }

        const size_t head = reservation.position;
        _buf[head & _mask].sequence.store(head + _cap, std::memory_order_release);
        _head.store(head + 1, std::memory_order_relaxed);
        resetReadReservation(reservation);
    }
};

#endif
