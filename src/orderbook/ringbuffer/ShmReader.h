#ifndef _SHM_READER_H
#define _SHM_READER_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "detail.h"
#include "ShmDetail.h"

/*
 * 共享内存环形队列读端。
 *
 * Reader 只保存自己的 _readTicket，不回写共享 head，因此多个 Reader 可以同时读取同一个
 * Writer 发布的数据流，彼此没有同步和背压关系。代价是 Writer 不知道谁还没读，槽位会按
 * 环形容量被覆盖；Reader 落后超过 capacity 时返回 readLeftBehind。
 *
 * 读取协议：
 * 1. 读取 tail，确认 Writer 已经发布到哪个 ticket。
 * 2. 如果 _readTicket == tail，说明当前没有新数据。
 * 3. 如果 tail - _readTicket > capacity，说明目标 ticket 必然被覆盖。
 * 4. 读取 slot.sequence，必须等于 (_readTicket << 1) | 1。
 * 5. memcpy 复制 data。
 * 6. 再读一次 slot.sequence，确认复制期间槽位没有被 Writer 覆盖。
 *
 * tryPopStatus 保留状态码，调用方可以自行处理空读和落后；pop 则偏向“持续读最新流”，
 * 发现落后后直接跳到当前 tail。
 */
template<typename T, size_t Cacheline = 64>
class ShmReader
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable for cross-process shm transport");
    static_assert(std::is_default_constructible_v<T>,
                  "T must be default constructible for shm slot initialization");
    static_assert(shm_detail::isPowerOfTwo(Cacheline),
                  "Cacheline must be a power of two");

private:
    using ControlBlock = shm_detail::ControlBlock;
    using Slot = shm_detail::Slot<T>;

    static constexpr size_t kCacheline = shm_detail::maxSize(Cacheline, alignof(Slot));
    static constexpr size_t kSlotSize = shm_detail::alignUp(sizeof(Slot), kCacheline);
    static constexpr size_t kSlotsOffset = shm_detail::alignUp(sizeof(ControlBlock), kCacheline);
    static_assert(kSlotSize <= std::numeric_limits<uint32_t>::max(),
                  "shm slot size is too large");

    shm_detail::MappedRegion _region;
    ControlBlock *_control = nullptr;
    char *_slots = nullptr;
    uint64_t _readTicket = 0;
    uint32_t _capacity = 0;
    uint32_t _mask = 0;

