#ifndef _SHM_DETAIL_H
#define _SHM_DETAIL_H

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/*
 * POSIX 共享内存 ringbuffer 的底层工具。
 *
 * 设计思路：
 * 1. Writer 负责创建并初始化共享内存对象，Reader 只打开已经存在的对象。
 * 2. 共享内存布局固定为 ControlBlock + N 个等长 Slot，所有偏移按缓存行对齐，
 *    减少跨进程轮询时的 false sharing。
 * 3. ControlBlock 保存只读元信息和全局写入票号 tail；Reader 的读票号保存在
 *    自己进程内，因此多个 Reader 可以互不影响地消费同一条数据流。
 * 4. Slot::sequence 是每个槽位的发布标记：偶数表示该 ticket 正在写入，奇数表示
 *    该 ticket 已经写完可读。Reader 读前读后各检查一次 sequence，用来发现被覆盖。
 * 5. 该实现不做阻塞式背压；Writer 永远向前写，慢 Reader 通过 readLeftBehind 感知丢数。
 */
namespace shm_detail
{
    // Writer 完成 ControlBlock 和所有 Slot 初始化后写入该状态，Reader 看到它才开始校验布局。
    constexpr uint32_t SHM_SPSC_READY = 1;

    // 判断容量或缓存行参数是否为 2 的幂。2^n 容量可以用 ticket & mask 快速取模。
    constexpr bool isPowerOfTwo(size_t value)
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    // 将 value 向上对齐到 alignment 的整数倍，用于控制块和槽位的共享内存偏移计算。
    constexpr size_t alignUp(size_t value, size_t alignment)
    {
        return ((value + alignment - 1) / alignment) * alignment;
    }

    // constexpr 版本的 max，避免为了一个很小的编译期计算额外依赖算法库调用。
    constexpr size_t maxSize(size_t lhs, size_t rhs)
    {
        return lhs > rhs ? lhs : rhs;
    }

    // POSIX shm_open 要求名字以 '/' 开头；调用方可以传 "xxx" 或 "/xxx"，这里统一规整。
    inline std::string normalizeName(const std::string &name)
    {
        if (name.empty())
        {
            throw std::invalid_argument("shm name must not be empty");
        }
        return name[0] == '/' ? name : "/" + name;
    }

    // 将 errno 转成 system_error，并把失败的系统调用和 shm 名字放进错误信息。
    [[noreturn]] inline void throwSystemError(const char *op, const std::string &name)
    {
        throw std::system_error(errno, std::generic_category(), std::string(op) + ": " + name);
    }

    /*
     * 共享内存头部。
     *
     * state/capacity/mask/slotSize 是初始化和兼容性校验用的元信息。
     * tail 是全局写票号，也就是下一条将被写入的数据序号；它单独按 64 字节对齐，
     * 避免 Writer 高频更新 tail 时和只读元信息落在同一条缓存行。
     */
    struct alignas(64) ControlBlock
    {
        std::atomic<uint32_t> state{0};
        uint32_t capacity = 0;
        uint32_t mask = 0;
        uint32_t slotSize = 0;
        alignas(64) std::atomic<uint64_t> tail{0};
    };

