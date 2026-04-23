#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_MPMC_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_MPMC_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include "detail.h"

// 所有队列统一采用 zero-copy 接口：
// - 非阻塞：tryAcquireWrite / tryAcquireRead
// - 阻塞：acquireWrite / acquireRead
// - 发布/释放：commitWrite / commitRead
// 成功 acquire 后，调用方必须在同一线程上完成对应的 commit。
// 心智模型可以理解成：
// 1. acquire 负责拿到当前槽位的使用权
// 2. commit 负责把这次写入/读取的结果正式发布给另一侧

template<typename T>
class MpmcRingBufferZC
{
    static_assert(std::is_default_constructible<T>::value, "T must be default constructible");

public:
    struct WriteReservation
    {
        T *data = nullptr;
        size_t position = 0;
    };

    struct ReadReservation
    {
        const T *data = nullptr;
        size_t position = 0;
    };

private:
    using Slot = ringbuffer_detail::SlotStorageImpl<T>;

    size_t _cap = 0;
    size_t _mask = 0;
    std::unique_ptr<Slot[]> _buf;
    // MPMC 两边都需要 CAS 抢占逻辑位置，是同步成本最高的队列模型。
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _tail;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _head;

public:
    explicit MpmcRingBufferZC(size_t cap = 4096)
        : _cap(ringbuffer_detail::round2(cap)),
          _mask(_cap - 1),
          _buf(std::make_unique<Slot[]>(_cap)),
          _tail(0),
          _head(0)
    {
        for (size_t i = 0; i < _cap; ++i)
        {
            _buf[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MpmcRingBufferZC(const MpmcRingBufferZC &) = delete;
    MpmcRingBufferZC &operator=(const MpmcRingBufferZC &) = delete;

    inline size_t capacity() const
    {
        return _cap;
    }

    inline size_t size() const
    {
        const size_t tail = _tail.load(std::memory_order_acquire);
        const size_t head = _head.load(std::memory_order_acquire);
        return tail - head;
    }

    inline bool empty() const
    {
        return size() == 0;
    }

    inline bool tryAcquireWrite(WriteReservation &reservation)
    {
        size_t pos = _tail.load(std::memory_order_relaxed);
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (true)
        {
            Slot &slot = _buf[pos & _mask];
            const size_t seq = slot.sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0)
            {
                // producer 先用 CAS 认领逻辑写位置，再在该槽位填充 payload。
                if (_tail.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    reservation.data = &slot.data;
                    reservation.position = pos;
                    return true;
                }
                backoff.pause();
                continue;
            }

            if (diff < 0)
            {
                return false;
            }

            pos = _tail.load(std::memory_order_relaxed);
            backoff.pause();
        }
    }

    inline T *acquireWrite(WriteReservation &reservation)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryAcquireWrite(reservation))
        {
            backoff.pause();
        }
        return reservation.data;
    }

    inline void commitWrite(WriteReservation &reservation)
    {
        if (reservation.data == nullptr)
        {
            return;
        }

        // payload 写完之后再发布 sequence，让 consumer 看到“可读”。
        _buf[reservation.position & _mask].sequence.store(
            reservation.position + 1, std::memory_order_release);
        reservation.data = nullptr;
    }

    inline bool tryAcquireRead(ReadReservation &reservation)
    {
        size_t pos = _head.load(std::memory_order_relaxed);
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (true)
        {
            Slot &slot = _buf[pos & _mask];
            const size_t seq = slot.sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0)
            {
                // consumer 通过 CAS 认领逻辑读位置，避免重复消费同一槽位。
                if (_head.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    reservation.data = &slot.data;
                    reservation.position = pos;
                    return true;
                }
                backoff.pause();
                continue;
            }

            if (diff < 0)
            {
                return false;
            }

            pos = _head.load(std::memory_order_relaxed);
            backoff.pause();
        }
    }

    inline const T *acquireRead(ReadReservation &reservation)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryAcquireRead(reservation))
        {
            backoff.pause();
        }
        return reservation.data;
    }

    inline void commitRead(ReadReservation &reservation)
    {
        if (reservation.data == nullptr)
        {
            return;
        }

        // 槽位进入下一轮可写状态。
        _buf[reservation.position & _mask].sequence.store(
            reservation.position + _cap, std::memory_order_release);
        reservation.data = nullptr;
    }
};

#endif
