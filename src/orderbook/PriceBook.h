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

    // ====== 基于 std::map 的价格簿（支持升序/降序） ======
    template<class Comp>
    class PriceBook
    {
    public:
        PriceBook() = default;

        // 返回（存在则true）并给出level引用；不存在则创建
        inline PriceLevel* getOrCreate(int64_t price)
        {
            auto [it, inserted] = levels.try_emplace(price, PriceLevel{});
            PriceLevel& level = it->second;
            level.is_use = true;
            level.price = price;
            return &level;
        }

        inline PriceLevel* find(int64_t price)
        {
            auto it = levels.find(price);
            return (it == levels.end()) ? nullptr : &it->second;
        }

        inline bool empty() const
        {
            // cleanupBest();
            return levels.empty();
        }

        // 最优价（买：最大；卖：最小；pending 依你用的方向）
        inline int64_t bestPrice() const
        {
            // cleanupBest(); 
            return levels.empty() ? 0 : levels.begin()->first;
        }

        // 删除某价位
        inline void erase(int64_t price)
        {
            auto it = levels.find(price);
            if (it != levels.end())
            {
                levels.erase(it);
            }
        }

        inline std::vector<std::pair<int64_t, const PriceLevel*>> topN(int n) const
        {
            if (n <= 0)
            {
                return {};
            }

            std::vector<std::pair<int64_t, const PriceLevel*>> out;
            out.reserve(static_cast<size_t>(n));
            for (auto it = levels.begin(); it != levels.end() && static_cast<int>(out.size()) < n;)
            {
                if (it->second.total_volume <= 0)
                {
                    ++it;
                    continue;
                }

                out.push_back({it->first, &it->second});
                ++it;
            }
            return out;
        }

        inline size_t size() const 
        {
            return levels.size();
        }

        void clear()
        {
            levels.clear();
        }

        inline const std::string printPriceVolume(int64_t id, size_t count = 0) const
        {
            std::stringstream ss;
            ss << "id:" << id;
            if (count == 0)
            {
                count = size();
            }
            int limit = 1;
            for (auto &_pair : topN(count))
            {
                ss << " " << limit << "=" << _pair.first << "/" << _pair.second->total_volume;
                ++limit;
            }
            return ss.str();
        }

    public:
        mutable std::map<int64_t, PriceLevel, Comp> levels;
    };
    using PriceBookGreat = PriceBook<std::greater<int64_t>>;
    using PriceBookLess = PriceBook<std::less<int64_t>>;


    // =====================
    // 高性能价格簿
    // Descending=true  : 大价优先（典型买盘）
    // Descending=false : 小价优先（典型卖盘）
    // 必须绑定共享 PriceLevelPool，所有价格档位统一从池中分配/归还
    // =====================
    template<bool Descending>
    class PriceLevelBook
    {
    public:
        PriceLevelBook(int64_t min_price, int64_t max_price, int64_t tick_size, PriceLevelPool* level_pool)
            : min_price_(min_price),
            max_price_(max_price),
            tick_size_(tick_size),
            level_pool_(level_pool)
        {
            if (tick_size_ <= 0)
            {
                throw std::invalid_argument("tick_size must be > 0");
            }
            if (min_price_ > max_price_)
            {
                throw std::invalid_argument("min_price must be <= max_price");
            }

            const int64_t span = max_price_ - min_price_;
            if (span % tick_size_ != 0)
            {
                throw std::invalid_argument("price range must align with tick_size");
            }
            if (level_pool_ == nullptr)
            {
                throw std::invalid_argument("PriceLevelPool must not be null");
            }

            tick_count_ = static_cast<int32_t>(span / tick_size_) + 1;
            levels_.assign(static_cast<size_t>(tick_count_), nullptr);
            bitmap_.assign(static_cast<size_t>((tick_count_ + 63) >> 6), 0ULL);

            best_tick_ = kInvalidTick;
            active_count_ = 0;
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
            return active_count_ == 0;
        }

        inline size_t size() const noexcept
        {
            return static_cast<size_t>(active_count_);
        }

        inline int64_t minPrice() const noexcept { return min_price_; }
        inline int64_t maxPrice() const noexcept { return max_price_; }
        inline int64_t tickSize() const noexcept { return tick_size_; }
        inline int32_t tickCount() const noexcept { return tick_count_; }

        // ========= 最优价 =========

        inline int64_t bestPrice() const noexcept
        {
            return best_tick_ == kInvalidTick ? 0 : tickToPrice(best_tick_);
        }

        inline int32_t bestTick() const noexcept
        {
            return best_tick_;
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
                levels_[static_cast<size_t>(tick)] = level;
                setBit(tick);
                ++active_count_;
                updateBestOnInsert(tick);
            }

            return levels_[static_cast<size_t>(tick)];
        }

        // 不存在返回 nullptr
        inline PriceLevel* find(int64_t price) noexcept
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                return nullptr;
            }

            return testBit(tick) ? levels_[static_cast<size_t>(tick)] : nullptr;
        }

        inline const PriceLevel* find(int64_t price) const noexcept
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                return nullptr;
            }

            return testBit(tick) ? levels_[static_cast<size_t>(tick)] : nullptr;
        }

        inline PriceLevel* findByTick(int32_t tick) noexcept
        {
            if (!isValidTick(tick))
            {
                return nullptr;
            }
            return testBit(tick) ? levels_[static_cast<size_t>(tick)] : nullptr;
        }

        inline const PriceLevel* findByTick(int32_t tick) const noexcept
        {
            if (!isValidTick(tick))
            {
                return nullptr;
            }
            return testBit(tick) ? levels_[static_cast<size_t>(tick)] : nullptr;
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
            --active_count_;

            if (tick == best_tick_)
            {
                recomputeBestAfterErase(tick);
            }
        }

        // 外部修改完 level 后，如果 volume<=0，调用这个
        inline void eraseIfEmpty(int64_t price) noexcept
        {
            PriceLevel* level = find(price);
            if (level != nullptr && level->total_volume <= 0)
            {
                erase(price);
            }
        }

        inline void eraseIfEmptyByTick(int32_t tick) noexcept
        {
            PriceLevel* level = findByTick(tick);
            if (level != nullptr && level->total_volume <= 0)
            {
                eraseByTick(tick);
            }
        }

        // ========= 清空 =========

        inline void clear() noexcept
        {
            std::fill(bitmap_.begin(), bitmap_.end(), 0ULL);
            for (int32_t tick = 0; tick < tick_count_; ++tick)
            {
                if (levels_[static_cast<size_t>(tick)] != nullptr)
                {
                    releaseLevel(tick);
                }
            }
            best_tick_ = kInvalidTick;
            active_count_ = 0;
        }

        // ========= TopN =========
        // 更适合热路径：调用方提供 buffer，避免分配
        inline int topNToBuffer(int n, std::pair<int64_t, const PriceLevel*>* out) const noexcept
        {
            if (n <= 0 || out == nullptr || active_count_ == 0)
            {
                return 0;
            }

            int count = 0;
            int32_t tick = best_tick_;

            while (tick != kInvalidTick && count < n)
            {
                const PriceLevel* level = levels_[static_cast<size_t>(tick)];
                out[count++] = {tickToPrice(tick), level};
                tick = nextActiveTick(tick);
            }

            return count;
        }

        // ========= All =========

        inline int allToBuffer(std::pair<int64_t, const PriceLevel*>* out) const noexcept
        {
            if (out == nullptr || active_count_ == 0)
            {
                return 0;
            }

            int count = 0;
            int32_t tick = best_tick_;

            while (tick != kInvalidTick && count < size())
            {
                out[count++] = {tickToPrice(tick), levels_[static_cast<size_t>(tick)]};
                tick = nextActiveTick(tick);
            }

            return count;
        }

        inline int allTicksToBuffer(int32_t* out) const noexcept
        {
            if (out == nullptr || active_count_ == 0)
            {
                return 0;
            }

            int count = 0;
            int32_t tick = best_tick_;

            while (tick != kInvalidTick && count < size())
            {
                out[count++] = tick;
                tick = nextActiveTick(tick);
            }

            return count;
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
                ss << " " << limit << "=" << item.first << "/" << item.second->total_volume;
                ++limit;
            }
            return ss.str();
        }

        // ========= 价格 / tick 转换 =========

        inline bool priceToTickNoThrow(int64_t price, int32_t& tick) const noexcept
        {
            if (price < min_price_ || price > max_price_)
            {
                return false;
            }

            const int64_t diff = price - min_price_;
            if (diff % tick_size_ != 0)
            {
                return false;
            }

            tick = static_cast<int32_t>(diff / tick_size_);
            return true;
        }

        inline int32_t priceToTickChecked(int64_t price) const
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                STDTHROW(STD_ERROR_CODE,"price out of range or not aligned with tick_size, price:"<<price<<" min:"<<min_price_<<" max:"<<max_price_, "price out of range or not aligned with tick_size, price:"<<price<<" min:"<<min_price_<<" max:"<<max_price_);
            }
            return tick;
        }

        inline int64_t tickToPrice(int32_t tick) const noexcept
        {
            return min_price_ + static_cast<int64_t>(tick) * tick_size_;
        }

        inline bool containsPrice(int64_t price) const noexcept
        {
            int32_t tick = 0;
            return priceToTickNoThrow(price, tick) && testBit(tick);
        }

        // 返回当前 book 绑定的价格档位池首地址，用于基于 slot 的订单链管理。
        inline PriceLevel* levelPoolData() noexcept
        {
            return level_pool_->data();
        }

        inline const PriceLevel* levelPoolData() const noexcept
        {
            return level_pool_->data();
        }

        // ========= 邻接有效档位 =========

        inline int32_t nextActiveTick(int32_t current_tick) const noexcept
        {
            if constexpr (Descending)
            {
                return findPrevSetBit(current_tick - 1);
            }
            else
            {
                return findNextSetBit(current_tick + 1);
            }
        }

        inline int32_t prevActiveTick(int32_t current_tick) const noexcept
        {
            if constexpr (Descending)
            {
                return findNextSetBit(current_tick + 1);
            }
            else
            {
                return findPrevSetBit(current_tick - 1);
            }
        }

    private:
        static constexpr int32_t kInvalidTick = -1;

        inline PriceLevel* acquireLevel(int32_t tick)
        {
            return level_pool_->alloc(tickToPrice(tick));
        }

        inline void releaseLevel(int32_t tick) noexcept
        {
            PriceLevel* level = levels_[static_cast<size_t>(tick)];
            if (level == nullptr)
            {
                return;
            }

            level_pool_->free(level);
            levels_[static_cast<size_t>(tick)] = nullptr;
        }

        inline bool isValidTick(int32_t tick) const noexcept
        {
            return tick >= 0 && tick < tick_count_;
        }

        inline bool testBit(int32_t tick) const noexcept
        {
            const uint64_t word = bitmap_[static_cast<size_t>(tick >> 6)];
            return ((word >> (tick & 63)) & 1ULL) != 0;
        }

        inline void setBit(int32_t tick) noexcept
        {
            bitmap_[static_cast<size_t>(tick >> 6)] |= (1ULL << (tick & 63));
        }

        inline void clearBit(int32_t tick) noexcept
        {
            bitmap_[static_cast<size_t>(tick >> 6)] &= ~(1ULL << (tick & 63));
        }

        inline void updateBestOnInsert(int32_t tick) noexcept
        {
            if (best_tick_ == kInvalidTick)
            {
                best_tick_ = tick;
                return;
            }

            if constexpr (Descending)
            {
                if (tick > best_tick_)
                {
                    best_tick_ = tick;
                }
            }
            else
            {
                if (tick < best_tick_)
                {
                    best_tick_ = tick;
                }
            }
        }

        inline void recomputeBestAfterErase(int32_t erased_tick) noexcept
        {
            if (active_count_ == 0)
            {
                best_tick_ = kInvalidTick;
                return;
            }

            if constexpr (Descending)
            {
                best_tick_ = findPrevSetBit(erased_tick - 1);
                if (best_tick_ == kInvalidTick)
                {
                    // 理论上不会走到这里，除非 best 之上有空洞且更高位没挂单
                    best_tick_ = findPrevSetBit(tick_count_ - 1);
                }
            }
            else
            {
                best_tick_ = findNextSetBit(erased_tick + 1);
                if (best_tick_ == kInvalidTick)
                {
                    best_tick_ = findNextSetBit(0);
                }
            }
        }

        // 找 >= start_tick 的第一个有效 tick
        inline int32_t findNextSetBit(int32_t start_tick) const noexcept
        {
            if (start_tick < 0)
            {
                start_tick = 0;
            }
            if (start_tick >= tick_count_)
            {
                return kInvalidTick;
            }

            size_t word_idx = static_cast<size_t>(start_tick >> 6);
            const int bit_idx = start_tick & 63;

            // 先处理起始 word
            {
                uint64_t word = bitmap_[word_idx];
                word &= (~0ULL << bit_idx);
                if (word != 0)
                {
                    const int offset = ctz64(word);
                    const int32_t tick = static_cast<int32_t>((word_idx << 6) + static_cast<size_t>(offset));
                    return tick < tick_count_ ? tick : kInvalidTick;
                }
            }

            // 后续完整 word
            const size_t word_count = bitmap_.size();
            for (++word_idx; word_idx < word_count; ++word_idx)
            {
                const uint64_t word = bitmap_[word_idx];
                if (word != 0)
                {
                    const int offset = ctz64(word);
                    const int32_t tick = static_cast<int32_t>((word_idx << 6) + static_cast<size_t>(offset));
                    return tick < tick_count_ ? tick : kInvalidTick;
                }
            }

            return kInvalidTick;
        }

        // 找 <= start_tick 的第一个有效 tick
        inline int32_t findPrevSetBit(int32_t start_tick) const noexcept
        {
            if (start_tick >= tick_count_)
            {
                start_tick = tick_count_ - 1;
            }
            if (start_tick < 0)
            {
                return kInvalidTick;
            }

            size_t word_idx = static_cast<size_t>(start_tick >> 6);
            const int bit_idx = start_tick & 63;

            // 先处理起始 word
            {
                uint64_t mask = (bit_idx == 63) ? ~0ULL : ((1ULL << (bit_idx + 1)) - 1ULL);
                uint64_t word = bitmap_[word_idx] & mask;
                if (word != 0)
                {
                    const int offset = 63 - clz64(word);
                    return static_cast<int32_t>((word_idx << 6) + static_cast<size_t>(offset));
                }
            }

            // 前面的完整 word
            while (word_idx > 0)
            {
                --word_idx;
                const uint64_t word = bitmap_[word_idx];
                if (word != 0)
                {
                    const int offset = 63 - clz64(word);
                    return static_cast<int32_t>((word_idx << 6) + static_cast<size_t>(offset));
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
        int64_t min_price_;
        int64_t max_price_;
        int64_t tick_size_;

        int32_t tick_count_ = 0;
        int32_t best_tick_ = kInvalidTick;
        int32_t active_count_ = 0;

        PriceLevelPool* level_pool_ = nullptr;
        std::vector<PriceLevel*> levels_;
        std::vector<uint64_t> bitmap_;
    };

    using PriceLevelBookGreat = PriceLevelBook<true>;   // 买盘：高价优先
    using PriceLevelBookLess  = PriceLevelBook<false>;  // 卖盘：低价优先

}
