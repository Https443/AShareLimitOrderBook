#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_SPMC_BROADCAST_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_SPMC_BROADCAST_H

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


// SPMC Broadcast RingBuffer：
// - 仅支持 1 个生产者 + N 个消费者。
// - 广播语义：每条消息会被每个消费者各消费一次。
// - 固定容量；满队列时生产侧可选择阻塞等待。
// - 由于要保证广播，T 需要可拷贝（不能在消费时move走槽位内容）。
template<typename T>
class SpmcBroadcastRingBuffer
{
    static_assert(std::is_default_constructible<T>::value, "T must be default constructible");
    static_assert(std::is_copy_assignable<T>::value, "T must be copy assignable for SpmcBroadcastRingBuffer");

private:
    struct Slot
    {
        std::atomic<size_t> sequence;
        T data;

        Slot() : sequence(std::numeric_limits<size_t>::max()), data()
        {
        }
    };

    // 每个消费者的 readSeq 独占一个 cache line，避免 false sharing。
    struct alignas(ringbuffer_detail::CACHE_LINE_SIZE) AlignedSeq
    {
        std::atomic<size_t> value{0};
    };

    size_t _cap = 0;
    size_t _mask = 0;
    size_t _consumerCount = 0;
    std::unique_ptr<Slot[]> _buf;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _writeSeq;
    std::unique_ptr<AlignedSeq[]> _readSeqs;

    inline size_t minReadSeq() const
    {
        size_t minSeq = _readSeqs[0].value.load(std::memory_order_acquire);
        for (size_t i = 1; i < _consumerCount; ++i)
        {
            const size_t seq = _readSeqs[i].value.load(std::memory_order_acquire);
            if (seq < minSeq)
            {
                minSeq = seq;
            }
        }
        return minSeq;
    }

public:
    explicit SpmcBroadcastRingBuffer(size_t cap = 4096, size_t consumerCount = 1)
        : _cap(ringbuffer_detail::round2(cap)),
          _mask(_cap - 1),
          _consumerCount(std::max<size_t>(1, consumerCount)),
          _buf(std::make_unique<Slot[]>(_cap)),
          _writeSeq(0),
          _readSeqs(std::make_unique<AlignedSeq[]>(_consumerCount))
    {
    }

    SpmcBroadcastRingBuffer(const SpmcBroadcastRingBuffer &) = delete;
    SpmcBroadcastRingBuffer &operator=(const SpmcBroadcastRingBuffer &) = delete;

    inline size_t capacity() const
    {
        return _cap;
    }

    inline size_t consumerCount() const
    {
        return _consumerCount;
    }

    inline size_t pending(size_t consumerId) const
    {
        if (consumerId >= _consumerCount)
        {
            return 0;
        }
        const size_t write = _writeSeq.load(std::memory_order_acquire);
        const size_t read = _readSeqs[consumerId].value.load(std::memory_order_acquire);
        return write - read;
    }

    inline bool tryPush(const T &value)
    {
        const size_t write = _writeSeq.load(std::memory_order_relaxed);
        const size_t minRead = minReadSeq();
        if (write - minRead >= _cap)
        {
            return false;
        }

        Slot &slot = _buf[write & _mask];
        slot.data = value;
        slot.sequence.store(write, std::memory_order_release);
        _writeSeq.store(write + 1, std::memory_order_release);
        return true;
    }

    inline bool tryPush(T &&value)
    {
        const size_t write = _writeSeq.load(std::memory_order_relaxed);
        const size_t minRead = minReadSeq();
        if (write - minRead >= _cap)
        {
            return false;
        }

        Slot &slot = _buf[write & _mask];
        slot.data = std::move(value);
        slot.sequence.store(write, std::memory_order_release);
        _writeSeq.store(write + 1, std::memory_order_release);
        return true;
    }

    template<typename... Args>
    inline bool tryEmplace(Args &&... args)
    {
        const size_t write = _writeSeq.load(std::memory_order_relaxed);
        const size_t minRead = minReadSeq();
        if (write - minRead >= _cap)
        {
            return false;
        }

        Slot &slot = _buf[write & _mask];
        slot.data = T(std::forward<Args>(args)...);
        slot.sequence.store(write, std::memory_order_release);
        _writeSeq.store(write + 1, std::memory_order_release);
        return true;
    }

    inline void push(const T &value)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryPush(value))
        {
            backoff.pause();
        }
    }

    inline void push(T &&value)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryPush(std::move(value)))
        {
            backoff.pause();
        }
    }

    inline bool tryPop(size_t consumerId, T &out)
    {
        if (consumerId >= _consumerCount)
        {
            return false;
        }

        const size_t read = _readSeqs[consumerId].value.load(std::memory_order_relaxed);
        if (read >= _writeSeq.load(std::memory_order_acquire))
        {
            return false;
        }

        Slot &slot = _buf[read & _mask];
        if (slot.sequence.load(std::memory_order_acquire) != read)
        {
            return false;
        }

        out = slot.data;
        _readSeqs[consumerId].value.store(read + 1, std::memory_order_release);
        return true;
    }

    inline void pop(size_t consumerId, T &out)
    {
        ringbuffer_detail::AdaptiveBackoff backoff;
        while (!tryPop(consumerId, out))
        {
            backoff.pause();
        }
    }
};

#endif