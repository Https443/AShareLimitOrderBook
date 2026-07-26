#pragma once
#include <cmath>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <utility>
#include "NodePool.h"
#include "util/StdException.h"

namespace marketdata
{
namespace orderbook
{
    // =====================
    // 高性能价格簿
    // Descending=true  : 大价优先（典型买盘）
    // Descending=false : 小价优先（典型卖盘）
    // 价格档位由 book 内部按 tick 下标管理，OrderNode 通过 PriceLevel* 关联档位。
    // =====================
    template<bool Descending>
    class PriceLevelBook
    {
    public:
        PriceLevelBook(int64_t minPrice, int64_t maxPrice, int64_t tickSize, std::string code)
            : m_minPrice(minPrice),
            m_maxPrice(maxPrice),
            m_tickSize(tickSize),
            m_code(code)
        {
            if (m_tickSize <= 0)
            {
                throw std::invalid_argument("tick_size must be > 0");
            }
            if (m_minPrice > m_maxPrice)
            {
                throw std::invalid_argument("min_price must be <= max_price");
            }

            const int64_t span = m_maxPrice - m_minPrice;
            if (span % m_tickSize != 0)
            {
                throw std::invalid_argument("price range must align with tick_size");
            }

            m_tickCount = static_cast<int32_t>(span / m_tickSize) + 1;
            m_levels.assign(static_cast<size_t>(m_tickCount), {});
            m_bitmap.assign(static_cast<size_t>((m_tickCount + 63) >> 6), 0ULL);

            m_bestTick = kInvalidTick;
            m_cageTick = kInvalidTick;
            m_activeCount = 0;
        }

        PriceLevelBook() = delete;
        ~PriceLevelBook() = default;

        PriceLevelBook(const PriceLevelBook&) = default;
        PriceLevelBook& operator=(const PriceLevelBook&) = default;
        PriceLevelBook(PriceLevelBook&&) noexcept = default;
        PriceLevelBook& operator=(PriceLevelBook&&) noexcept = default;

    public:
        // ========= 基本属性 =========

        inline bool empty() const noexcept
        {
            return m_activeCount == 0;
        }

        inline size_t size() const noexcept
        {
            return static_cast<size_t>(m_activeCount);
        }

        // ========= 最优价 =========

        inline int32_t bestTick() const noexcept
        {
            return m_bestTick;
        }

        inline int64_t bestPrice() const noexcept
        {
            return m_bestTick == kInvalidTick ? 0 : tickToPrice(m_bestTick);
        }

        // ========= 价格笼子起点 =========

        inline int32_t cageTick() const noexcept
        {
            return m_cageTick;
        }

        inline int64_t cagePrice() const noexcept
        {
            return m_cageTick == kInvalidTick ? 0 : tickToPrice(m_cageTick);
        }

        inline bool hasCageTick() const noexcept
        {
            return m_cageTick != kInvalidTick;
        }

        // ========= 查找 / 创建 =========

        // 返回（存在则直接给指针；不存在则创建并返回）
        inline PriceLevel* getOrCreate(int64_t price)
        {
            const int32_t tick = priceToTickChecked(price);

            if (!testBit(tick))
            {
                PriceLevel* level = acquireLevel(tick);
                if (level == nullptr)
                {
                    return nullptr;
                }
                setBit(tick);
                ++m_activeCount;
                updateBestOnInsert(tick);
            }

            return &m_levels[static_cast<size_t>(tick)];
        }

        // 不存在返回 nullptr
        inline PriceLevel* find(int64_t price) noexcept
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                return nullptr;
            }

            return testBit(tick) ? &m_levels[static_cast<size_t>(tick)] : nullptr;
        }

