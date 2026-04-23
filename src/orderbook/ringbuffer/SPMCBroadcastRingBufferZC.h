#ifndef MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_SPMC_BROADCAST_H
#define MARKETDATA_ORDERBOOK_RING_BUFFER_ZERO_COPY_SPMC_BROADCAST_H

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
class SpmcBroadcastRingBufferZC
{
    static_assert(std::is_default_constructible<T>::value, "T must be default constructible");

public:
    struct WriteReservation
    {
        T *data = nullptr;
        size_t sequence = 0;
    };

    struct ReadReservation
    {
        const T *data = nullptr;
        size_t sequence = 0;
        size_t consumerId = std::numeric_limits<size_t>::max();
    };

private:
    struct alignas(ringbuffer_detail::CACHE_LINE_SIZE) AlignedSeq
    {
        // 每个 consumer 对外公布“自己已经消费到哪里”。
        std::atomic<size_t> value{0};
    };

    struct alignas(ringbuffer_detail::CACHE_LINE_SIZE) ConsumerCursor
    {
        // 仅由对应 consumer 线程私有访问的下一个读位置。
        size_t nextRead = 0;
    };

    size_t _cap = 0;
    size_t _mask = 0;
    size_t _consumerCount = 0;
    std::unique_ptr<T[]> _buf;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) std::atomic<size_t> _writeSeq;
    std::unique_ptr<AlignedSeq[]> _readSeqs;
    std::unique_ptr<ConsumerCursor[]> _consumerCursors;
    // producer 侧缓存：
    // _producerWriteSeqCache 保存下一次准备写入的逻辑序号；
    // _producerMinReadCache 保存上次扫描出的最慢 consumer 位置。
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) size_t _producerWriteSeqCache = 0;
    alignas(ringbuffer_detail::CACHE_LINE_SIZE) size_t _producerMinReadCache = 0;

    inline size_t minReadSeq() const
    {
        // 广播队列何时能覆盖旧槽位，取决于最慢那个 consumer。
        size_t minSeq = _readSeqs[0].value.load(std::memory_order_relaxed);
        for (size_t i = 1; i < _consumerCount; ++i)
        {
            const size_t seq = _readSeqs[i].value.load(std::memory_order_relaxed);
            if (seq < minSeq)
            {
                minSeq = seq;
            }
        }
        return minSeq;
    }

public:
    explicit SpmcBroadcastRingBufferZC(size_t cap = 4096, size_t consumerCount = 1)
        : _cap(ringbuffer_detail::round2(cap)),
          _mask(_cap - 1),
          _consumerCount(std::max<size_t>(1, consumerCount)),
          _buf(std::make_unique<T[]>(_cap)),
          _writeSeq(0),
          _readSeqs(std::make_unique<AlignedSeq[]>(_consumerCount)),
          _consumerCursors(std::make_unique<ConsumerCursor[]>(_consumerCount))
    {
    }

    SpmcBroadcastRingBufferZC(const SpmcBroadcastRingBufferZC &) = delete;
    SpmcBroadcastRingBufferZC &operator=(const SpmcBroadcastRingBufferZC &) = delete;

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

    inline bool tryAcquireWrite(WriteReservation &reservation)
    {
        const size_t write = _producerWriteSeqCache;
        size_t minRead = _producerMinReadCache;
        // 只有接近写满时，producer 才重新扫描所有 consumer 的 read 序号。
        if (write - minRead >= _cap)
        {
            minRead = minReadSeq();
            _producerMinReadCache = minRead;
            if (write - minRead >= _cap)
            {
                return false;
            }
        }

        reservation.data = &_buf[write & _mask];
        reservation.sequence = write;
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

        const size_t write = reservation.sequence;
        _producerWriteSeqCache = write + 1;
        // 广播队列只需发布一个单调递增的 writeSeq，consumer 按各自 readSeq 顺序读取。
        _writeSeq.store(write + 1, std::memory_order_release);
        reservation.data = nullptr;
    }

    inline bool tryAcquireRead(size_t consumerId, ReadReservation &reservation)
    {
        if (consumerId >= _consumerCount)
        {
            return false;
        }

        ConsumerCursor &cursor = _consumerCursors[consumerId];
        const size_t read = cursor.nextRead;
        // 当前 consumer 只要确认“自己下一条要读的序号”已经被 producer 发布即可。
        if (read >= _writeSeq.load(std::memory_order_acquire))
        {
            return false;
        }

        reservation.data = &_buf[read & _mask];
        reservation.sequence = read;
        reservation.consumerId = consumerId;
        return true;
    }

    inline const T *acquireRead(size_t consumerId, ReadReservation &reservation)
    {
        while (!tryAcquireRead(consumerId, reservation))
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

        ConsumerCursor &cursor = _consumerCursors[reservation.consumerId];
        cursor.nextRead = reservation.sequence + 1;
        // 把消费进度公布给 producer，供后续计算最慢 consumer。
        _readSeqs[reservation.consumerId].value.store(
            reservation.sequence + 1, std::memory_order_release);
        reservation.data = nullptr;
    }
};

#endif