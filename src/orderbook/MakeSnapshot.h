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
            _snap.id = lob->getCurrentId();
            _snap.last_price = lob->getLastPrice();
            _snap.low_limited = lob->getLimitdownPrice();
            _snap.high_limited = lob->getLimitupPrice();
            _snap.pre_close_price = lob->getPreClose();
            _snap.high_price = std::max(_snap.high_price, _snap.last_price);
            _snap.low_price = std::min(_snap.low_price, _snap.last_price);
            _snap.volume = lob->getVolumes();
            _snap.turnover = lob->getTurnover();

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
            _snap.id = lob->getCurrentId();
            _snap.last_price = lob->getLastPrice();
            _snap.low_limited = lob->getLimitdownPrice();
            _snap.high_limited = lob->getLimitupPrice();
            _snap.pre_close_price = lob->getPreClose();
            _snap.high_price = std::max(_snap.high_price, _snap.last_price);
            _snap.low_price = std::min(_snap.low_price, _snap.last_price);
            _snap.volume = lob->getMatchVolumes();
            _snap.turnover = lob->getMatchTurnover();

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

                // const int64_t visible_order_size = std::clamp<int64_t>(
                //     static_cast<int64_t>(level->match_order_size), 0, static_cast<int64_t>(level->order_size));
                // const int64_t visible_volume = std::clamp<int64_t>(
                //     level->match_total_volume, 0, level->total_volume);
                const int64_t visible_order_size = static_cast<int64_t>(level->match_order_size);
                const int64_t visible_volume = level->match_total_volume;

                _snap.bid_order[level_num] = visible_order_size;
                _snap.bid_price[level_num] = level->price;
                _snap.bid_volume[level_num] = visible_volume;
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

                // const int64_t visible_order_size = std::clamp<int64_t>(
                //     static_cast<int64_t>(level->match_order_size), 0, static_cast<int64_t>(level->order_size));
                // const int64_t visible_volume = std::clamp<int64_t>(
                //     level->match_total_volume, 0, level->total_volume);
                const int64_t visible_order_size = static_cast<int64_t>(level->match_order_size);
                const int64_t visible_volume = level->match_total_volume;

                _snap.offer_order[level_num] = visible_order_size;
                _snap.offer_price[level_num] = level->price;
                _snap.offer_volume[level_num] = visible_volume;
                sell_tick = sell_book->nextActiveTick(sell_tick);

                if (sell_check_break_tick == sell_tick) break;
                sell_check_break_tick = sell_tick;

                ++level_num;
            }
        }

        inline MDRapidSnapshot* getSnapshot() { return &_snap; }

        inline std::string print()
        {
            std::ostringstream ss;
            ss << "code:" << _snap.security_code
                << " id:" << _snap.id
                << " orig_time:" << _snap.orig_time
                << " last_price:" << _snap.last_price
                << " pre_close_price:" << _snap.pre_close_price
                << " open_price:" << _snap.open_price
                << " high_price:" << _snap.high_price
                << " low_price:" << _snap.low_price
                << " close_price:" << _snap.close_price
                << " volume:" << _snap.volume
                << " turnover:" << _snap.turnover
                << " high_limited:" << _snap.high_limited
                << " low_limited:" << _snap.low_limited;
 
            for (int i = 0; i < ConstField::kPositionParidLevelLen; ++i)
            {
                ss << " b" << i + 1 << "=" << _snap.bid_price[i] << "/" << _snap.bid_volume[i];
                ss << " s" << i + 1 << "=" << _snap.offer_price[i] << "/" << _snap.offer_volume[i];
            }

            for (int i = 0; i < ConstField::kPositionParidLevelLen; ++i)
            {
                ss << " bo" << i + 1 << "=" << _snap.bid_order[i];
                ss << " so" << i + 1 << "=" << _snap.offer_order[i];
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
