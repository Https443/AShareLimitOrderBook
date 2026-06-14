#ifndef MARKETDATA_ORDERBOOK_LIMITORDERBOOKLITE_H
#define MARKETDATA_ORDERBOOK_LIMITORDERBOOKLITE_H

#include "common/MarketDataStruct.h"
#include "util/Config.h"
#include <memory>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <cassert>
#include "util/logger.h"
#include "util/MyLnxTimer.h"
#include "util/Util.h"
#include <algorithm>
#include <queue>
#include <vector>
#include "PriceBook.h"

namespace marketdata
{

    struct OrderLiteMeta
    {
        int64_t price = -1;
        int64_t remaining_volume = 0;
    };

    class LimitOrderBookLite
    {
    public:
        explicit LimitOrderBookLite(const std::string &date,
                                    const std::string &code,
                                    const uint8_t exchange,
                                    const int64_t per_close_price,
                                    const int64_t min_price,
                                    const int64_t max_price):
            _date(date), _code(code), _exchange(exchange),
            per_close(per_close_price),
            buy_book(min_price, max_price, 10000, code),
            sell_book(min_price, max_price, 10000, code),
            pending_buy_book(min_price, max_price, 10000, code),
            pending_sell_book(min_price, max_price, 10000, code)
        {
            id_meta_map.reserve(30'000);
            pending_trade.reserve(1'000);

            // 价格笼子（试点阶段）：
            // 科创板从2019年7月22日起即运用价格笼子 + 废单处理，即价格笼子以外直接废单；
            // 创业板在试点注册制推广后从2020年6月12日开始，采用了价格笼子 + 订单暂存 → 等待条件满足再入撮合 的机制。（佐证材料：https://www.szse.cn/disclosure/notice/general/t20200612_578381.html）

            // 2023年4月10日 全面注册制+主板价格笼子
            // 主板、科创板、创业板均改外超过价格笼子即废单处理 （佐证材料：“当委托进入交易系统时，如果其价格超过有效价格范围或价格限制，该委托将被视为无效。”——引自证监会）
            if (((date >= "20200612" && !code.empty() && code[0] == '3' && exchange == 0)) ||
                (code.size() >= 2 && code[0] == '6' && code[1] == '8' && exchange == 1))
            {
                LOG_INFO(app_log::logger(), "enable price cage, date:{} code:{}", date, code);
                this->enablePriceCage(true, false);
            }
            // 20230410注册制，主板开启价格笼子，主板、科创板、创业板均改外超过价格笼子即废单处理[理论上不会有触发价格笼子的订单]
            else if (date >= "20230410")
            {
                LOG_INFO(app_log::logger(), "enable all price cage, date:{} code:{}", date, code);
                this->enablePriceCage(true, true);
            }
        }

        ~LimitOrderBookLite()
        {
            pending_trade.clear();
            id_meta_map.clear();

            buy_book.clear();
            sell_book.clear();

            pending_buy_book.clear();
            pending_sell_book.clear();

            last_price = 0;
            current_id = 0;
            per_close = 0;
            price_cage_enabled = false;
            price_cage_Amain = false;

            _date = "";
            _code = "";
        }

        // 启用/禁用价格笼子
        inline void enablePriceCage(bool enable, bool isMain)
        {
            price_cage_enabled = enable;
            price_cage_Amain = isMain;
        }

        inline bool isPriceCageEnabled() const
        {
            return price_cage_enabled;
        }

        // 设置昨收价
        inline void setPreClose(int64_t price)
        {
            per_close = price;
        }

        inline int64_t getPreClose() const
        {
            return per_close;
        }

        // 设置最新成交价
        inline void setLastPrice(int64_t price)
        {
            last_price = price;
            checkAndMovePendingOrders();
        }

        inline int64_t getLastPrice() const
        {
            return last_price;
        }

        inline void processOrder(const Order *order)
        {
            if (order == nullptr)
            {
                return;
            }

            int64_t orderID = order->appl_seq_num;
            int64_t time = order->time;

            if (order->channel_no < 10)
            {
                // 上交所输出的order是未成交的order委托，已成交的的不显示委托，只在trade中一带而过
                if (order->order_type == 'A')
                {
                    if (order->side == 'B')
                    {
                        addOrder(order->appl_seq_num, order->price, order->volume, time, OrderSideType::BUY);
                    }
                    else if (order->side == 'S')
                    {
                        addOrder(order->appl_seq_num, order->price, order->volume, time, OrderSideType::SELL);
                    }
                }
                else if (order->order_type == 'D')
                {
                    if (order->side == 'B')
                    {
                        dropOrder(order->appl_seq_num, order->volume, time, OrderSideType::BUY);
                    }
                    else if (order->side == 'S')
                    {
                        dropOrder(order->appl_seq_num, order->volume, time, OrderSideType::SELL);
                    }
                }
            }
            else if (order->channel_no > 2000)
            {
                // 买 / 借入
                if (order->side == '1' || order->side == 'G')
                {
                    // 限价单
                    if (order->order_type == '2')
                    {
                        addOrder(order->appl_seq_num, order->price, order->volume, time, OrderSideType::BUY);
                    }
                    // 市价单情况特殊处理[除本方最优，其他四个市价单均以对手价成交]
                    else if (order->order_type == '1')
                    {
                        auto sell1_price = sell_book.bestPrice();
                        if (sell1_price <= 0)
                        {
                            skip_ids.insert(order->appl_seq_num);
                            return;
                        }
                        addOrder(order->appl_seq_num, sell1_price, order->volume, time, OrderSideType::BUY);
                    }
                    // 本方最优价格申报，以申报进入交易主机时"集中申报簿中本方队列的最优价格"为其申报价格，集中申报簿中本方无申报的，申报自动撤销。
                    else if (order->order_type == 'U')
                    {
                        auto buy1_price = buy_book.bestPrice();
                        if (buy1_price <= 0)
                        {
                            skip_ids.insert(order->appl_seq_num);
                            return;
                        }
                        addOrder(order->appl_seq_num, buy1_price, order->volume, time, OrderSideType::BUY);
                    }
                }
                // 卖 / 出借
                else if (order->side == '2' || order->side == 'F')
                {
                    if (order->order_type == '2')
                    {
                        addOrder(order->appl_seq_num, order->price, order->volume, time, OrderSideType::SELL);
                    }
                    else if (order->order_type == '1')
                    {
                        auto buy1_price = buy_book.bestPrice();
                        if (buy1_price <= 0)
                        {
                            skip_ids.insert(order->appl_seq_num);
                            return;
                        }
                        addOrder(order->appl_seq_num, buy1_price, order->volume, time, OrderSideType::SELL);
                    }
                    else if (order->order_type == 'U')
                    {
                        auto sell1_price = sell_book.bestPrice();
                        if (sell1_price <= 0)
                        {
                            skip_ids.insert(order->appl_seq_num);
                            return;
                        }
                        addOrder(order->appl_seq_num, sell1_price, order->volume, time, OrderSideType::SELL);
                    }
                }
                else
                {
                    LOG_ERROR(app_log::logger(), "not support order side:{}, order type:{}, orderID:{}", order->side, order->order_type, orderID);
                }
            }
        };

        inline void processTrade(const Trade *trade)
        {
            if (trade == nullptr)
            {
                return;
            }
            // std::string code = trade->security_code;
            int64_t time = trade->time;

            if (trade->channel_no < 10)
            {
                // 连续竞价阶段
                if (time >= 93000000000000L && time < 145700000000000L)
                {
                    // 更新最新成交价
                    setLastPrice(trade->price);

                    // 上交所成交考虑方向，如果order和trade方向相同则跳过，不同则删除
                    if (trade->bid_appl_seq_num > trade->offer_appl_seq_num || trade->side == 'B')
                    {
                        dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL);
                    }
                    else if (trade->bid_appl_seq_num < trade->offer_appl_seq_num || trade->side == 'S')
                    {
                        dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY);
                    }
                    // N仅发生在集合竞价阶段
                    else if (trade->side == 'N')
                    {
                        LOG_WARNING(app_log::logger(), "code:{} biz_index:{} bid id:{} offer id:{} side is 'N' unknow", trade->security_code, trade->biz_index, trade->bid_appl_seq_num, trade->offer_appl_seq_num);
                        dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY, true);
                        dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL, true);
                    }
                    else
                    {
                        LOG_WARNING(app_log::logger(), "code:{} biz_index:{} bid id:{} offer id:{} side is unknow", trade->security_code, trade->biz_index, trade->bid_appl_seq_num, trade->offer_appl_seq_num);
                        dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY, true);
                        dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL, true);
                    }
                }
                // 开盘集合竞价阶段
                else if (time < 93000000000000L)
                {
                    dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY);
                    dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL);
                }
                // 收盘集合竞价阶段
                else if (time >= 145700000000000L)
                {
                    dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY, true);
                    dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL, true);
                }
                updateCurrentPrice();
            }
            else if (trade->channel_no > 2000)
            {
                // 撤单
                if (trade->exec_type == '4')
                {
                    // 获取订单编号，撤买/撤卖
                    if (trade->bid_appl_seq_num != 0)
                    {
                        if (skip_ids.find(trade->bid_appl_seq_num) != skip_ids.end()) return;
                        dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY);
                    }
                    else if (trade->offer_appl_seq_num != 0)
                    {
                        if (skip_ids.find(trade->offer_appl_seq_num) != skip_ids.end()) return;
                        dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL);
                    }
                }
                // 成交
                else if (trade->exec_type == 'F')
                {
                    // 更新最新成交价
                    setLastPrice(trade->price);
                    dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY);
                    dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL);
                }
                updateCurrentPrice();
            }
        };

        inline const std::vector<std::pair<int64_t, const PriceLevel*>> getBuyTopN(int n) const
        {
            if (n <= 0)
            {
                return {};
            }

            std::vector<std::pair<int64_t, const PriceLevel*>> out(static_cast<size_t>(n));
            const int count = buy_book.topNToBuffer(n, out.data());
            out.resize(static_cast<size_t>(count));
            return out;
        }

        inline const std::vector<std::pair<int64_t, const PriceLevel*>> getSellTopN(int n) const
        {
            if (n <= 0)
            {
                return {};
            }

            std::vector<std::pair<int64_t, const PriceLevel*>> out(static_cast<size_t>(n));
            const int count = sell_book.topNToBuffer(n, out.data());
            out.resize(static_cast<size_t>(count));
            return out;
        }

        inline void getBuyTopN(int n, std::pair<int64_t, const PriceLevel*> *out) const
        {
            buy_book.topNToBuffer(n, out);
            return;
        }

        inline void getSellTopN(int n, std::pair<int64_t, const PriceLevel*> *out) const
        {
            sell_book.topNToBuffer(n, out);
            return;
        }

        const int64_t getBuyBestPrice() const { return buy_book.bestPrice(); }

        const int64_t getSellBestPrice() const { return sell_book.bestPrice(); }

        inline bool buyIsEmpty()
        {
            return buy_book.empty();
        };

        inline bool sellIsEmpty()
        {
            return sell_book.empty();
        };

        inline const std::string printBuyPriceVolume(int64_t id, size_t count = 0) const
        {
            std::stringstream ss;
            ss << "buy " << buy_book.printPriceVolume(id, count) << "\n";
            return ss.str();
        }

        inline const std::string printSellPriceVolume(int64_t id, size_t count = 0) const
        {
            std::stringstream ss;
            ss << "sell " << sell_book.printPriceVolume(id, count) << "\n";
            return ss.str();
        }

    private:
        // 获取买单基准价: ask1 -> bid1 -> lastprice -> preclose
        // 按照 对方 -> 本方 -> 最新价 -> 前一天收盘价顺序取第一个有效值
        inline int64_t getBuyBasePrice() const
        {
            // ask1
            if (current_ask1_price > 0) return current_ask1_price;
            // bid1
            if (current_bid1_price > 0) return current_bid1_price;
            // lastprice
            if (last_price > 0) return last_price;
            // preclose
            return per_close;
        }

        // 获取卖单基准价: bid1 -> ask1 -> lastprice -> preclose
        // 按照 对方 -> 本方 -> 最新价 -> 前一天收盘价顺序取第一个有效值
        inline int64_t getSellBasePrice() const
        {
            // bid1
            if (current_bid1_price > 0) return current_bid1_price;
            // ask1
            if (current_ask1_price > 0) return current_ask1_price;
            // lastprice
            if (last_price > 0) return last_price;
            // preclose
            return per_close;
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

            if (price_cage_Amain)
            {
                double upper_by_tick = origin_price + 0.01 * 10;
                upper = std::max(upper_by_ratio, upper_by_tick);
            }

            return static_cast<int64_t>(std::llround(roundTo(upper, 2) * 1000000));
        }

        inline int64_t getSellCageLowerPrice(int64_t base_price) const
        {
            double origin_price = static_cast<double>(base_price) / 1000000;
            double lower_by_ratio = origin_price * 0.98;
            double lower = lower_by_ratio;

            if (price_cage_Amain)
            {
                double lower_by_tick = origin_price - 0.01 * 10;
                lower = std::min(lower_by_ratio, lower_by_tick);
            }

            return static_cast<int64_t>(std::llround(roundTo(lower, 2) * 1000000));
        }

        // 检查买单价格是否在笼子内: price <= 笼子上限
        inline bool isBuyPriceInCage(int64_t price) const
        {
            if (!price_cage_enabled) return true;
            int64_t base_price = getBuyBasePrice();
            if (base_price <= 0) return true;
            return price <= getBuyCageUpperPrice(base_price);
        }

        // 检查卖单价格是否在笼子内: price >= 笼子下限
        inline bool isSellPriceInCage(int64_t price) const
        {
            if (!price_cage_enabled) return true;
            int64_t base_price = getSellBasePrice();
            if (base_price <= 0) return true;
            return price >= getSellCageLowerPrice(base_price);
        }

        inline void mergeLevel(PriceLevel* target, PriceLevel* source)
        {
            if (!target || !source || source->total_volume <= 0) return;

            target->total_volume += source->total_volume;
            target->order_size += source->order_size;
        }

        inline bool moveMainBuyOrdersOutOfCage()
        {
            if (!price_cage_enabled) return false;

            bool moved = false;
            while (true)
            {
                int64_t p = buy_book.bestPrice();
                if (p <= 0) break;
                if (isBuyPriceInCage(p)) break;

                PriceLevel* main = buy_book.find(p);
                if (!main || main->total_volume <= 0)
                {
                    buy_book.erase(p);
                    continue;
                }

                PriceLevel* target = pending_buy_book.getOrCreate(p);
                mergeLevel(target, main);

                main->reset();
                buy_book.erase(p);
                moved = true;
            }

            return moved;
        }

        inline bool moveMainSellOrdersOutOfCage()
        {
            if (!price_cage_enabled) return false;

            bool moved = false;
            while (true)
            {
                int64_t p = sell_book.bestPrice();
                if (p <= 0) break;
                if (isSellPriceInCage(p)) break;

                PriceLevel* main = sell_book.find(p);
                if (!main || main->total_volume <= 0)
                {
                    sell_book.erase(p);
                    continue;
                }

                PriceLevel* target = pending_sell_book.getOrCreate(p);
                mergeLevel(target, main);

                main->reset();
                sell_book.erase(p);
                moved = true;
            }

            return moved;
        }

        // 将临时买单book中回到笼子内的订单移动到正常book
        inline bool movePendingBuyOrdersIntoCage()
        {
            if (!price_cage_enabled) return false;

            bool moved = false;

            // 从价格最低的开始检查（pending_buy_book是less排序，低价优先）
            while (true)
            {
                int64_t p = pending_buy_book.bestPrice(); // 最低价
                if (p <= 0) break;

                if (!isBuyPriceInCage(p))
                    break;

                PriceLevel* pend = pending_buy_book.find(p);
                if (!pend || pend->total_volume <= 0)
                {
                    pending_buy_book.erase(p);
                    continue;
                }

                PriceLevel* target = buy_book.getOrCreate(p);
                mergeLevel(target, pend);

                pend->reset();
                pending_buy_book.erase(p);
                moved = true;
            }

            return moved;
        }

        // 将临时卖单book中回到笼子内的订单移动到正常book
        inline bool movePendingSellOrdersIntoCage()
        {
            if (!price_cage_enabled) return false;

            bool moved = false;

            // 从价格最高的开始检查（pending_sell_book是greater排序，高价优先）
            while (true)
            {
                int64_t p = pending_sell_book.bestPrice(); // 最高价
                if (p <= 0) break;

                if (!isSellPriceInCage(p))
                    break;

                PriceLevel* pend = pending_sell_book.find(p);
                if (!pend || pend->total_volume <= 0)
                {
                    pending_sell_book.erase(p);
                    continue;
                }

                PriceLevel* target = sell_book.getOrCreate(p);
                mergeLevel(target, pend);

                pend->reset();
                pending_sell_book.erase(p);
                moved = true;
            }

            return moved;
        }

        // 检查并重平衡正常book与pending book
        inline void checkAndMovePendingOrders()
        {
            if (!price_cage_enabled) return;
            if (current_time < 93000000L || current_time >= 145700000L) return;
            while (true)
            {
                bool changed = false;
                changed |= moveMainBuyOrdersOutOfCage();
                changed |= moveMainSellOrdersOutOfCage();
                changed |= movePendingBuyOrdersIntoCage();
                changed |= movePendingSellOrdersIntoCage();
                if (!changed) break;
            }
        }

        inline void updateCurrentPrice()
        {
            int64_t best_buy_price = buy_book.bestPrice();
            int64_t best_sell_price = sell_book.bestPrice();
            if (best_buy_price > 0 && best_sell_price > 0
                && best_buy_price < best_sell_price)
            {
                current_bid1_price = best_buy_price;
                current_ask1_price = best_sell_price;
            }
        }

        inline void link(PriceLevel *price_level, int64_t volume)
        {
            price_level->total_volume += volume;
            ++price_level->order_size;
        }

        inline void unlink(PriceLevel *price_level, int64_t volume)
        {
            // 修改总volume
            price_level->total_volume -= volume;
            --price_level->order_size;
        }

        inline void addOrder(int64_t id, int64_t price, int64_t volume, int64_t time, OrderSideType side_type)
        {
            current_id = id;
            current_time = time;
            // 防止重复委托导致订单簿结构被破坏（重复id会导致同一id对应多个节点）
            if (id_meta_map.find(id) != id_meta_map.end())
            {
                LOG_WARNING(app_log::logger(), "dup order id:{}, ignore addOrder", id);
                return;
            }

            // 处理延迟订单
            if (!pending_trade.empty())
            {
                auto _pending_itor = pending_trade.find(id);
                if (_pending_itor != pending_trade.end())
                {
                    if (_pending_itor->second >= volume)
                    {
                        _pending_itor->second -= volume;

                        if (_pending_itor->second == 0)
                            pending_trade.erase(_pending_itor);

                        return;
                    }

                    volume -= _pending_itor->second;
                    pending_trade.erase(_pending_itor);
                }
            }

            if (volume == 0) return;

            // 检查并移动临时book中回到笼子内的订单
            checkAndMovePendingOrders();

            if (side_type == OrderSideType::BUY)
            {
                // 价格笼子检查：买单价格 <= 有效上限（主板max(102%, +10tick)，科创/创业板102%）
                if (price_cage_enabled && time >= 93000000000000L && time < 145700000000000L && !isBuyPriceInCage(price))
                {
                    // 超出笼子范围，加入临时book
                    PriceLevel* lv = pending_buy_book.getOrCreate(price);
                    link(lv, volume);
                }
                else
                {
                    PriceLevel* lv = buy_book.getOrCreate(price);
                    link(lv, volume);
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                // 价格笼子检查：卖单价格 >= 有效下限（主板min(98%, -10tick)，科创/创业板98%）
                if (price_cage_enabled && time >= 93000000000000L && time < 145700000000000L && !isSellPriceInCage(price))
                {
                    // 超出笼子范围，加入临时book
                    PriceLevel* lv = pending_sell_book.getOrCreate(price);
                    link(lv, volume);
                }
                else
                {
                    PriceLevel* lv = sell_book.getOrCreate(price);
                    link(lv, volume);
                }
            }

            // 理论上不会失败
            auto [it, ok] = id_meta_map.emplace(id, OrderLiteMeta{price, volume});
            if (!ok)
            {
                LOG_ERROR(app_log::logger(), "dup order id:{}", id);
                STDTHROW(STD_ERROR_CODE, "dup order id:"<< id, "dup order id:"<< id);
                return;
            }
        }

        inline void dropOrder(int64_t id, int64_t volume, int64_t time, OrderSideType side_type, bool ignore = false)
        {
            current_id = id;
            current_time = time;

            auto it = id_meta_map.find(id);

            // 乱序成交
            if (it == id_meta_map.end())
            {
                if (ignore)
                {
                    return;
                }
                pending_trade[id] += volume;
                LOG_WARNING(app_log::logger(), "get loss order id:{} volume:{}", id, volume);
                return;
            }

            int64_t price = it->second.price;
            int64_t remain_volume = it->second.remaining_volume;
            int64_t target_volume = std::min(remain_volume, volume);
            bool full_remove = (target_volume >= remain_volume);
            
            if (side_type == OrderSideType::BUY)
            {
                PriceLevel *lv = buy_book.find(price);
                if (lv)
                {
                    lv->total_volume -= std::min(lv->total_volume, target_volume);
                    if (full_remove && lv->order_size > 0)
                    {
                        --lv->order_size;
                    }
                    if (lv->total_volume <= 0 || lv->order_size <= 0)
                    {
                        buy_book.erase(price);
                    }
                }
                else
                {
                    PriceLevel *pend_lv = pending_buy_book.find(price);
                    if (pend_lv)
                    {
                        pend_lv->total_volume -= std::min(pend_lv->total_volume, target_volume);
                        if (full_remove && pend_lv->order_size > 0)
                        {
                            --pend_lv->order_size;
                        }
                        if (pend_lv->total_volume <= 0 || pend_lv->order_size <= 0)
                        {
                            pending_buy_book.erase(price);
                        }
                    }
                    else
                    {
                        LOG_WARNING(app_log::logger(), "dropOrder miss buy level id:{} price:{} volume:{}", id, price, target_volume);
                    }
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                PriceLevel *lv = sell_book.find(price);
                if (lv)
                {
                    lv->total_volume -= std::min(lv->total_volume, target_volume);
                    if (full_remove && lv->order_size > 0)
                    {
                        --lv->order_size;
                    }
                    if (lv->total_volume <= 0 || lv->order_size <= 0)
                    {
                        sell_book.erase(price);
                    }
                }
                else
                {
                    PriceLevel *pend_lv = pending_sell_book.find(price);
                    if (pend_lv)
                    {
                        pend_lv->total_volume -= std::min(pend_lv->total_volume, target_volume);
                        if (full_remove && pend_lv->order_size > 0)
                        {
                            --pend_lv->order_size;
                        }
                        if (pend_lv->total_volume <= 0 || pend_lv->order_size <= 0)
                        {
                            pending_sell_book.erase(price);
                        }
                    }
                    else
                    {
                        LOG_WARNING(app_log::logger(), "dropOrder miss sell level id:{} price:{} volume:{}", id, price, target_volume);
                    }
                }
            }

            if (full_remove)
            {
                id_meta_map.erase(it);
            }
            else
            {
                it->second.remaining_volume -= target_volume;
            }

            // 检查并移动临时book中回到笼子内的订单
            checkAndMovePendingOrders();
        }
    
    private:
        // unordered_map<id, pending_volume> 乱序订单
        std::unordered_map<int64_t, int64_t> pending_trade;
        // id->对象
        std::unordered_map<int64_t, OrderLiteMeta> id_meta_map;
        // 跳过的订单id，主要存在于市价单，当对手方无对手价/本方无最优价，则发单立即撤单，视为废单，不进入订单簿
        std::unordered_set<int64_t> skip_ids;

        // 价格队列
        PriceLevelBookGreat buy_book;  // 买单从最高价开始
        PriceLevelBookLess sell_book; // 卖单从最高价开始

        // 价格笼子：临时存放超出笼子范围的订单(排序方式相反，可以更快速找到符合区间)
        PriceLevelBookLess pending_buy_book;   // 从最低价开始检查
        PriceLevelBookGreat pending_sell_book;  // 从最高价开始检查

        // 价格笼子基准价相关
        int64_t last_price = 0; // 最新价
        int64_t current_id = 0; // 最新id
        int64_t per_close = 0;  // 昨收价
        bool price_cage_enabled = false; // 是否开启价格笼子
        bool price_cage_Amain = false;   // 是否主板价格笼子
        int64_t current_time = 0; // 维护最新时间，用于检查订单簿
        int64_t current_bid1_price = 0;
        int64_t current_ask1_price = 0;

        uint8_t _exchange;
        std::string _date = "";
        std::string _code = "";
    
    };

}

#endif
