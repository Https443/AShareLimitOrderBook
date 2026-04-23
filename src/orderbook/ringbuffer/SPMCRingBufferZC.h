#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_SPMC_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_SPMC_H

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
class SpmcRingBufferZC
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
    // 单 producer 推进 tail；多 consumer 通过 CAS 抢占 head。
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _tail;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _head;

public:
    explicit SpmcRingBufferZC(size_t cap = 4096)
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

    SpmcRingBufferZC(const SpmcRingBufferZC &) = delete;
    SpmcRingBufferZC &operator=(const SpmcRingBufferZC &) = delete;

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
        const size_t pos = _tail.load(std::memory_order_relaxed);
        Slot &slot = _buf[pos & _mask];
        const size_t seq = slot.sequence.load(std::memory_order_acquire);
        // seq == pos 表示该槽位已经完成上一轮消费，现在可以写本轮数据。
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
        if (diff != 0)
        {
            return false;
        }

        reservation.data = &slot.data;
        reservation.position = pos;
        return true;
    }

    inline T *acquireWrite(WriteReservation &reservation)
    {
        while (!tryAcquireWrite(reservation))
        {
            ringbuffer_detail::cpuRelax();
        }
        return reservation.data;
    }

    inline void commitWrite(WriteReservation &reservation)
    {
        if (reservation.data == nullptr)
        {
            return;
        }

        const size_t pos = reservation.position;
        // 先把槽位标记成“可读”，再让 tail 指向下一个逻辑位置。
        _buf[pos & _mask].sequence.store(pos + 1, std::memory_order_release);
        _tail.store(pos + 1, std::memory_order_relaxed);
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
            // seq == pos + 1 表示 producer 已完整写完，consumer 可以争抢这条消息。
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0)
            {
                // 谁 CAS 成功，谁拿到这个逻辑读位置。
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

        // 把 sequence 推进到下一轮，producer 再次看到时就知道该槽位可写。
        _buf[reservation.position & _mask].sequence.store(
            reservation.position + _cap, std::memory_order_release);
        reservation.data = nullptr;
    }
};

#endif