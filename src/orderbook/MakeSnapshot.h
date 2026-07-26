#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include "LimitOrderBook.h"
#include "common/MarketDataStruct.h"

namespace marketdata
{
namespace orderbook
{

    class MakeSnapshot
    {
        public:
            MakeSnapshot(std::string code): m_code(code)
            {
                std::strncpy(m_snap.securityCode, m_code.c_str(), ConstField::kRSecurityCodeLen);
                m_snap.securityCode[ConstField::kRSecurityCodeLen - 1] = '\0';
            };

            ~MakeSnapshot() {};

            inline void updateSnapshot(const LimitOrderBook *lob)
            {
                if (lob == nullptr) return;

                m_snap.origTime = lob->getCurrentTime();
                clearBidLevels();
                clearOfferLevels();
                m_snap.id = lob->getCurrentId();
                m_snap.lastPrice = lob->getLastPrice();
                m_snap.lowLimited = lob->getLimitdownPrice();
                m_snap.highLimited = lob->getLimitupPrice();
                m_snap.preClosePrice = lob->getPreClose();
                if (m_snap.lastPrice > 0)
                {
                    m_snap.highPrice = std::max(m_snap.highPrice, m_snap.lastPrice);
                    m_snap.lowPrice = m_snap.lowPrice > 0 ? std::min(m_snap.lowPrice, m_snap.lastPrice) : m_snap.lastPrice;
                }
                m_snap.volume = lob->getVolumes();
                m_snap.turnover = lob->getTurnover();

                std::array<std::pair<int64_t, const PriceLevel *>, ConstField::kPositionParidLevelLen> buyLevels{};
                lob->getBuyTopN(ConstField::kPositionParidLevelLen, buyLevels.data());
                int levelNum = 0;
                for (const auto &[price, level] : buyLevels)
                {
                    if (level == nullptr) break;

                    m_snap.bidOrder[levelNum] = level->orderSize;
                    m_snap.bidPrice[levelNum] = price;
                    m_snap.bidVolume[levelNum] = level->totalVolume;
                    ++levelNum;
                }

                std::array<std::pair<int64_t, const PriceLevel *>, ConstField::kPositionParidLevelLen> sellLevels{};
                lob->getSellTopN(ConstField::kPositionParidLevelLen, sellLevels.data());
                levelNum = 0;
                for (const auto &[price, level] : sellLevels)
                {
                    if (level == nullptr) break;

                    m_snap.offerOrder[levelNum] = level->orderSize;
                    m_snap.offerPrice[levelNum] = price;
                    m_snap.offerVolume[levelNum] = level->totalVolume;
                    ++levelNum;
                }
            }

