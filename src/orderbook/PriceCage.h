#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace marketdata
{
namespace orderbook
{

class PriceCage
{
    public:
        PriceCage(){};
        ~PriceCage(){};

        inline void init(int64_t preClosePrice, bool amain = true)
        {
            m_preClosePrice = preClosePrice;
            m_amain = amain;
        }

        inline void set(int64_t buyBestPrice, int64_t sellBestPrice, int64_t lastPrice)
        {
            m_buyBestPrice = buyBestPrice;
            m_sellBestPrice = sellBestPrice;
            m_lastPrice = lastPrice;
        }

        inline void clear()
        {
            m_preClosePrice = 0;
            m_buyBestPrice = 0;
            m_sellBestPrice = 0;
            m_lastPrice = 0;
            m_amain = true;
        }

        // 获取买单基准价: ask1 -> bid1 -> lastprice -> preclose
        // 按照 对方 -> 本方 -> 最新价 -> 前一天收盘价顺序取第一个有效值
        inline int64_t getBuyBasePrice() const
        {
            // ask1
            if (m_sellBestPrice > 0) return m_sellBestPrice;
            // bid1
            if (m_buyBestPrice > 0) return m_buyBestPrice;
            // lastprice
            if (m_lastPrice > 0) return m_lastPrice;
            // preclose
            return m_preClosePrice;
        }

        // 获取卖单基准价: bid1 -> ask1 -> lastprice -> preclose
        // 按照 对方 -> 本方 -> 最新价 -> 前一天收盘价顺序取第一个有效值
        inline int64_t getSellBasePrice() const
        {
            // bid1
            if (m_buyBestPrice > 0) return m_buyBestPrice;
            // ask1
            if (m_sellBestPrice > 0) return m_sellBestPrice;
            // lastprice
            if (m_lastPrice > 0) return m_lastPrice;
            // preclose
            return m_preClosePrice;
        }

        inline static double roundTo(double value, int digits)
        {
            double scale = std::pow(10.0, digits);
            return std::round(value * scale) / scale;
        }

        inline int64_t getBuyCageUpperPrice(int64_t basePrice) const
        {
            double originPrice = static_cast<double>(basePrice) / 1000000;
            double upperByRatio = originPrice * 1.02;
            double upper = upperByRatio;

            if (m_amain)
            {
                double upperByTick = originPrice + 0.01 * 10;
                upper = std::max(upperByRatio, upperByTick);
            }

            return static_cast<int64_t>(std::llround(roundTo(upper, 2) * 1000000));
        }

        inline int64_t getSellCageLowerPrice(int64_t basePrice) const
        {
            double originPrice = static_cast<double>(basePrice) / 1000000;
            double lowerByRatio = originPrice * 0.98;
            double lower = lowerByRatio;

            if (m_amain)
            {
                double lowerByTick = originPrice - 0.01 * 10;
                lower = std::min(lowerByRatio, lowerByTick);
            }

            return static_cast<int64_t>(std::llround(roundTo(lower, 2) * 1000000));
        }

        // 检查买单价格是否在笼子内: price <= 笼子上限
        inline bool isBuyPriceInCage(int64_t price) const
        {
            int64_t basePrice = getBuyBasePrice();
            if (basePrice <= 0) return true;
            return price <= getBuyCageUpperPrice(basePrice);
        }

        // 检查卖单价格是否在笼子内: price >= 笼子下限
        inline bool isSellPriceInCage(int64_t price) const
        {
            int64_t basePrice = getSellBasePrice();
            if (basePrice <= 0) return true;
            return price >= getSellCageLowerPrice(basePrice);
        }

    private:
        int64_t m_buyBestPrice = 0;
        int64_t m_sellBestPrice = 0;
        int64_t m_lastPrice = 0;
        int64_t m_preClosePrice = 0;
        bool m_amain = true;
};

}
}
