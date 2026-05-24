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
        PriceLevelBook(int64_t min_price, int64_t max_price, int64_t tick_size, std::string code)
            : _min_price(min_price),
            _max_price(max_price),
            _tick_size(tick_size),
            _code(code)
        {
            if (_tick_size <= 0)
            {
                throw std::invalid_argument("tick_size must be > 0");
            }
            if (_min_price > _max_price)
            {
                throw std::invalid_argument("min_price must be <= max_price");
            }

            const int64_t span = _max_price - _min_price;
            if (span % _tick_size != 0)
            {
                throw std::invalid_argument("price range must align with tick_size");
            }

            _tick_count = static_cast<int32_t>(span / _tick_size) + 1;
            _levels.assign(static_cast<size_t>(_tick_count), {});
            _bitmap.assign(static_cast<size_t>((_tick_count + 63) >> 6), 0ULL);

            _best_tick = kInvalidTick;
            _cage_tick = kInvalidTick;
            _active_count = 0;
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
            return _active_count == 0;
        }

        inline size_t size() const noexcept
        {
            return static_cast<size_t>(_active_count);
        }

        // ========= 最优价 =========

        inline int32_t bestTick() const noexcept
        {
            return _best_tick;
        }

        inline int64_t bestPrice() const noexcept
        {
            return _best_tick == kInvalidTick ? 0 : tickToPrice(_best_tick);
        }

        // ========= 价格笼子起点 =========

        inline int32_t cageTick() const noexcept
        {
            return _cage_tick;
        }

        inline int64_t cagePrice() const noexcept
        {
            return _cage_tick == kInvalidTick ? 0 : tickToPrice(_cage_tick);
        }

        inline bool hasCageTick() const noexcept
        {
            return _cage_tick != kInvalidTick;
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
                ++_active_count;
                updateBestOnInsert(tick);
            }

            return &_levels[static_cast<size_t>(tick)];
        }

        // 不存在返回 nullptr
        inline PriceLevel* find(int64_t price) noexcept
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                return nullptr;
            }

            return testBit(tick) ? &_levels[static_cast<size_t>(tick)] : nullptr;
        }

        inline const PriceLevel* find(int64_t price) const noexcept
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                return nullptr;
            }

            return testBit(tick) ? &_levels[static_cast<size_t>(tick)] : nullptr;
        }

        inline PriceLevel* findByTick(int32_t tick) noexcept
        {
            if (!isValidTick(tick))
            {
                return nullptr;
            }
            return testBit(tick) ? &_levels[static_cast<size_t>(tick)] : nullptr;
        }

        inline const PriceLevel* findByTick(int32_t tick) const noexcept
        {
            if (!isValidTick(tick))
            {
                return nullptr;
            }
            return testBit(tick) ? &_levels[static_cast<size_t>(tick)] : nullptr;
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
            --_active_count;

            if (tick == _best_tick)
            {
                recomputeBestAfterErase(tick);
            }
            if (tick == _cage_tick)
            {
                recomputeCageAfterErase(tick);
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
            for (int32_t tick = 0; tick < _tick_count; ++tick)
            {
                if (_levels[static_cast<size_t>(tick)].is_use)
                {
                    releaseLevel(tick);
                }
            }
            std::fill(_bitmap.begin(), _bitmap.end(), 0ULL);
            _best_tick = kInvalidTick;
            _cage_tick = kInvalidTick;
            _active_count = 0;
        }

        // ========= TopN =========
        // 更适合热路径：调用方提供 buffer，避免分配
        inline int topNToBuffer(int n, std::pair<int64_t, const PriceLevel*>* out) const noexcept
        {
            if (n <= 0 || out == nullptr || _active_count == 0)
            {
                return 0;
            }

            int count = 0;
            int32_t tick = _best_tick;

            while (tick != kInvalidTick && count < n)
            {
                const PriceLevel* level = &_levels[static_cast<size_t>(tick)];
                out[count++] = {tickToPrice(tick), level};
                tick = nextActiveTick(tick);
            }

            return count;
        }

        // ========= All =========

        inline int allToBuffer(std::pair<int64_t, const PriceLevel*>* out) const noexcept
        {
            if (out == nullptr || _active_count == 0)
            {
                return 0;
            }

            int count = 0;
            int32_t tick = firstBookActiveTick();

            while (tick != kInvalidTick && count < size())
            {
                out[count++] = {tickToPrice(tick), &_levels[static_cast<size_t>(tick)]};
                tick = nextActiveTick(tick);
            }

            return count;
        }

        // ========= 价格笼子遍历 =========
        // 价格笼子的遍历方向与 PriceLevelBook 的常规方向相反：
        // Descending=true 时从低价向高价找，Descending=false 时从高价向低价找。

        template<typename InCage>
        inline bool refreshBestByCage(InCage&& in_cage) noexcept
        {
            const int32_t old_best_tick = _best_tick;
            const int32_t old_cage_tick = _cage_tick;

            _best_tick = kInvalidTick;
            _cage_tick = kInvalidTick;

            int32_t tick = firstBookActiveTick();
            while (tick != kInvalidTick)
            {
                const int64_t price = tickToPrice(tick);
                if (in_cage(price))
                {
                    _best_tick = tick;
                    break;
                }

                // 记录最靠近笼内边界的笼外档位，后续按反方向继续遍历笼外区域。
                _cage_tick = tick;
                tick = nextActiveTick(tick);
            }

            return old_best_tick != _best_tick || old_cage_tick != _cage_tick;
        }

        inline bool clearCage() noexcept
        {
            const int32_t old_best_tick = _best_tick;
            const int32_t old_cage_tick = _cage_tick;

            _best_tick = firstBookActiveTick();
            _cage_tick = kInvalidTick;

            return old_best_tick != _best_tick || old_cage_tick != _cage_tick;
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
            if (price < _min_price || price > _max_price)
            {
                return false;
            }

            const int64_t diff = price - _min_price;
            if (diff % _tick_size != 0)
            {
                return false;
            }

            tick = static_cast<int32_t>(diff / _tick_size);
            return true;
        }

        inline int32_t priceToTickChecked(int64_t price) const
        {
            int32_t tick = 0;
            if (!priceToTickNoThrow(price, tick))
            {
                STDTHROW(STD_ERROR_CODE,"price out of range or not aligned with tick_size, price:"<<price<<" min:"<<_min_price<<" max:"<<_max_price<<" code:"<<_code, "price out of range or not aligned with tick_size, price:"<<price<<" min:"<<_min_price<<" max:"<<_max_price<<" code:"<<_code);
            }
            return tick;
        }

        inline int64_t tickToPrice(int32_t tick) const noexcept
        {
            return _min_price + static_cast<int64_t>(tick) * _tick_size;
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

        inline int32_t firstCageActiveTick() const noexcept
        {
            return _cage_tick;
        }

        inline int32_t nextCageActiveTick(int32_t current_tick) const noexcept
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

        inline int32_t prevCageActiveTick(int32_t current_tick) const noexcept
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
            PriceLevel* level = &_levels[static_cast<size_t>(tick)];
            level->reset();
            level->is_use = true;
            level->price = tickToPrice(tick);
            return level;
        }

        inline void releaseLevel(int32_t tick) noexcept
        {
            if (!isValidTick(tick))
            {
                return;
            }
            PriceLevel* level = &_levels[static_cast<size_t>(tick)];
            level->reset();
        }

        inline bool isValidTick(int32_t tick) const noexcept
        {
            return tick >= 0 && tick < _tick_count;
        }

        inline bool testBit(int32_t tick) const noexcept
        {
            const uint64_t word = _bitmap[static_cast<size_t>(tick >> 6)];
            return ((word >> (tick & 63)) & 1ULL) != 0;
        }

        inline void setBit(int32_t tick) noexcept
        {
            _bitmap[static_cast<size_t>(tick >> 6)] |= (1ULL << (tick & 63));
        }

        inline void clearBit(int32_t tick) noexcept
        {
            _bitmap[static_cast<size_t>(tick >> 6)] &= ~(1ULL << (tick & 63));
        }

        inline void updateBestOnInsert(int32_t tick) noexcept
        {
            if (_best_tick == kInvalidTick)
            {
                _best_tick = tick;
                return;
            }

            if constexpr (Descending)
            {
                if (tick > _best_tick)
                {
                    _best_tick = tick;
                }
            }
            else
            {
                if (tick < _best_tick)
                {
                    _best_tick = tick;
                }
            }
        }

        inline int32_t firstBookActiveTick() const noexcept
        {
            if constexpr (Descending)
            {
                return findPrevSetBit(_tick_count - 1);
            }
            else
            {
                return findNextSetBit(0);
            }
        }

        inline void recomputeBestAfterErase(int32_t erased_tick) noexcept
        {
            if (_active_count == 0)
            {
                _best_tick = kInvalidTick;
                return;
            }

            if constexpr (Descending)
            {
                _best_tick = findPrevSetBit(erased_tick - 1);
            }
            else
            {
                _best_tick = findNextSetBit(erased_tick + 1);
            }
        }

        inline void recomputeCageAfterErase(int32_t erased_tick) noexcept
        {
            if (_active_count == 0)
            {
                _cage_tick = kInvalidTick;
                return;
            }

            if constexpr (Descending)
            {
                _cage_tick = findNextSetBit(erased_tick + 1);
            }
            else
            {
                _cage_tick = findPrevSetBit(erased_tick - 1);
            }
        }

        // 找 >= start_tick 的第一个有效 tick
        inline int32_t findNextSetBit(int32_t start_tick) const noexcept
        {
            if (start_tick < 0)
            {
                start_tick = 0;
            }
            if (start_tick >= _tick_count)
            {
                return kInvalidTick;
            }

            size_t word_idx = static_cast<size_t>(start_tick >> 6);
            const int bit_idx = start_tick & 63;

            // 先处理起始 word
            {
                uint64_t word = _bitmap[word_idx];
                word &= (~0ULL << bit_idx);
                if (word != 0)
                {
                    const int offset = ctz64(word);
                    const int32_t tick = static_cast<int32_t>((word_idx << 6) + static_cast<size_t>(offset));
                    return tick < _tick_count ? tick : kInvalidTick;
                }
            }

            // 后续完整 word
            const size_t word_count = _bitmap.size();
            for (++word_idx; word_idx < word_count; ++word_idx)
            {
                const uint64_t word = _bitmap[word_idx];
                if (word != 0)
                {
                    const int offset = ctz64(word);
                    const int32_t tick = static_cast<int32_t>((word_idx << 6) + static_cast<size_t>(offset));
                    return tick < _tick_count ? tick : kInvalidTick;
                }
            }

            return kInvalidTick;
        }

        // 找 <= start_tick 的第一个有效 tick
        inline int32_t findPrevSetBit(int32_t start_tick) const noexcept
        {
            if (start_tick >= _tick_count)
            {
                start_tick = _tick_count - 1;
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
                uint64_t word = _bitmap[word_idx] & mask;
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
                const uint64_t word = _bitmap[word_idx];
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
        int64_t _min_price;
        int64_t _max_price;
        int64_t _tick_size;

        int32_t _tick_count = 0;
        int32_t _best_tick = kInvalidTick;
        int32_t _cage_tick = kInvalidTick;
        int32_t _active_count = 0;

        std::vector<PriceLevel> _levels;
        std::vector<uint64_t> _bitmap;

        std::string _code;
    };

    using PriceLevelBookGreat = PriceLevelBook<true>;   // 买盘：高价优先
    using PriceLevelBookLess  = PriceLevelBook<false>;  // 卖盘：低价优先

}
