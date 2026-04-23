#ifndef MARKETDATA_ORDERBOOK_RINGBUFFER_DETAIL_COMMON_H
#define MARKETDATA_ORDERBOOK_RINGBUFFER_DETAIL_COMMON_H

#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <utility>

namespace ringbuffer_detail
{
    // 主流 x86_64/ARM 服务器的缓存行大小。
    constexpr size_t CACHE_LINE_SIZE = 64;

    // 容量向上取 2^n，便于通过 mask 做无分支索引。
    inline size_t round2(size_t cap)
    {
        if (cap < 2)
        {
            return 2;
        }
        --cap;
        for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
        {
            cap |= (cap >> shift);
        }
        return cap + 1;
    }

    inline void cpuRelax()
    {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
    }

    class AdaptiveBackoff
    {
    private:
        uint32_t _step = 0;

    public:
        inline void pause()
        {
            constexpr uint32_t MAX_STEP = 6;
            if (_step <= MAX_STEP)
            {
                const uint32_t spinCount = (1u << _step);
                for (uint32_t i = 0; i < spinCount; ++i)
                {
                    cpuRelax();
                }
                ++_step;
                return;
            }
            std::this_thread::yield();
        }
    };

    template<typename T>
    struct alignas(CACHE_LINE_SIZE) SlotStorageImpl
    {
        std::atomic<size_t> sequence;
        T data;

        SlotStorageImpl() : sequence(0), data()
        {
        }
    };
}
#endif