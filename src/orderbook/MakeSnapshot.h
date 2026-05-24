#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include "LimitOrderBook.h"
#include "common/MarketDataStruct.h"

namespace marketdata{

class MakeSnapshot
{
    public:
        MakeSnapshot(std::string code): _code(code) 
        {
            std::strncpy(_snap.security_code, _code.c_str(), ConstField::rSecurityCodeLen);
            _snap.security_code[ConstField::rSecurityCodeLen - 1] = '\0';
        };

        ~MakeSnapshot() {};

        inline void updateSnapshot(const LimitOrderBook *lob)
        {
            if (lob == nullptr) return;

            _snap.orig_time = lob->getCurrentTime();
            clearBidLevels();
            clearOfferLevels();

            std::array<std::pair<int64_t, const marketdata::PriceLevel *>, ConstField::kPositionParidLevelLen> buyLevels{};
            lob->getBuyTopN(ConstField::kPositionParidLevelLen, buyLevels.data());
            int level_num = 0;
            for (const auto &[price, level] : buyLevels)
            {
                if (level == nullptr) break;

                _snap.bid_order[level_num] = level->order_size;
                _snap.bid_price[level_num] = price;
                _snap.bid_volume[level_num] = level->total_volume;
                ++level_num;
            }

            std::array<std::pair<int64_t, const marketdata::PriceLevel *>, ConstField::kPositionParidLevelLen> sellLevels{};
            lob->getSellTopN(ConstField::kPositionParidLevelLen, sellLevels.data());
            level_num = 0;
            for (const auto &[price, level] : sellLevels)
            {
                if (level == nullptr) break;

                _snap.offer_order[level_num] = level->order_size;
                _snap.offer_price[level_num] = price;
                _snap.offer_volume[level_num] = level->total_volume;
                ++level_num;
            }
        }

        inline void updateMatchingSnapshot(const LimitOrderBook *lob)
        {
            if (lob == nullptr) return;

            const auto* buy_book = lob->getBuyBook();
            const auto* sell_book = lob->getSellBook();
            const auto* match_tick = lob->getMatchStartTick();

            _snap.orig_time = lob->getCurrentTime();
            clearBidLevels();
            clearOfferLevels();

            int32_t buy_tick = buy_book->findByTick(match_tick->_buy_tick) ? match_tick->_buy_tick : buy_book->bestTick();
            int level_num = 0;
            int32_t buy_check_break_tick = buy_tick;
            while (true)
            {
                if (level_num == 20) break;
                const PriceLevel *level = buy_book->findByTick(buy_tick);
                if (level == nullptr) break;
                if (level->matched || level->match_total_volume <= 0)
                {
                    buy_tick = buy_book->nextActiveTick(buy_tick);

                    if (buy_check_break_tick == buy_tick) break;
                    buy_check_break_tick = buy_tick;
                    continue;
                }

                _snap.bid_order[level_num] = level->match_order_size;
                _snap.bid_price[level_num] = level->price;
                _snap.bid_volume[level_num] = level->match_total_volume;
                buy_tick = buy_book->nextActiveTick(buy_tick);

                if (buy_check_break_tick == buy_tick) break;
                buy_check_break_tick = buy_tick;

                ++level_num;
            }
            
            int32_t sell_tick = sell_book->findByTick(match_tick->_sell_tick) ? match_tick->_sell_tick : sell_book->bestTick();
            level_num = 0;
            int32_t sell_check_break_tick = sell_tick;
            while (true)
            {
                if (level_num == 20) break;
                const PriceLevel *level = sell_book->findByTick(sell_tick);
                if (level == nullptr) break;
                if (level->matched || level->match_total_volume <= 0)
                {
                    sell_tick = sell_book->nextActiveTick(sell_tick);

                    if (sell_check_break_tick == sell_tick) break;
                    sell_check_break_tick = sell_tick;
                    continue;
                }

                _snap.offer_order[level_num] = level->match_order_size;
                _snap.offer_price[level_num] = level->price;
                _snap.offer_volume[level_num] = level->match_total_volume;
                sell_tick = sell_book->nextActiveTick(sell_tick);

                if (sell_check_break_tick == sell_tick) break;
                sell_check_break_tick = sell_tick;

                ++level_num;
            }
        }

        inline MDRapidSnapshot* getSnapshot() { return &_snap; }

        inline std::string printBuyLevel(int64_t id)
        {
            std::ostringstream ss;
            ss << "buy id:" << id;

            for (int i = 0; i < ConstField::kPositionParidLevelLen; ++i)
            {
                ss << " " << i + 1 << "=" << _snap.bid_price[i] << "/" << _snap.bid_volume[i];
            }
            ss << "\n";
            return ss.str();
        }

        inline std::string printSellLevel(int64_t id)
        {
            std::ostringstream ss;
            ss << "sell id:" << id;

            for (int i = 0; i < ConstField::kPositionParidLevelLen; ++i)
            {
                ss << " " << i + 1 << "=" << _snap.offer_price[i] << "/" << _snap.offer_volume[i];
            }
            ss << "\n";
            return ss.str();
        }

    private:
        inline void clearBidLevels()
        {
            std::fill_n(_snap.bid_order, ConstField::kPositionParidLevelLen, 0);
            std::fill_n(_snap.bid_price, ConstField::kPositionParidLevelLen, 0);
            std::fill_n(_snap.bid_volume, ConstField::kPositionParidLevelLen, 0);
        }

        inline void clearOfferLevels()
        {
            std::fill_n(_snap.offer_order, ConstField::kPositionParidLevelLen, 0);
            std::fill_n(_snap.offer_price, ConstField::kPositionParidLevelLen, 0);
            std::fill_n(_snap.offer_volume, ConstField::kPositionParidLevelLen, 0);
        }

        MDRapidSnapshot _snap{};
        std::string _code;

};

}
