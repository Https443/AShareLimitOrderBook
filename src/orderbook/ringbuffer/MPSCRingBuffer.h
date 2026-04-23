#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_MPSC_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_MPSC_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include "detail.h"

// MPSC RingBuffer:
// - 支持多生产者 + 单消费者。
// - 固定容量，容量会向上取整到 2^n。
// - 写侧使用每槽 sequence + tail CAS 抢占写位置。
// - 读侧是单消费者，消费路径不需要 CAS。
// - 该版本只保留 push/pop 风格接口，把写入、发布合并到同一热路径里。
template<typename T>
class MpscRingBuffer
{
    static_assert(std::is_default_constructible<T>::value,
                  "T must be default constructible");

private:
    using Slot = ringbuffer_detail::SlotStorageImpl<T>;

    size_t _cap = 0;
    size_t _mask = 0;
    std::unique_ptr<Slot[]> _buf;

    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _head;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _tail;

    template<typename Writer>
    inline bool tryWrite(Writer&& writer)
    {
        size_t tail = _tail.load(std::memory_order_relaxed);
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
                    writer(slot.data);
                    slot.sequence.store(tail + 1, std::memory_order_release);
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

public:
    explicit MpscRingBuffer(size_t cap = 4096)
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

    MpscRingBuffer(const MpscRingBuffer&) = delete;
    MpscRingBuffer& operator=(const MpscRingBuffer&) = delete;

    inline size_t capacity() const
    {
        return _cap;
    }

    // 这里只能给近似值：tail 包含已抢到写槽位但尚未完成发布的写入。
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

    inline bool tryPush(const T& value)
    {
        return tryWrite([&value](T& slotData)
        {
            slotData = value;
        });
    }

    inline bool tryPush(T&& value)
    {
        return tryWrite([&value](T& slotData)
        {
            slotData = std::move(value);
        });
    }

    template<typename... Args>
    inline bool tryEmplace(Args&&... args)
    {
        return tryWrite([&args...](T& slotData)
        {
            slotData = T(std::forward<Args>(args)...);
        });
    }

    inline void push(const T& value)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryPush(value))
        {
            backoff.pause();
        }
    }

    inline void push(T&& value)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryPush(std::move(value)))
        {
            backoff.pause();
        }
    }

    template<typename... Args>
    inline void emplace(Args&&... args)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryEmplace(std::forward<Args>(args)...))
        {
            backoff.pause();
        }
    }

    inline bool tryPop(T& out)
    {
        const size_t head = _head.load(std::memory_order_relaxed);
        Slot& slot = _buf[head & _mask];
        const size_t seq = slot.sequence.load(std::memory_order_acquire);

        if (seq != head + 1)
        {
            return false;
        }

        out = std::move(slot.data);
        slot.sequence.store(head + _cap, std::memory_order_release);
        _head.store(head + 1, std::memory_order_relaxed);
        return true;
    }

    inline void pop(T& out)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryPop(out))
        {
            backoff.pause();
        }
    }

    inline bool pop_front(T& out)
    {
        return tryPop(out);
    }
};

#endif
