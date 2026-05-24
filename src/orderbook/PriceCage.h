#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace marketdata{

class PriceCage
{
    public:
        PriceCage(){};
        ~PriceCage(){};

        inline void init(int64_t pre_close_price, bool Amain = true)
        {
            _pre_close_price = pre_close_price;
            _Amain = Amain;
        }

        inline void set(int64_t buy_best_price, int64_t sell_best_price, int64_t last_price)
        {
            _buy_best_price = buy_best_price;
            _sell_best_price = sell_best_price;
            _last_price = last_price;
        }

        inline void clear()
        {
            _pre_close_price = 0;
            _buy_best_price = 0;
            _sell_best_price = 0;
            _last_price = 0;
            _Amain = true;
        }

        // 获取买单基准价: ask1 -> bid1 -> lastprice -> preclose
        // 按照 对方 -> 本方 -> 最新价 -> 前一天收盘价顺序取第一个有效值
        inline int64_t getBuyBasePrice() const
        {
            // ask1
            if (_sell_best_price > 0) return _sell_best_price;
            // bid1
            if (_buy_best_price > 0) return _buy_best_price;
            // lastprice
            if (_last_price > 0) return _last_price;
            // preclose
            return _pre_close_price;
        }

        // 获取卖单基准价: bid1 -> ask1 -> lastprice -> preclose
        // 按照 对方 -> 本方 -> 最新价 -> 前一天收盘价顺序取第一个有效值
        inline int64_t getSellBasePrice() const
        {
            // bid1
            if (_buy_best_price > 0) return _buy_best_price;
            // ask1
            if (_sell_best_price > 0) return _sell_best_price;
            // lastprice
            if (_last_price > 0) return _last_price;
            // preclose
            return _pre_close_price;
        }

        inline static double roundTo(double value, int digits)
        {
            double scale = std::pow(10.0, digits);
            return std::round(value * scale) / scale;
        }

        inline int64_t getBuyCageUpperPrice(int64_t base_price) const
        {
            double origin_price = static_cast<double>(base_price) / 1000000;
            double upper_by_ratio = origin_price * 1.02;
            double upper = upper_by_ratio;

            if (_Amain)
            {
                double upper_by_tick = origin_price + 0.01 * 10;
                upper = std::max(upper_by_ratio, upper_by_tick);
            }

            return static_cast<int64_t>(roundTo(upper, 2) * 1000000);
        }

        inline int64_t getSellCageLowerPrice(int64_t base_price) const
        {
            double origin_price = static_cast<double>(base_price) / 1000000;
            double lower_by_ratio = origin_price * 0.98;
            double lower = lower_by_ratio;

            if (_Amain)
            {
                double lower_by_tick = origin_price - 0.01 * 10;
                lower = std::min(lower_by_ratio, lower_by_tick);
            }

            return static_cast<int64_t>(roundTo(lower, 2) * 1000000);
        }

        // 检查买单价格是否在笼子内: price <= 笼子上限
        inline bool isBuyPriceInCage(int64_t price) const
        {
            int64_t base_price = getBuyBasePrice();
            if (base_price <= 0) return true;
            return price <= getBuyCageUpperPrice(base_price);
        }

        // 检查卖单价格是否在笼子内: price >= 笼子下限
        inline bool isSellPriceInCage(int64_t price) const
        {
            int64_t base_price = getSellBasePrice();
            if (base_price <= 0) return true;
            return price >= getSellCageLowerPrice(base_price);
        }

    private:
        int64_t _buy_best_price = 0;
        int64_t _sell_best_price = 0;
        int64_t _last_price = 0;
        int64_t _pre_close_price = 0;
        bool _Amain = true;
};

}