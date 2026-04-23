#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_SPSC_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_SPSC_H

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
class SpscRingBufferZC
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
    size_t _cap = 0;
    size_t _mask = 0;
    std::unique_ptr<T[]> _buf;

    // SPSC 只有单生产者/单消费者，不需要 CAS 和 per-slot sequence。
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _head;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _tail;
    // 两端缓存对方游标，尽量避免每次都跨核读共享原子。
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) size_t _producerHeadCache = 0;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) size_t _consumerTailCache = 0;

public:
    explicit SpscRingBufferZC(size_t cap = 4096)
        : _cap(ringbuffer_detail::round2(cap)),
          _mask(_cap - 1),
          _buf(std::make_unique<T[]>(_cap)),
          _head(0),
          _tail(0)
    {
    }

    SpscRingBufferZC(const SpscRingBufferZC &) = delete;
    SpscRingBufferZC &operator=(const SpscRingBufferZC &) = delete;

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
        const size_t tail = _tail.load(std::memory_order_relaxed);
        size_t headCache = _producerHeadCache;
        // 只有看起来写满时，producer 才回头刷新一次 consumer 的 head。
        if (tail - headCache >= _cap)
        {
            headCache = _head.load(std::memory_order_acquire);
            _producerHeadCache = headCache;
            if (tail - headCache >= _cap)
            {
                return false;
            }
        }

        reservation.data = &_buf[tail & _mask];
        reservation.position = tail;
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

        // 先保证槽位 payload 可见，再发布新的 tail。
        _tail.store(reservation.position + 1, std::memory_order_release);
        reservation.data = nullptr;
    }

    inline bool tryAcquireRead(ReadReservation &reservation)
    {
        const size_t head = _head.load(std::memory_order_relaxed);
        size_t tailCache = _consumerTailCache;
        // 只有看起来为空时，consumer 才刷新 producer 的 tail。
        if (tailCache == head)
        {
            tailCache = _tail.load(std::memory_order_acquire);
            _consumerTailCache = tailCache;
            if (tailCache == head)
            {
                return false;
            }
        }

        reservation.data = &_buf[head & _mask];
        reservation.position = head;
        return true;
    }

    inline const T *acquireRead(ReadReservation &reservation)
    {
        while (!tryAcquireRead(reservation))
        {
            ringbuffer_detail::cpuRelax();
        }
        return reservation.data;
    }

    inline void commitRead(ReadReservation &reservation)
    {
        if (reservation.data == nullptr)
        {
            return;
        }

        // 释放该槽位，producer 后续才能在下一圈重新写入。
        _head.store(reservation.position + 1, std::memory_order_release);
        reservation.data = nullptr;
    }
};

#endif