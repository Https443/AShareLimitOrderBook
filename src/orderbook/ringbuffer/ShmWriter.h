#ifndef _SHM_WRITER_H
#define _SHM_WRITER_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <sys/mman.h>

#include "detail.h"
#include "ShmDetail.h"

/*
 * 共享内存环形队列写端。
 *
 * 适用场景：
 * - 单个 Writer 进程持续发布 trivially copyable 的定长结构体。
 * - 一个或多个 Reader 进程按自己的速度读取最新数据流。
 * - 允许慢 Reader 被覆盖；系统通过 readLeftBehind 通知丢数，而不是阻塞 Writer。
 *
 * 内存布局：
 * [ControlBlock][padding][Slot 0][padding][Slot 1]...
 *
 * 写入协议：
 * 1. tail 是下一条写入记录的全局 ticket。
 * 2. ticket & mask 定位环形槽位。
 * 3. 先把 slot.sequence 写成偶数 ticket << 1，表示该槽正在被覆盖。
 * 4. memcpy 写入 data。
 * 5. 再把 slot.sequence 写成奇数 (ticket << 1) | 1，表示数据完整发布。
 * 6. 最后发布 tail = ticket + 1，让 Reader 看到新数据。
 *
 * 这里不维护 head，也不检查剩余空间，所以写端是 wait-free 的固定成本写入。
 */
template<typename T, size_t Cacheline = 64>
class ShmWriter
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
    uint32_t _capacity = 0;
    uint32_t _mask = 0;

public:
    ShmWriter() = default;

    // 构造时直接创建共享内存对象，等价于默认构造后调用 create(name, capacity)。
    ShmWriter(const std::string &name, size_t capacity)
    {
        create(name, capacity);
    }
    ShmWriter(const ShmWriter &) = delete;
    ShmWriter &operator=(const ShmWriter &) = delete;
    ShmWriter(ShmWriter &&other) = delete;
    ShmWriter &operator=(ShmWriter &&other) = delete;

    /*
     * 创建并初始化共享内存 ringbuffer。
     *
     * requestedCapacity 会向上取整为 2 的幂，便于后续通过 ticket & mask 定位槽位。
     * 初始化顺序必须是：
     * - 创建 mmap；
     * - placement-new 构造 ControlBlock；
     * - 写入 capacity/mask/slotSize/tail；
     * - placement-new 构造所有 Slot，并把 sequence 置为无效值；
     * - release 写 state=READY。
     *
     * Reader 在 state 变成 READY 前只自旋等待，因此不会读到半初始化布局。
     */
    void create(const std::string &name, size_t requestedCapacity)
    {
        close();

        const size_t cap = ringbuffer_detail::round2(requestedCapacity);
        const size_t bytes = totalBytesForCapacity(cap);
        _region = shm_detail::MappedRegion::create(name, bytes);

        _capacity = static_cast<uint32_t>(cap);
        _mask = _capacity - 1;
        _control = new (_region.addr()) ControlBlock();
        _control->capacity = _capacity;
        _control->mask = _mask;
        _control->slotSize = static_cast<uint32_t>(kSlotSize);
        _control->tail.store(0, std::memory_order_relaxed);

        _slots = static_cast<char *>(_region.addr()) + kSlotsOffset;
        for (uint32_t i = 0; i < _capacity; ++i)
        {
            Slot *slot = new (_slots + i * kSlotSize) Slot();
            slot->sequence.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
        }

        _control->state.store(shm_detail::SHM_SPSC_READY, std::memory_order_release);
    }

    /*
     * 关闭当前 writer。
     *
     * 该函数只解除本进程的 mmap/文件描述符持有关系；默认不会 shm_unlink，
     * 因此 Reader 已经打开的映射仍然有效。需要删除名字时显式调用 unlink()。
     */
    void close() noexcept
    {
        _control = nullptr;
        _slots = nullptr;
        _capacity = 0;
        _mask = 0;
        _region.reset();
    }

    // 删除 POSIX shm 名字。已经 mmap 的进程不受影响，新进程不能再通过该名字 open。
    static void unlink(const std::string &name)
    {
        const std::string normalizedName = shm_detail::normalizeName(name);
        (void)::shm_unlink(normalizedName.c_str());
    }

    // 当前对象是否持有一个已初始化的共享内存映射。
    bool isOpen() const
    {
        return _control != nullptr;
    }

    // 返回 normalize 后的 shm 名字，未打开时为空字符串。
    const std::string &name() const
    {
        return _region.name();
    }

    // 实际容量，即 requestedCapacity 向上取整后的 2^n。
    size_t capacity() const
    {
        return _capacity;
    }

    // 返回当前写票号，也就是已经发布的记录数。acquire 与 Reader 的读取语义保持一致。
    uint64_t tail() const
    {
        assert(_control != nullptr);
        return _control->tail.load(std::memory_order_acquire);
    }

    /*
     * 发布一条记录。
     *
     * 这是核心写路径，只有单 Writer 可以调用；多 Writer 会破坏 tail 和槽位 sequence。
     * 两次 sequence.store(release) 的作用分别是：
     * - 偶数 sequence 告诉 Reader 该槽正在写，避免读到半覆盖数据；
     * - 奇数 sequence 在 data memcpy 之后发布，Reader acquire 看到奇数后才能安全复制 data。
     *
     * tail 在最后 release 发布，使 Reader 先通过 tail 知道有新 ticket，再通过 slot.sequence
     * 校验该 ticket 是否仍然完整存在。
     */
    void push(const T &value)
    {
        assert(_control != nullptr);
        const uint64_t ticket = _control->tail.load(std::memory_order_relaxed);
        Slot *slot = reinterpret_cast<Slot *>(_slots + ((ticket & _mask) * kSlotSize));

        slot->sequence.store(ticket << 1, std::memory_order_release);
        std::memcpy(&slot->data, &value, sizeof(T));
        slot->sequence.store((ticket << 1) | 1ULL, std::memory_order_release);
        _control->tail.store(ticket + 1, std::memory_order_release);
    }

    // 在栈上构造一个 T，再按 push 的 memcpy 协议发布；适合用参数直接生成消息。
    template<typename... Args>
    void emplace(Args &&... args)
    {
        T value(std::forward<Args>(args)...);
        push(value);
    }

private:
    // 按容量计算 mmap 总字节数，并防止 uint32_t 元信息和 size_t 乘法溢出。
    static size_t totalBytesForCapacity(size_t cap)
    {
        if (cap == 0 || cap > std::numeric_limits<uint32_t>::max() ||
            cap > (std::numeric_limits<size_t>::max() - kSlotsOffset) / kSlotSize)
        {
            throw std::length_error("shm capacity is too large");
        }

        return kSlotsOffset + cap * kSlotSize;
    }
};

#endif