    static_assert(alignof(ControlBlock) == 64);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);

    /*
     * 单个环形槽位。
     *
     * sequence 的编码：
     * - ticket << 1：Writer 已占用该槽位，正在写 data。
     * - (ticket << 1) | 1：Writer 已完成 data 拷贝，Reader 可以读取。
     *
     * T 被要求 trivially copyable，因此跨进程传输只做 memcpy，不依赖构造/析构语义。
     */
    template<typename T>
    struct alignas(64) Slot
    {
        std::atomic<uint64_t> sequence{0};
        T data{};
    };

    /*
     * POSIX 共享内存映射的 RAII 包装。
     *
     * MappedRegion 只表达“当前对象拥有 fd + mmap 地址 + 可选 unlink 权限”。
     * 它不可拷贝，避免两个对象重复 munmap/close；可以 move，便于 create/open
     * 返回临时对象后转移给 ShmWriter/ShmReader 的成员变量。
     */
    class MappedRegion
    {
    public:
        // 空对象表示尚未打开任何共享内存，reset() 对空对象是安全的。
        MappedRegion() = default;

        // 接管已经 mmap 成功的 fd/addr；外部不再直接 close 或 munmap。
        MappedRegion(int fd, void *addr, size_t bytes, std::string name, bool unlinkOnDestroy)
            : _fd(fd),
              _addr(addr),
              _bytes(bytes),
              _name(std::move(name)),
              _unlinkOnDestroy(unlinkOnDestroy) {}
        // 复制构造
        MappedRegion(const MappedRegion &) = delete;
        // 复制赋值
        MappedRegion &operator=(const MappedRegion &) = delete;
        // 移动构造
        MappedRegion(MappedRegion &&other) noexcept :
            _fd(std::exchange(other._fd, -1)), 
            _addr(std::exchange(other._addr, nullptr)),
            _bytes(std::exchange(other._bytes, 0)),
            _name(std::move(other._name)),
            _unlinkOnDestroy(std::exchange(other._unlinkOnDestroy, false)) {};
        // 移动赋值
        MappedRegion &operator=(MappedRegion &&other) noexcept
        {
            if (&other != this)
            {
                reset();
                this->_fd = std::exchange(other._fd, -1);
                this->_bytes = std::exchange(other._bytes, 0);
                this->_addr = std::exchange(other._addr, nullptr);
                this->_name = other._name;
                this->_unlinkOnDestroy = std::exchange(other._unlinkOnDestroy, false);
            }
            return *this;
        }

        ~MappedRegion()
        {
            reset();
        }

        // 返回 mmap 起始地址。调用方根据约定布局自行解释 ControlBlock 和 Slot。
        void *addr() const
        {
            return _addr;
        }

        // 返回映射字节数，用于 Reader 校验共享内存对象是否完整。
        size_t bytes() const
        {
            return _bytes;
        }

        // 返回 normalize 后的 POSIX shm 名字，便于错误信息和诊断输出。
        const std::string &name() const
        {
            return _name;
        }

        // 释放当前映射。析构、close 和移动赋值都会走这里；所有系统调用失败都被忽略。
        void reset() noexcept
        {
            if (_addr != nullptr)
            {
                (void)::munmap(_addr, _bytes);
            }
            if (_fd >= 0)
            {
                (void)::close(_fd);
            }
            if (_unlinkOnDestroy && !_name.empty())
            {
                (void)::shm_unlink(_name.c_str());
            }

            _fd = -1;
            _addr = nullptr;
            _bytes = 0;
            _name.clear();
            _unlinkOnDestroy = false;
        }

        /*
         * 创建新的共享内存对象并 mmap。
         *
         * unlinkBeforeCreate 默认打开，用来清理上一次异常退出留下的同名对象；
         * O_EXCL 保证不会误接到一个布局未知的旧对象。成功后只返回映射，不初始化业务布局，
         * 具体 ControlBlock/Slot 初始化由 ShmWriter::create 完成。
         */
        static MappedRegion create(const std::string &inputName,
                                   size_t bytes,
                                   mode_t mode = 0600,
                                   bool unlinkBeforeCreate = true,
                                   bool unlinkOnDestroy = false)
        {
            const std::string name = normalizeName(inputName);
            // 先unlink再创建POSIX shm
            if (unlinkBeforeCreate)
            {
                (void)::shm_unlink(name.c_str());
            }

            int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, mode);
            if (fd < 0)
            {
                throwSystemError("shm_open create", name);
            }

            if (ftruncate(fd, static_cast<off_t>(bytes)) != 0)
            {
                (void)::close(fd);
                (void)::shm_unlink(name.c_str());
                throwSystemError("ftruncate", name);
            }

            return mapShared(fd, bytes, name, unlinkOnDestroy, true);
        }

        /*
         * 打开已有共享内存对象并 mmap。
         *
         * Reader 走这个入口。这里只校验对象存在且大小非空，协议兼容性由 ShmReader
         * 在看到 READY 后检查 capacity/mask/slotSize。
         */
        static MappedRegion open(const std::string &inputName, bool unlinkOnDestroy = false)
        {
            const std::string name = normalizeName(inputName);
            int fd = ::shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
            if (fd < 0)
            {
                throwSystemError("shm_open open", name);
            }

            struct stat st{};
            if (fstat(fd, &st) != 0)
            {
                (void)::close(fd);
                throwSystemError("fstat", name);
            }
            if (st.st_size <= 0)
            {
                (void)::close(fd);
                throw std::runtime_error("shared memory object is empty: " + name);
            }

            return mapShared(fd, static_cast<size_t>(st.st_size), name, unlinkOnDestroy, false);
        }

    private:
        int _fd = -1;
        void *_addr = nullptr;
        size_t _bytes = 0;
        std::string _name;
        bool _unlinkOnDestroy = false;

        // 对 fd 做 MAP_SHARED 映射。失败时关闭 fd，并按创建路径需要决定是否 unlink。
        static MappedRegion mapShared(int fd,
                                size_t bytes,
                                const std::string &name,
                                bool unlinkOnDestroy = false,
                                bool unlinkOnFailure = false)
        {
            void *addr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (addr == MAP_FAILED)
            {
                (void)::close(fd);
                if (unlinkOnFailure)
                {
                    (void)::shm_unlink(name.c_str());
                }
                throwSystemError("mmap", name);
            }

            return MappedRegion(fd, addr, bytes, name, unlinkOnDestroy);
        }
    };
}

enum class ShmReadStatus : int32_t
{
    readable = 0,
    nothingToRead = -1,
    readLeftBehind = -2,
};

#endif