            inline void updateMatchingSnapshot(const LimitOrderBook *lob)
            {
                if (lob == nullptr) return;

                m_snap.origTime = lob->getCurrentTime();
                if (m_snap.origTime >= 145700000000000L)
                {
                    updateSnapshot(lob);
                    return;
                }

                const auto* buyBook = lob->getBuyBook();
                const auto* sellBook = lob->getSellBook();
                const auto* matchTick = lob->getMatchStartTick();

                clearBidLevels();
                clearOfferLevels();
                m_snap.id = lob->getCurrentId();
                m_snap.lastPrice = lob->getLastPrice();
                m_snap.lowLimited = lob->getLimitdownPrice();
                m_snap.highLimited = lob->getLimitupPrice();
                m_snap.preClosePrice = lob->getPreClose();
                if (m_snap.lastPrice > 0)
                {
                    m_snap.highPrice = std::max(m_snap.highPrice, m_snap.lastPrice);
                    m_snap.lowPrice = m_snap.lowPrice > 0 ? std::min(m_snap.lowPrice, m_snap.lastPrice) : m_snap.lastPrice;
                }
                m_snap.volume = lob->getMatchVolumes();
                m_snap.turnover = lob->getMatchTurnover();

                int32_t buyTick = buyBook->findByTick(matchTick->buyTick) ? matchTick->buyTick : buyBook->bestTick();
                int levelNum = 0;
                int32_t buyCheckBreakTick = buyTick;
                while (true)
                {
                    if (levelNum == 20) break;
                    const PriceLevel *level = buyBook->findByTick(buyTick);
                    if (level == nullptr) break;
                    if (level->matched || level->matchTotalVolume <= 0)
                    {
                        buyTick = buyBook->nextActiveTick(buyTick);

                        if (buyCheckBreakTick == buyTick) break;
                        buyCheckBreakTick = buyTick;
                        continue;
                    }

                    // const int64_t visible_order_size = std::clamp<int64_t>(
                    //     static_cast<int64_t>(level->match_order_size), 0, static_cast<int64_t>(level->order_size));
                    // const int64_t visible_volume = std::clamp<int64_t>(
                    //     level->match_total_volume, 0, level->total_volume);
                    const int64_t visibleOrderSize = static_cast<int64_t>(level->matchOrderSize);
                    const int64_t visibleVolume = level->matchTotalVolume;

                    m_snap.bidOrder[levelNum] = visibleOrderSize;
                    m_snap.bidPrice[levelNum] = level->price;
                    m_snap.bidVolume[levelNum] = visibleVolume;
                    buyTick = buyBook->nextActiveTick(buyTick);

                    if (buyCheckBreakTick == buyTick) break;
                    buyCheckBreakTick = buyTick;

                    ++levelNum;
                }

                int32_t sellTick = sellBook->findByTick(matchTick->sellTick) ? matchTick->sellTick : sellBook->bestTick();
                levelNum = 0;
                int32_t sellCheckBreakTick = sellTick;
                while (true)
                {
                    if (levelNum == 20) break;
                    const PriceLevel *level = sellBook->findByTick(sellTick);
                    if (level == nullptr) break;
                    if (level->matched || level->matchTotalVolume <= 0)
                    {
                        sellTick = sellBook->nextActiveTick(sellTick);

                        if (sellCheckBreakTick == sellTick) break;
                        sellCheckBreakTick = sellTick;
                        continue;
                    }

                    // const int64_t visible_order_size = std::clamp<int64_t>(
                    //     static_cast<int64_t>(level->match_order_size), 0, static_cast<int64_t>(level->order_size));
                    // const int64_t visible_volume = std::clamp<int64_t>(
                    //     level->match_total_volume, 0, level->total_volume);
                    const int64_t visibleOrderSize = static_cast<int64_t>(level->matchOrderSize);
                    const int64_t visibleVolume = level->matchTotalVolume;

                    m_snap.offerOrder[levelNum] = visibleOrderSize;
                    m_snap.offerPrice[levelNum] = level->price;
                    m_snap.offerVolume[levelNum] = visibleVolume;
                    sellTick = sellBook->nextActiveTick(sellTick);

                    if (sellCheckBreakTick == sellTick) break;
                    sellCheckBreakTick = sellTick;

                    ++levelNum;
                }
            }

            inline MDRapidSnapshot* getSnapshot() { return &m_snap; }

            inline std::string print()
            {
                std::ostringstream ss;
                ss << "code:" << m_snap.securityCode
                    << " id:" << m_snap.id
                    << " orig_time:" << m_snap.origTime
                    << " last_price:" << m_snap.lastPrice
                    << " pre_close_price:" << m_snap.preClosePrice
                    << " open_price:" << m_snap.openPrice
                    << " high_price:" << m_snap.highPrice
                    << " low_price:" << m_snap.lowPrice
                    << " close_price:" << m_snap.closePrice
                    << " volume:" << m_snap.volume
                    << " turnover:" << m_snap.turnover
                    << " high_limited:" << m_snap.highLimited
                    << " low_limited:" << m_snap.lowLimited;

                for (int i = 0; i < ConstField::kPositionParidLevelLen; ++i)
                {
                    ss << " b" << i + 1 << "=" << m_snap.bidPrice[i] << "/" << m_snap.bidVolume[i];
                    ss << " s" << i + 1 << "=" << m_snap.offerPrice[i] << "/" << m_snap.offerVolume[i];
                }

                for (int i = 0; i < ConstField::kPositionParidLevelLen; ++i)
                {
                    ss << " bo" << i + 1 << "=" << m_snap.bidOrder[i];
                    ss << " so" << i + 1 << "=" << m_snap.offerOrder[i];
                }

                ss << "\n";
                return ss.str();
            }

        private:
            inline void clearBidLevels()
            {
                std::fill_n(m_snap.bidOrder, ConstField::kPositionParidLevelLen, 0);
                std::fill_n(m_snap.bidPrice, ConstField::kPositionParidLevelLen, 0);
                std::fill_n(m_snap.bidVolume, ConstField::kPositionParidLevelLen, 0);
            }

            inline void clearOfferLevels()
            {
                std::fill_n(m_snap.offerOrder, ConstField::kPositionParidLevelLen, 0);
                std::fill_n(m_snap.offerPrice, ConstField::kPositionParidLevelLen, 0);
                std::fill_n(m_snap.offerVolume, ConstField::kPositionParidLevelLen, 0);
            }

        private:
            MDRapidSnapshot m_snap{};
            std::string m_code;

    };
}
}
