#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_SPMC_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_SPMC_H

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


// SPMC RingBuffer：
// - 仅支持 1 个生产者 + N 个消费者。
// - 消费侧通过 CAS 竞争 head 来抢占元素。
template<typename T>
class SpmcRingBuffer
{
    static_assert(std::is_default_constructible<T>::value, "T must be default constructible");
    static_assert(std::is_nothrow_copy_assignable<T>::value, "T must be nothrow copy assignable");
    static_assert(std::is_nothrow_move_assignable<T>::value, "T must be nothrow move assignable");

private:
    // sequence == pos      ：生产者可写
    // sequence == pos + 1  ：消费者可读
    using Slot = ringbuffer_detail::SlotStorageImpl<T>;

    size_t _cap = 0;
    size_t _mask = 0;
    std::unique_ptr<Slot[]> _buf;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _tail;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _head;

public:
    explicit SpmcRingBuffer(size_t cap = 4096)
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

    SpmcRingBuffer(const SpmcRingBuffer &) = delete;
    SpmcRingBuffer &operator=(const SpmcRingBuffer &) = delete;

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
        const size_t pos = _tail.load(std::memory_order_relaxed);
        Slot &slot = _buf[pos & _mask];
        const size_t seq = slot.sequence.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

        if (diff != 0)
        {
            return false;
        }

        slot.data = value;
        slot.sequence.store(pos + 1, std::memory_order_release);
        _tail.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    inline bool tryPush(T &&value)
    {
        const size_t pos = _tail.load(std::memory_order_relaxed);
        Slot &slot = _buf[pos & _mask];
        const size_t seq = slot.sequence.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

        if (diff != 0)
        {
            return false;
        }

        slot.data = std::move(value);
        slot.sequence.store(pos + 1, std::memory_order_release);
        _tail.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    template<typename... Args>
    inline bool tryEmplace(Args &&... args)
    {
        const size_t pos = _tail.load(std::memory_order_relaxed);
        Slot &slot = _buf[pos & _mask];
        const size_t seq = slot.sequence.load(std::memory_order_acquire);
        const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

        if (diff != 0)
        {
            return false;
        }

        slot.data = T(std::forward<Args>(args)...);
        slot.sequence.store(pos + 1, std::memory_order_release);
        _tail.store(pos + 1, std::memory_order_relaxed);
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
        size_t pos = _head.load(std::memory_order_relaxed);
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (true)
        {
            Slot &slot = _buf[pos & _mask];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0)
            {
                if (_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    out = std::move(slot.data);
                    slot.sequence.store(pos + _cap, std::memory_order_release);
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

    inline void pop(T &out)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryPop(out))
        {
            backoff.pause();
        }
    }

    inline bool pop_front(T &out)
    {
        return tryPop(out);
    }
};

#endif