public:
    ShmReader() = default;

    // 构造时直接打开已有共享内存对象，等价于默认构造后调用 open(name)。
    explicit ShmReader(const std::string &name)
    {
        open(name);
    }
    ShmReader(const ShmReader &) = delete;
    ShmReader &operator=(const ShmReader &) = delete;
    ShmReader(ShmReader &&other) = delete;
    ShmReader &operator=(ShmReader &&other) = delete;

    ~ShmReader()
    {
        close();
    }

    /*
     * 打开 Writer 已创建的共享内存 ringbuffer。
     *
     * open 只连接现有对象，不负责创建。函数会等待 ControlBlock::state 变成 READY，
     * 然后校验容量、mask、slotSize 和映射大小，避免 Reader 以错误的 T 或 Cacheline
     * 解释共享内存。打开成功后默认 resetToTail()，即从“打开后的新数据”开始读，
     * 不回放打开前已经写入的历史槽位。
     */
    void open(const std::string &name)
    {
        close();

        _region = shm_detail::MappedRegion::open(name);
        if (_region.bytes() < kSlotsOffset)
        {
            throw std::runtime_error("shared memory object is too small: " + _region.name());
        }

        _control = static_cast<ControlBlock *>(_region.addr());
        while (_control->state.load(std::memory_order_acquire) != shm_detail::SHM_SPSC_READY)
        {
            ringbuffer_detail::cpuRelax();
        }

        _capacity = _control->capacity;
        _mask = _control->mask;
        if (!shm_detail::isPowerOfTwo(_capacity) ||
            _capacity < 2 ||
            _mask != _capacity - 1 ||
            _control->slotSize != kSlotSize)
        {
            close();
            throw std::runtime_error("shared memory object is not a compatible shm: " + _region.name());
        }

        const size_t expectedBytes = kSlotsOffset + static_cast<size_t>(_capacity) * kSlotSize;
        if (_region.bytes() < expectedBytes)
        {
            close();
            throw std::runtime_error("shared memory object is truncated: " + _region.name());
        }

        _slots = static_cast<char *>(_region.addr()) + kSlotsOffset;
        resetToTail();
    }

    // 解除本进程映射并清空本地读状态；不会删除共享内存名字。
    void close() noexcept
    {
        _control = nullptr;
        _slots = nullptr;
        _readTicket = 0;
        _capacity = 0;
        _mask = 0;
        _region.reset();
    }

    // 当前对象是否已经成功打开共享内存。
    bool isOpen() const
    {
        return _control != nullptr;
    }

    // 返回 normalize 后的 shm 名字，未打开时为空字符串。
    const std::string &name() const
    {
        return _region.name();
    }

    // 返回 Writer 初始化时确定的实际容量。
    size_t capacity() const
    {
        return _capacity;
    }

    // 返回 Writer 已发布到的全局票号，也就是下一条将被写入的 ticket。
    uint64_t tail() const
    {
        assert(_control != nullptr);
        return _control->tail.load(std::memory_order_acquire);
    }

    // 返回本 Reader 下一次准备读取的 ticket。该值只在当前进程内维护。
    uint64_t readTicket() const
    {
        return _readTicket;
    }

    // 估算当前可读记录数；如果 Writer 同时继续写入，这个值只适合做瞬时参考。
    size_t pending() const
    {
        const uint64_t writeTicket = tail();
        return writeTicket > _readTicket ? static_cast<size_t>(writeTicket - _readTicket) : 0;
    }

    // pending() == 0 的便捷判断。
    bool empty() const
    {
        return pending() == 0;
    }

    // 放弃当前积压数据，把下一次读取位置跳到最新 tail。
    void resetToTail()
    {
        _readTicket = tail();
    }

    /*
     * 非阻塞读取一条记录，并返回详细状态。
     *
     * 返回值：
     * - readable：成功复制 out，_readTicket 前进 1。
     * - nothingToRead：当前没有新 ticket，out 不保证被修改。
     * - readLeftBehind：Reader 太慢或复制期间槽位被覆盖，调用方应决定是否 resetToTail。
     *
     * 两次 sequence 检查是为了覆盖两类竞态：
     * - 读前 sequence 不匹配，说明目标 ticket 已不在该槽位；
     * - memcpy 后 sequence 改变，说明复制过程中 Writer 正好覆盖了该槽位。
     */
    ShmReadStatus tryPopStatus(T &out)
    {
        assert(_control != nullptr);
        const uint64_t writeTicket = _control->tail.load(std::memory_order_acquire);
        if (_readTicket == writeTicket)
        {
            return ShmReadStatus::nothingToRead;
        }

        if (writeTicket - _readTicket > _capacity)
        {
            return ShmReadStatus::readLeftBehind;
        }

        Slot *slot = reinterpret_cast<Slot *>(_slots + ((_readTicket & _mask) * kSlotSize));
        const uint64_t expectedSequence = (_readTicket << 1) | 1ULL;
        if (slot->sequence.load(std::memory_order_acquire) != expectedSequence)
        {
            return ShmReadStatus::readLeftBehind;
        }

        std::memcpy(&out, &slot->data, sizeof(T));
        if (slot->sequence.load(std::memory_order_acquire) != expectedSequence)
        {
            return ShmReadStatus::readLeftBehind;
        }

        ++_readTicket;
        return ShmReadStatus::readable;
    }

    // 兼容普通队列接口：只关心是否读到数据，不暴露空读/落后原因。
    bool tryPop(T &out)
    {
        return tryPopStatus(out) == ShmReadStatus::readable;
    }

    /*
     * 阻塞等待直到不再是 nothingToRead。
     *
     * 该函数只在“暂时没数据”时自旋；如果检测到 readLeftBehind 会立即返回，
     * 让上层决定是报错、统计丢数还是跳到最新 tail。
     */
    ShmReadStatus popStatus(T &out)
    {
        ShmReadStatus status;
        while ((status = tryPopStatus(out)) == ShmReadStatus::nothingToRead)
        {
            ringbuffer_detail::cpuRelax();
        }
        return status;
    }

    /*
     * 阻塞读取一条可用记录。
     *
     * 如果等待过程中发现 Reader 落后，会 resetToTail() 丢弃积压数据并继续等下一条。
     * 这个接口适合只关心最新连续流、不希望上层处理丢数状态的场景。
     */
    void pop(T &out)
    {
        while (popStatus(out) != ShmReadStatus::readable)
        {
            resetToTail();
        }
    }

    // 兼容部分 ringbuffer 命名习惯，语义等同 tryPop(out)。
    bool pop_front(T &out)
    {
        return tryPop(out);
    }
};

#endif