        inline const PriceLevel* find(int64_t price) const noexcept
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                return nullptr;
            }

            return testBit(tick) ? &m_levels[static_cast<size_t>(tick)] : nullptr;
        }

        inline PriceLevel* findByTick(int32_t tick) noexcept
        {
            if (!isValidTick(tick))
            {
                return nullptr;
            }
            return testBit(tick) ? &m_levels[static_cast<size_t>(tick)] : nullptr;
        }

        inline const PriceLevel* findByTick(int32_t tick) const noexcept
        {
            if (!isValidTick(tick))
            {
                return nullptr;
            }
            return testBit(tick) ? &m_levels[static_cast<size_t>(tick)] : nullptr;
        }

        // ========= 删除 =========

        inline void erase(int64_t price) noexcept
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                return;
            }
            eraseByTick(tick);
        }

        inline void eraseByTick(int32_t tick) noexcept
        {
            if (!isValidTick(tick) || !testBit(tick))
            {
                return;
            }

            releaseLevel(tick);
            clearBit(tick);
            --m_activeCount;

            if (tick == m_bestTick)
            {
                recomputeBestAfterErase(tick);
            }
            if (tick == m_cageTick)
            {
                recomputeCageAfterErase(tick);
            }
        }

        // 外部修改完 level 后，如果 volume<=0，调用这个
        inline void eraseIfEmpty(int64_t price) noexcept
        {
            PriceLevel* level = find(price);
            if (level != nullptr && level->totalVolume <= 0)
            {
                erase(price);
            }
        }

        inline void eraseIfEmptyByTick(int32_t tick) noexcept
        {
            PriceLevel* level = findByTick(tick);
            if (level != nullptr && level->totalVolume <= 0)
            {
                eraseByTick(tick);
            }
        }

        // ========= 清空 =========

        inline void clear() noexcept
        {
            for (int32_t tick = 0; tick < m_tickCount; ++tick)
            {
                if (m_levels[static_cast<size_t>(tick)].isUse)
                {
                    releaseLevel(tick);
                }
            }
            std::fill(m_bitmap.begin(), m_bitmap.end(), 0ULL);
            m_bestTick = kInvalidTick;
            m_cageTick = kInvalidTick;
            m_activeCount = 0;
        }

        // ========= TopN =========
        // 更适合热路径：调用方提供 buffer，避免分配
        inline int topNToBuffer(int n, std::pair<int64_t, const PriceLevel*>* out) const noexcept
        {
            if (n <= 0 || out == nullptr || m_activeCount == 0)
            {
                return 0;
            }

            int count = 0;
            int32_t tick = m_bestTick;

            while (tick != kInvalidTick && count < n)
            {
                const PriceLevel* level = &m_levels[static_cast<size_t>(tick)];
                out[count++] = {tickToPrice(tick), level};
                tick = nextActiveTick(tick);
            }

            return count;
        }

        // ========= All =========

        inline int allToBuffer(std::pair<int64_t, const PriceLevel*>* out) const noexcept
        {
            if (out == nullptr || m_activeCount == 0)
            {
                return 0;
            }

            int count = 0;
            int32_t tick = firstBookActiveTick();

            while (tick != kInvalidTick && count < size())
            {
                out[count++] = {tickToPrice(tick), &m_levels[static_cast<size_t>(tick)]};
                tick = nextActiveTick(tick);
            }

            return count;
        }

        // ========= 价格笼子遍历 =========
        // 价格笼子的遍历方向与 PriceLevelBook 的常规方向相反：
        // Descending=true 时从低价向高价找，Descending=false 时从高价向低价找。

        template<typename InCage>
        inline bool refreshBestByCage(InCage&& inCage) noexcept
        {
            const int32_t oldBestTick = m_bestTick;
            const int32_t oldCageTick = m_cageTick;

            m_bestTick = kInvalidTick;
            m_cageTick = kInvalidTick;

            int32_t tick = firstBookActiveTick();
            while (tick != kInvalidTick)
            {
                const int64_t price = tickToPrice(tick);
                if (inCage(price))
                {
                    m_bestTick = tick;
                    break;
                }

                // 记录最靠近笼内边界的笼外档位，后续按反方向继续遍历笼外区域。
                m_cageTick = tick;
                tick = nextActiveTick(tick);
            }

            return oldBestTick != m_bestTick || oldCageTick != m_cageTick;
        }

        inline bool clearCage() noexcept
        {
            const int32_t oldBestTick = m_bestTick;
            const int32_t oldCageTick = m_cageTick;

            m_bestTick = firstBookActiveTick();
            m_cageTick = kInvalidTick;

            return oldBestTick != m_bestTick || oldCageTick != m_cageTick;
        }

        // ========= 打印 =========

        inline std::string printPriceVolume(int64_t id, size_t count = 0) const
        {
            std::ostringstream ss;
            ss << "id:" << id;

            if (count == 0)
            {
                count = size();
            }

            int limit = 1;
            std::vector<std::pair<int64_t, const PriceLevel*>> vec(static_cast<int>(count));
            topNToBuffer(count, vec.data());
            for (const auto& item : vec)
            {
                ss << " " << limit << "=" << item.first << "/" << item.second->totalVolume;
                ++limit;
            }
            return ss.str();
        }

        // ========= 价格 / tick 转换 =========

        inline bool priceToTickNoThrow(int64_t price, int32_t& tick) const noexcept
        {
            if (price < m_minPrice || price > m_maxPrice)
            {
                return false;
            }

            const int64_t diff = price - m_minPrice;
            if (diff % m_tickSize != 0)
            {
                return false;
            }

            tick = static_cast<int32_t>(diff / m_tickSize);
            return true;
        }

        inline int32_t priceToTickChecked(int64_t price) const
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                STDTHROW(STD_ERROR_CODE,"price out of range or not aligned with tick_size, price:"<<price<<" min:"<<m_minPrice<<" max:"<<m_maxPrice<<" code:"<<m_code, "price out of range or not aligned with tick_size, price:"<<price<<" min:"<<m_minPrice<<" max:"<<m_maxPrice<<" code:"<<m_code);
            }
            return tick;
        }

        inline int64_t tickToPrice(int32_t tick) const noexcept
        {
            return m_minPrice + static_cast<int64_t>(tick) * m_tickSize;
        }

        // ========= 邻接有效档位 =========

        inline int32_t nextActiveTick(int32_t currentTick) const noexcept
        {
            if constexpr (Descending)
            {
                return findPrevSetBit(currentTick - 1);
            }
            else
            {
                return findNextSetBit(currentTick + 1);
            }
        }

        inline int32_t prevActiveTick(int32_t currentTick) const noexcept
        {
            if constexpr (Descending)
            {
                return findNextSetBit(currentTick + 1);
            }
            else
            {
                return findPrevSetBit(currentTick - 1);
            }
        }

        inline int32_t firstCageActiveTick() const noexcept
        {
            return m_cageTick;
        }

        inline int32_t nextCageActiveTick(int32_t currentTick) const noexcept
        {
            if constexpr (Descending)
            {
                return findNextSetBit(currentTick + 1);
            }
            else
            {
                return findPrevSetBit(currentTick - 1);
            }
        }

        inline int32_t prevCageActiveTick(int32_t currentTick) const noexcept
        {
            if constexpr (Descending)
            {
                return findPrevSetBit(currentTick - 1);
            }
            else
            {
                return findNextSetBit(currentTick + 1);
            }
        }

        inline int32_t lastTick(int32_t tick) const noexcept
        {
            if (testBit(tick))
            {
                return tick;
            }
            return nextActiveTick(tick);
        }

    private:
        static constexpr int32_t kInvalidTick = -1;

        inline PriceLevel* acquireLevel(int32_t tick)
        {
            PriceLevel* level = &m_levels[static_cast<size_t>(tick)];
            level->reset();
            level->isUse = true;
            level->price = tickToPrice(tick);
            return level;
        }

        inline void releaseLevel(int32_t tick) noexcept
        {
            if (!isValidTick(tick))
            {
                return;
            }
            PriceLevel* level = &m_levels[static_cast<size_t>(tick)];
            level->reset();
        }

        inline bool isValidTick(int32_t tick) const noexcept
        {
            return tick >= 0 && tick < m_tickCount;
        }

        inline bool testBit(int32_t tick) const noexcept
        {
            const uint64_t word = m_bitmap[static_cast<size_t>(tick >> 6)];
            return ((word >> (tick & 63)) & 1ULL) != 0;
        }

        inline void setBit(int32_t tick) noexcept
        {
            m_bitmap[static_cast<size_t>(tick >> 6)] |= (1ULL << (tick & 63));
        }

        inline void clearBit(int32_t tick) noexcept
        {
            m_bitmap[static_cast<size_t>(tick >> 6)] &= ~(1ULL << (tick & 63));
        }

        inline void updateBestOnInsert(int32_t tick) noexcept
        {
            if (m_bestTick == kInvalidTick)
            {
                m_bestTick = tick;
                return;
            }

            if constexpr (Descending)
            {
                if (tick > m_bestTick)
                {
                    m_bestTick = tick;
                }
            }
            else
            {
                if (tick < m_bestTick)
                {
                    m_bestTick = tick;
                }
            }
        }

        inline int32_t firstBookActiveTick() const noexcept
        {
            if constexpr (Descending)
            {
                return findPrevSetBit(m_tickCount - 1);
            }
            else
            {
                return findNextSetBit(0);
            }
        }

        inline void recomputeBestAfterErase(int32_t erasedTick) noexcept
        {
            if (m_activeCount == 0)
            {
                m_bestTick = kInvalidTick;
                return;
            }

            if constexpr (Descending)
            {
                m_bestTick = findPrevSetBit(erasedTick - 1);
            }
            else
            {
                m_bestTick = findNextSetBit(erasedTick + 1);
            }
        }

        inline void recomputeCageAfterErase(int32_t erasedTick) noexcept
        {
            if (m_activeCount == 0)
            {
                m_cageTick = kInvalidTick;
                return;
            }

            if constexpr (Descending)
            {
                m_cageTick = findNextSetBit(erasedTick + 1);
            }
            else
            {
                m_cageTick = findPrevSetBit(erasedTick - 1);
            }
        }

        // 找 >= start_tick 的第一个有效 tick
        inline int32_t findNextSetBit(int32_t startTick) const noexcept
        {
            if (startTick < 0)
            {
                startTick = 0;
            }
            if (startTick >= m_tickCount)
            {
                return kInvalidTick;
            }

            size_t wordIdx = static_cast<size_t>(startTick >> 6);
            const int bitIdx = startTick & 63;

            // 先处理起始 word
            {
                uint64_t word = m_bitmap[wordIdx];
                word &= (~0ULL << bitIdx);
                if (word != 0)
                {
                    const int offset = ctz64(word);
                    const int32_t tick = static_cast<int32_t>((wordIdx << 6) + static_cast<size_t>(offset));
                    return tick < m_tickCount ? tick : kInvalidTick;
                }
            }

            // 后续完整 word
            const size_t wordCount = m_bitmap.size();
            for (++wordIdx; wordIdx < wordCount; ++wordIdx)
            {
                const uint64_t word = m_bitmap[wordIdx];
                if (word != 0)
                {
                    const int offset = ctz64(word);
                    const int32_t tick = static_cast<int32_t>((wordIdx << 6) + static_cast<size_t>(offset));
                    return tick < m_tickCount ? tick : kInvalidTick;
                }
            }

            return kInvalidTick;
        }

        // 找 <= start_tick 的第一个有效 tick
        inline int32_t findPrevSetBit(int32_t startTick) const noexcept
        {
            if (startTick >= m_tickCount)
            {
                startTick = m_tickCount - 1;
            }
            if (startTick < 0)
            {
                return kInvalidTick;
            }

            size_t wordIdx = static_cast<size_t>(startTick >> 6);
            const int bitIdx = startTick & 63;

            // 先处理起始 word
            {
                uint64_t mask = (bitIdx == 63) ? ~0ULL : ((1ULL << (bitIdx + 1)) - 1ULL);
                uint64_t word = m_bitmap[wordIdx] & mask;
                if (word != 0)
                {
                    const int offset = 63 - clz64(word);
                    return static_cast<int32_t>((wordIdx << 6) + static_cast<size_t>(offset));
                }
            }

            // 前面的完整 word
            while (wordIdx > 0)
            {
                --wordIdx;
                const uint64_t word = m_bitmap[wordIdx];
                if (word != 0)
                {
                    const int offset = 63 - clz64(word);
                    return static_cast<int32_t>((wordIdx << 6) + static_cast<size_t>(offset));
                }
            }

            return kInvalidTick;
        }

        // count trailing zeros
        static inline int ctz64(uint64_t x) noexcept
        {
            return __builtin_ctzll(x);
        }

        // count leading zeros
        static inline int clz64(uint64_t x) noexcept
        {
            return __builtin_clzll(x);
        }

    private:
        int64_t m_minPrice;
        int64_t m_maxPrice;
        int64_t m_tickSize;

        int32_t m_tickCount = 0;
        int32_t m_bestTick = kInvalidTick;
        int32_t m_cageTick = kInvalidTick;
        int32_t m_activeCount = 0;

        std::vector<PriceLevel> m_levels;
        std::vector<uint64_t> m_bitmap;

        std::string m_code;
    };

    using PriceLevelBookGreat = PriceLevelBook<true>;   // 买盘：高价优先
    using PriceLevelBookLess  = PriceLevelBook<false>;  // 卖盘：低价优先
}
}
