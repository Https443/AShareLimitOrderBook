#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_SPSC_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_SPSC_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include "detail.h"

// SPSC RingBuffer：
// - 仅支持 1 个生产者 + 1 个消费者。
// - 固定容量，热路径无动态内存分配。
// - tryPush/tryPop 为非阻塞接口；push/pop 为自旋等待接口。
template<typename T>
class SpscRingBuffer
{
    static_assert(std::is_default_constructible<T>::value, "T must be default constructible");

private:
    size_t _cap = 0;
    size_t _mask = 0;
    std::unique_ptr<T[]> _buf;

    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _head;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _tail;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) size_t _producerHeadCache = 0;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) size_t _consumerTailCache = 0;

public:
    explicit SpscRingBuffer(size_t cap = 4096)
        : _cap(ringbuffer_detail::round2(cap)),
          _mask(_cap - 1),
          _buf(std::make_unique<T[]>(_cap)),
          _head(0),
          _tail(0)
    {
    }

    SpscRingBuffer(const SpscRingBuffer &) = delete;
    SpscRingBuffer &operator=(const SpscRingBuffer &) = delete;

    inline size_t capacity() const
    {
        return _cap;
    }

    inline size_t size() const
    {
        size_t tail = _tail.load(std::memory_order_acquire);
        size_t head = _head.load(std::memory_order_acquire);
        return tail - head;
    }

    inline bool empty() const
    {
        return size() == 0;
    }

    inline bool tryPush(const T &value)
    {
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t headCache = _producerHeadCache;

        if (tail - headCache >= _cap)
        {
            headCache = _head.load(std::memory_order_acquire);
            _producerHeadCache = headCache;
            if (tail - headCache >= _cap)
            {
                return false;
            }
        }

        _buf[tail & _mask] = value;
        _tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    inline bool tryPush(T &&value)
    {
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t headCache = _producerHeadCache;

        if (tail - headCache >= _cap)
        {
            headCache = _head.load(std::memory_order_acquire);
            _producerHeadCache = headCache;
            if (tail - headCache >= _cap)
            {
                return false;
            }
        }

        _buf[tail & _mask] = std::move(value);
        _tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    template<typename... Args>
    inline bool tryEmplace(Args &&... args)
    {
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t headCache = _producerHeadCache;

        if (tail - headCache >= _cap)
        {
            headCache = _head.load(std::memory_order_acquire);
            _producerHeadCache = headCache;
            if (tail - headCache >= _cap)
            {
                return false;
            }
        }

        _buf[tail & _mask] = T(std::forward<Args>(args)...);
        _tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    inline void push(const T &value)
    {
        while (!tryPush(value))
        {
            ringbuffer_detail::cpuRelax();
        }
    }

    inline void push(T &&value)
    {
        while (!tryPush(std::move(value)))
        {
            ringbuffer_detail::cpuRelax();
        }
    }

    template<typename... Args>
    inline void emplace(Args &&... args)
    {
        while (!tryEmplace(std::forward<Args>(args)...))
        {
            ringbuffer_detail::cpuRelax();
        }
    }

    inline bool tryPop(T &out)
    {
        size_t head = _head.load(std::memory_order_relaxed);
        size_t tailCache = _consumerTailCache;

        if (tailCache == head)
        {
            tailCache = _tail.load(std::memory_order_acquire);
            _consumerTailCache = tailCache;
            if (tailCache == head)
            {
                return false;
            }
        }

        out = std::move(_buf[head & _mask]);
        _head.store(head + 1, std::memory_order_release);
        return true;
    }

    inline void pop(T &out)
    {
        while (!tryPop(out))
        {
            ringbuffer_detail::cpuRelax();
        }
    }

    inline bool pop_front(T &out)
    {
        return tryPop(out);
    }
};


#endif