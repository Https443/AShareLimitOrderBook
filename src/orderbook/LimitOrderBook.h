#ifndef MARKETDATA_ORDERBOOK_LIMITORDERBOOK_H
#define MARKETDATA_ORDERBOOK_LIMITORDERBOOK_H

#include "common/MarketDataStruct.h"
#include "MatchTypes.h"
#include "util/Config.h"
#include <memory>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <cassert>
#include <mutex>
#include "util/logger.h"
#include "util/MyLnxTimer.h"
#include "util/Util.h"
#include <algorithm>
#include <queue>
#include <vector>
#include <functional>
#include "NodePool.h"
#include "PriceBook.h"
#include "PriceCage.h"

namespace marketdata
{
    struct MatchingStartTick
    {
        int32_t _buy_tick;
        int32_t _sell_tick;
    };

    class LimitOrderBook
    {
    public:
        using MatchCallback = std::function<void(const MatchRecord&)>;

    public:
        explicit LimitOrderBook(const std::string &date, 
                                const std::string &code, 
                                std::shared_ptr<OrderPool> poolPtr,
                                const int64_t pre_close_price,
                                const int64_t min_price,
                                const int64_t max_price): 
            _date(date), _code(code), _pool(poolPtr),
            _pre_close_price(pre_close_price),
            _buy_book(min_price, max_price, 10000, code),
            _sell_book(min_price, max_price, 10000, code),
            _limitup_price(max_price), _limitdown_price(min_price),
            _match_tick{-1, -1}
        {
            if (!_pool)
            {
                LOG_ERROR(app_log::logger(), "OrderPool is null, date:{} code:{}", date, code);
                STDTHROW(STD_ERROR_CODE, "OrderPool is null", "OrderPool is null");
            }

            _pending_trade.reserve(200);
            _match_changed_order_node_slots.reserve(200);
            _match_changed_price_levels.reserve(10);

            // 价格笼子（试点阶段）：
            // 科创板从2019年7月22日起即运用价格笼子 + 废单处理，即价格笼子以外直接废单；
            // 创业板在试点注册制推广后从2020年6月12日开始，采用了价格笼子 + 订单暂存 → 等待条件满足再入撮合 的机制。
            // 参考材料：https://www.szse.cn/disclosure/notice/general/t20200612_578381.html

            // 2023年4月10日
            // 全面注册制+主板价格笼子
            // 主板、科创板、创业板均改外超过价格笼子即废单处理 （佐证材料：“当委托进入交易系统时，如果其价格超过有效价格范围或价格限制，该委托将被视为无效。”——引自证监会）

            if (((date >= "20200612" && !code.empty() && code[0] == '3')) ||
                (code.size() >= 2 && code[0] == '6' && code[1] == '8'))
            {
                LOG_INFO(app_log::logger(), "enable price cage, date:{} code:{}", date, code);
                _price_cage_enabled = true;
                _price_cage_Amain = false;
            }
            else if (date >= "20230410")
            {
                LOG_INFO(app_log::logger(), "enable all price cage, date:{} code:{}", date, code);
                _price_cage_enabled = true;
                _price_cage_Amain = true;
            }

            if (!code.empty() && (code.starts_with("00") || code.starts_with("30")))
            {
                _current_exchange = ExchangeType::SZ;
            }
            else if (!code.empty() && (code.starts_with("60") || code.starts_with("68")))
            {
                _current_exchange = ExchangeType::SH;
            }

            // 价格笼子初始化
            _price_cage.init(_pre_close_price, _price_cage_Amain);
            _match_price_cage.init(_pre_close_price, _price_cage_Amain);
        }

        ~LimitOrderBook()
        {
            _pending_trade.clear();
            _skip_ids.clear();
            _match_changed_order_node_slots.clear();
            _match_changed_price_levels.clear();

            releaseBookOrders(_buy_book);
            releaseBookOrders(_sell_book);

            _match_callback = nullptr;
            _current_phase = TradingPhase::PRE_OPEN;
        }

        inline bool isPriceCageEnabled() const { return _price_cage_enabled; }

        inline void setMatchCallback(MatchCallback cb) { _match_callback = std::move(cb); }

        inline int64_t getPreClose() const { return _pre_close_price; }

        inline int64_t getLastPrice() const { return _last_price; }

        inline int64_t getCurrentTime() { return _current_time; }

        const int64_t getBuyBestPrice() const { return _buy_book.bestPrice(); }

        const int64_t getSellBestPrice() const { return _sell_book.bestPrice(); }

        inline bool buyIsEmpty() { return _buy_book.empty(); };

        inline bool sellIsEmpty() { return _sell_book.empty(); };

        inline const PriceLevelBookGreat* getBuyBook() const { return &_buy_book; }

        inline const PriceLevelBookLess* getSellBook() const { return &_sell_book; }

        inline const MatchingStartTick* getMatchStartTick() const { return &_match_tick; }

        inline const int64_t getCurrentTime() const { return _current_time; }

        inline void processOrder(const MDOrder *order)
        {
            if (order == nullptr)
            {
                return;
            }
            int64_t time = order->datetime % 1000000000L;
            TradingPhase phase = determinePhase(time);
            handlePhaseTransition(phase, time);

            if (order->channel_no < 10)
            {
                // 上交所输出的order是未成交的order委托，已成交的的不显示委托，只在trade中一带而过
                if (order->order_type == 'A')
                {
                    int64_t buy_best_price = _buy_book.bestPrice();
                    int64_t sell_best_price = _sell_book.bestPrice();
                    _price_cage.set(buy_best_price, sell_best_price, _last_price);

                    if (order->side == 'B')
                    {
                        addOrder(order->appl_seq_num, order->price, order->volume, time, OrderSideType::BUY, MarketOrderType::NONE);
                    }
                    else if (order->side == 'S')
                    {
                        addOrder(order->appl_seq_num, order->price, order->volume, time, OrderSideType::SELL, MarketOrderType::NONE);
                    }
                    checkAndMovePendingOrders();
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

                    int64_t buy_best_price = _buy_book.bestPrice();
                    int64_t sell_best_price = _sell_book.bestPrice();
                    _price_cage.set(buy_best_price, sell_best_price, _last_price);
                    checkAndMovePendingOrders();
                }
            }
            else if (order->channel_no > 2000)
            {
                int64_t buy_best_price = _buy_book.bestPrice();
                int64_t sell_best_price = _sell_book.bestPrice();
                _price_cage.set(buy_best_price, sell_best_price, _last_price);
                // 买 / 借入
                if (order->side == '1' || order->side == 'G')
                {
                    // 限价单
                    if (order->order_type == '2')
                    {
                        addOrder(order->appl_seq_num, order->price, order->volume, time, OrderSideType::BUY, MarketOrderType::LIMIT);
                        // 集合竞价期间模拟撮合最新价
                        if (phase == TradingPhase::OPEN_CALL_AUCTION)
                        {
                            _last_price = calculateAuctionPrice(true);
                        }
                        else if (phase == TradingPhase::CLOSE_CALL_AUCTION)
                        {
                            _last_price = calculateAuctionPrice(false);
                        }
                    }
                    // 市价单情况特殊处理[除本方最优，其他四个市价单均以对手价成交]
                    else if (order->order_type == '1')
                    {
                        auto sell1_price = _sell_book.bestPrice();
                        if (sell1_price <= 0)
                        {
                            _skip_ids.insert(order->appl_seq_num);
                            return;
                        }
                        addOrder(order->appl_seq_num, sell1_price, order->volume, time, OrderSideType::BUY, MarketOrderType::MARKET);
                    }
                    // 本方最优价格申报，以申报进入交易主机时"集中申报簿中本方队列的最优价格"为其申报价格，集中申报簿中本方无申报的，申报自动撤销。
                    else if (order->order_type == 'U')
                    {
                        auto buy1_price = _buy_book.bestPrice();
                        if (buy1_price <= 0)
                        {
                            _skip_ids.insert(order->appl_seq_num);
                            return;
                        }
                        addOrder(order->appl_seq_num, buy1_price, order->volume, time, OrderSideType::BUY, MarketOrderType::MARKET_BEST_SELF);
                    }
                }
                // 卖 / 出借
                else if (order->side == '2' || order->side == 'F')
                {
                    if (order->order_type == '2')
                    {
                        addOrder(order->appl_seq_num, order->price, order->volume, time, OrderSideType::SELL, MarketOrderType::LIMIT);
                        // 集合竞价期间模拟撮合最新价
                        if (phase == TradingPhase::OPEN_CALL_AUCTION)
                        {
                            _last_price = calculateAuctionPrice(true);
                        }
                        else if (phase == TradingPhase::CLOSE_CALL_AUCTION)
                        {
                            _last_price = calculateAuctionPrice(false);
                        }
                    }
                    else if (order->order_type == '1')
                    {
                        auto buy1_price = _buy_book.bestPrice();
                        if (buy1_price <= 0)
                        {
                            _skip_ids.insert(order->appl_seq_num);
                            return;
                        }
                        addOrder(order->appl_seq_num, buy1_price, order->volume, time, OrderSideType::SELL, MarketOrderType::MARKET);
                    }
                    else if (order->order_type == 'U')
                    {
                        auto sell1_price = _sell_book.bestPrice();
                        if (sell1_price <= 0)
                        {
                            _skip_ids.insert(order->appl_seq_num);
                            return;
                        }
                        addOrder(order->appl_seq_num, sell1_price, order->volume, time, OrderSideType::SELL, MarketOrderType::MARKET_BEST_SELF);
                    }
                }
                else
                {
                    LOG_ERROR(app_log::logger(), "not support order side:{}, order type:{}, order id:{}", order->side, order->order_type, order->appl_seq_num);
                }
                // 价格笼子边界可能变化，刷新笼内 best。
                checkAndMovePendingOrders();

                // 模拟撮合
                tryMatchCrossedMainBook(order->datetime);
            }
        };

        inline void processTrade(const MDTrade *trade)
        {
            if (trade == nullptr)
            {
                return;
            }
            int64_t time = trade->datetime % 1000000000L;
            TradingPhase phase = determinePhase(time);
            handlePhaseTransition(phase, time);

            if (trade->channel_no < 10)
            {
                // 主买 bid_appl_seq_num > offer_appl_seq_num or side=B 只存在卖单
                // 主卖 bid_appl_seq_num < offer_appl_seq_num or side=S 只存在买单

                // 连续竞价阶段
                if (time >= 93000000L && time < 145700000L)
                {
                    // 更新最新成交价
                    _last_price = trade->price;

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
                        dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY, false, true);
                        dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL, false, true);
                    }
                    else
                    {
                        LOG_WARNING(app_log::logger(), "code:{} biz_index:{} bid id:{} offer id:{} side is unknow", trade->security_code, trade->biz_index, trade->bid_appl_seq_num, trade->offer_appl_seq_num);
                        dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY, false, true);
                        dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL, false, true);
                    }
                }
                // 开盘集合竞价阶段
                else if (time < 93000000L)
                {
                    dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY);
                    dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL);
                }
                // 收盘集合竞价阶段
                else if (time >= 145700000L)
                {
                    dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY, false, true);
                    dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL, false, true);
                }
                if (!hasCrossedMainBook())
                {
                    int64_t buy_best_price = _buy_book.bestPrice();
                    int64_t sell_best_price = _sell_book.bestPrice();
                    _price_cage.set(buy_best_price, sell_best_price, _last_price);
                    // 价格笼子边界可能变化，刷新笼内 best。
                    checkAndMovePendingOrders();
                }
            }
            else if (trade->channel_no > 2000)
            {
                int64_t buy_best_price = _buy_book.bestPrice();
                int64_t sell_best_price = _sell_book.bestPrice();
                _price_cage.set(buy_best_price, sell_best_price, _last_price);

                // 撤单
                if (trade->exec_type == '4')
                {
                    // 获取订单编号，撤买/撤卖
                    if (trade->bid_appl_seq_num != 0)
                    {
                        if (_skip_ids.find(trade->bid_appl_seq_num) != _skip_ids.end()) return;
                        dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY, true);
                    }
                    else if (trade->offer_appl_seq_num != 0)
                    {
                        if (_skip_ids.find(trade->offer_appl_seq_num) != _skip_ids.end()) return;
                        dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL, true);
                    }
                    if (!hasCrossedMainBook())
                    {
                        int64_t buy_best_price = _buy_book.bestPrice();
                        int64_t sell_best_price = _sell_book.bestPrice();
                        _price_cage.set(buy_best_price, sell_best_price, _last_price);
                        // 价格笼子边界可能变化，刷新笼内 best。
                        checkAndMovePendingOrders();
                    }
                    // 模拟撮合
                    tryMatchCrossedMainBook(trade->datetime);
                }
                // 成交
                else if (trade->exec_type == 'F')
                {
                    // 更新最新成交价
                    _last_price = trade->price;
                    dropOrder(trade->bid_appl_seq_num, trade->volume, time, OrderSideType::BUY);
                    dropOrder(trade->offer_appl_seq_num, trade->volume, time, OrderSideType::SELL);
                    if (!hasCrossedMainBook())
                    {
                        int64_t buy_best_price = _buy_book.bestPrice();
                        int64_t sell_best_price = _sell_book.bestPrice();
                        _price_cage.set(buy_best_price, sell_best_price, _last_price);
                        // 价格笼子边界可能变化，刷新笼内 best。
                        checkAndMovePendingOrders();
                    }
                    // 真实成交会改变主簿；若上一条订单触发过模拟撮合，需要在这里清理或继续撮合。
                    tryMatchCrossedMainBook(trade->datetime);
                }
            }
        };

        inline const std::vector<std::pair<int64_t, const PriceLevel*>> getBuyTopN(int n) const
        {
            return getTopN<PriceLevelBookGreat>(_buy_book, n);
        }

        inline const std::vector<std::pair<int64_t, const PriceLevel*>> getSellTopN(int n) const
        {
            return getTopN<PriceLevelBookLess>(_sell_book, n);
        }

        inline void getBuyTopN(int n, std::pair<int64_t, const PriceLevel*> *out) const
        {
            _buy_book.topNToBuffer(n, out);
            return;
        }

        inline void getSellTopN(int n, std::pair<int64_t, const PriceLevel*> *out) const
        {
            _sell_book.topNToBuffer(n, out);
            return;
        }

        // 按价格档位内的时间优先顺序遍历订单节点，对外屏蔽slot链表细节。
        template<typename Fn>
        inline void forEachLevelOrder(const PriceLevel* level, Fn&& fn) const
        {
            if (!level || !_pool)
            {
                return;
            }

            const OrderPool* order_pool = _pool.get();
            uint32_t slot = level->head_slot;
            while (slot != OrderNode::INVALID_SLOT)
            {
                const OrderNode* node = order_pool->getBySlot(slot);
                if (!node)
                {
                    break;
                }
                fn(node);
                slot = node->next_slot;
            }
        }

        inline const std::string printBuyPriceVolume(int64_t id, size_t count = 0) const
        {
            std::stringstream ss;
            ss << "buy " << _buy_book.printPriceVolume(id, count) << "\n";
            return ss.str();
        }

        inline const std::string printSellPriceVolume(int64_t id, size_t count = 0) const
        {
            std::stringstream ss;
            ss << "sell " << _sell_book.printPriceVolume(id, count) << "\n";
            return ss.str();
        }

    
    private:
        inline static double roundTo(double value, int digits)
        {
            double scale = std::pow(10.0, digits);
            return std::round(value * scale) / scale;
        }

        inline PriceLevel* getPriceLevel(const OrderNode* node) const
        {
            if (!node || node->price_level_ptr == nullptr)
            {
                return nullptr;
            }
            return node->price_level_ptr;
        }

        inline OrderNode* getHeadNode(const PriceLevel* level) const
        {
            if (!level || !_pool || level->head_slot == PriceLevel::INVALID_SLOT)
            {
                return nullptr;
            }
            return _pool->getBySlot(level->head_slot);
        }

        inline OrderNode* getNextNode(const OrderNode* node) const
        {
            if (!node || !_pool || node->next_slot == OrderNode::INVALID_SLOT)
            {
                return nullptr;
            }
            return _pool->getBySlot(node->next_slot);
        }

        template<typename BookType>
        inline int64_t tickToActivePrice(const BookType& book, int32_t tick) const
        {
            return book.findByTick(tick) ? book.tickToPrice(tick) : 0L;
        }

        template<typename BookType>
        inline bool linkNodeToBook(BookType& book, int64_t price, OrderNode* node)
        {
            PriceLevel* level = book.getOrCreate(price);
            if (!level)
            {
                LOG_ERROR(app_log::logger(), "getOrCreate price level failed, date:{} code:{} price:{}", _date, _code, price);
                return false;
            }
            _pool->link(level, node);
            return true;
        }

        inline void eraseEmptyLevel(OrderSideType side_type, int64_t price, PriceLevel* level)
        {
            if (!level || level->total_volume > 0)
            {
                return;
            }

            if (side_type == OrderSideType::BUY)
            {
                PriceLevel* lv = _buy_book.find(price);
                if (lv && lv == level)
                {
                    _buy_book.erase(price);
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                PriceLevel* lv = _sell_book.find(price);
                if (lv && lv == level)
                {
                    _sell_book.erase(price);
                }
            }
        }

        // 订单节点的 id 索引已经下沉到共享 OrderPool，
        // 析构时按本订单簿的各个价格簿遍历释放，避免误操作同 shard 里其他 orderbook 的节点。
        template<typename BookType>
        inline void releaseBookOrders(BookType& book) noexcept
        {
            if (!_pool)
            {
                book.clear();
                return;
            }

            std::vector<std::pair<int64_t, const PriceLevel*>> buf(book.size());
            const int count = book.allToBuffer(buf.data());
            for (int i = 0; i < count; ++i)
            {
                OrderNode* node = getHeadNode(buf[static_cast<size_t>(i)].second);
                while (node)
                {
                    OrderNode* next = getNextNode(node);
                    _pool->free(node);
                    node = next;
                }
            }

            book.clear();
        }

        inline bool checkMoveBuyCage()
        {
            if (!_price_cage_enabled) return false;
            int64_t base_price = _price_cage.getBuyBasePrice();
            if (base_price <= 0)
            {
                return _buy_book.clearCage();
            }

            return _buy_book.refreshBestByCage([this](int64_t price) 
            {
                return _price_cage.isBuyPriceInCage(price);
            });
        }

        inline bool checkMoveSellCage()
        {
            if (!_price_cage_enabled) return false;
            int64_t base_price = _price_cage.getSellBasePrice();
            if (base_price <= 0)
            {
                return _sell_book.clearCage();
            }

            const int64_t lower_price = _price_cage.getSellCageLowerPrice(base_price);
            return _sell_book.refreshBestByCage([this](int64_t price) 
            {
                return _price_cage.isSellPriceInCage(price);
            });
        }

        // 按价格笼子刷新买卖簿的笼内 best_tick_ 与笼外 cage_tick_。
        inline void checkAndMovePendingOrders()
        {
            if (!_price_cage_enabled)
            {
                return;
            }
            if (_current_time < 93000000L || _current_time >= 145700000L)
            {
                _buy_book.clearCage();
                _sell_book.clearCage();
                return;
            }

            while (true)
            {
                bool changed = false;
                changed |= checkMoveBuyCage();
                changed |= checkMoveSellCage();
                if (!changed) break;
            }
        }

        // ============ 模拟撮合核心逻辑 ============

        /**
         * @brief 生成一笔成交记录并回调
         * @param bid_id 买单ID
         * @param offer_id 卖单ID
         * @param price 成交价格
         * @param volume 成交量
         * @param datetime 成交时间
         * @param side 主动方（'B'=买方主动，'S'=卖方主动）
         * @note 逐笔成交回调，如果注册回调，就进行回调，否则直接跳过
         */
        inline void generateMatch(
            int64_t bid_id, 
            int64_t offer_id, 
            int64_t price,
            int64_t volume, 
            int64_t datetime, 
            char side)
        {
            if (_match_callback)
            {
                MatchRecord record;
                record.match_id = ++_match_id_counter;
                record.bid_order_id = bid_id;
                record.offer_order_id = offer_id;
                record.price = price;
                record.volume = volume;
                record.datetime = datetime;
                record.side = side;
                _match_callback(record);
            }
        }

        // ============ 连续竞价撮合核心实现 ============

        inline bool isContinuousCageTime() const
        {
            return _current_time >= 93000000L && _current_time < 145700000L;
        }

        /**
         * @brief 处理主订单簿中的价格相交（bid1 >= ask1）
         * @param datetime 成交时间
         * @note 用于订单触发自动撮合，直到最优价不再相交
         */
        inline int64_t matchCrossedMainBook(int64_t datetime)
        {
            int32_t buy_tick = _buy_book.bestTick();
            int64_t buy_price = tickToActivePrice(_buy_book, buy_tick);
            int32_t sell_tick = _sell_book.bestTick();
            int64_t sell_price = tickToActivePrice(_sell_book, sell_tick);

            int64_t current_buy_price = buy_price;
            int64_t current_sell_price = sell_price;

            int64_t consumed = 0;
            while (true)
            {
                // 涨跌停则跳过
                if (buy_price <= 0 || sell_price <= 0) break;
                // 撮合完成则跳过
                if (buy_price < sell_price)
                {
                    _match_tick._buy_tick = buy_tick;
                    _match_tick._sell_tick = sell_tick;
                    break;
                }

                PriceLevel *buy_level = _buy_book.find(buy_price);
                PriceLevel *sell_level = _sell_book.find(sell_price);
                if (!buy_level || !sell_level) break;
                
                // 获取订单队列
                OrderNode* buy_node = nullptr;
                bool buy_null_level = false;
                uint32_t buy_node_slot = OrderNode::INVALID_SLOT;
                uint32_t buy_node_head_slot = OrderNode::INVALID_SLOT;
                while (true)
                {
                    if (buy_level->head_slot == PriceLevel::INVALID_SLOT) [[unlinkly]]
                    {
                        buy_node = nullptr;
                        break;
                    }
                    if (buy_level->matched || buy_level->match_total_volume <= 0) [[unlinkly]]
                    {
                        buy_node = nullptr;
                        buy_null_level = true;
                        break;
                    }
                    if (buy_node_head_slot == OrderNode::INVALID_SLOT)  [[unlinkly]]
                    {
                        buy_node_head_slot = buy_level->head_slot;
                        buy_node_slot = buy_node_head_slot;
                    }
                    buy_node = _pool->getBySlot(buy_node_slot);
                    if (!buy_node || !buy_node->matched || buy_node->remaining_match_volume > 0)
                    {
                        break;
                    }
                    buy_node_slot = buy_node->next_slot;
                    if (buy_node_head_slot == buy_node_slot)  [[unlinkly]]
                    {
                        buy_node = nullptr;
                        buy_null_level = true;
                        break;
                    }
                }
                if (buy_null_level)
                {
                    buy_tick = _buy_book.nextActiveTick(buy_tick);
                    buy_price = tickToActivePrice(_buy_book, buy_tick);
                    continue;
                }

                OrderNode* sell_node = nullptr;
                bool sell_null_level = false;
                uint32_t sell_node_slot = OrderNode::INVALID_SLOT;
                uint32_t sell_node_head_slot = OrderNode::INVALID_SLOT;
                while (true)
                {
                    if (sell_level->head_slot == PriceLevel::INVALID_SLOT) [[unlinkly]]
                    {
                        sell_node = nullptr;
                        break;
                    }
                    if (sell_level->matched || sell_level->match_total_volume <= 0) [[unlinkly]]
                    {
                        sell_node = nullptr;
                        sell_null_level = true;
                        break;
                    }
                    if (sell_node_head_slot == OrderNode::INVALID_SLOT) [[unlinkly]]
                    {
                        sell_node_head_slot = sell_level->head_slot;
                        sell_node_slot = sell_node_head_slot;
                    }
                    sell_node = _pool->getBySlot(sell_node_slot);
                    if (!sell_node || !sell_node->matched || sell_node->remaining_match_volume > 0)
                    {
                        break;
                    }
                    sell_node_slot = sell_node->next_slot;
                    if (sell_node_head_slot == sell_node_slot) [[unlinkly]]
                    {
                        sell_node = nullptr;
                        sell_null_level = true;
                        break;
                    }
                }
                if (sell_null_level)
                {
                    sell_tick = _sell_book.nextActiveTick(sell_tick);
                    sell_price = tickToActivePrice(_sell_book, sell_tick);
                    continue;
                }
                if (!buy_node || !sell_node) break;

                // 获取这一次的撮合量
                int64_t trade_vol = std::min(buy_node->remaining_match_volume, sell_node->remaining_match_volume);
                if (trade_vol <= 0) break;

                // 获取是否主买/主卖
                bool is_buy = buy_node->id > sell_node->id;
                // 如果主买则以对手价成交
                int64_t match_price = is_buy ? sell_price : buy_price;
                char side = is_buy ? 'B' : 'S';
                generateMatch(buy_node->id, sell_node->id, match_price, trade_vol, datetime, side);
                
                buy_node->remaining_match_volume -= trade_vol;
                sell_node->remaining_match_volume -= trade_vol;
                buy_level->match_total_volume -= trade_vol;
                sell_level->match_total_volume -= trade_vol;
                _match_last_price = match_price;

                _match_changed_order_node_slots.insert(buy_node->self_slot);
                _match_changed_order_node_slots.insert(sell_node->self_slot);
                _match_changed_price_levels.insert(buy_level);
                _match_changed_price_levels.insert(sell_level);

                if (buy_node->remaining_match_volume <= 0)
                {
                    buy_node->matched = true;
                    buy_node->remaining_match_volume = 0;
                    --buy_level->match_order_size;
                }
                if (sell_node->remaining_match_volume <= 0)
                {
                    sell_node->matched = true;
                    sell_node->remaining_match_volume = 0;
                    --sell_level->match_order_size;
                }
                if (buy_level->match_total_volume <= 0)
                {
                    buy_level->matched = true;
                    buy_level->match_total_volume = 0;

                    buy_tick = _buy_book.nextActiveTick(buy_tick);
                    buy_price = tickToActivePrice(_buy_book, buy_tick);
                }
                if (sell_level->match_total_volume <= 0)
                {
                    sell_level->matched = true;
                    sell_level->match_total_volume = 0;

                    sell_tick = _sell_book.nextActiveTick(sell_tick);
                    sell_price = tickToActivePrice(_sell_book, sell_tick);
                }

                // 撮合完成后，检查盘口是否变更，如果盘口变更则需要检查是否有价格入笼
                if (current_buy_price != buy_price || current_sell_price != sell_price)
                {
                    _match_price_cage.set(buy_price, sell_price, _match_last_price);
                    int64_t buy_base_price = _match_price_cage.getBuyBasePrice();
                    if (buy_base_price <= 0) break;
                    int64_t upper_price = _match_price_cage.getBuyCageUpperPrice(buy_base_price);
                    if (upper_price > _limitup_price)
                    {
                        upper_price = _limitup_price;
                    }
                    buy_tick = _buy_book.priceToTickChecked(upper_price);
                    buy_tick = _buy_book.lastTick(buy_tick);
                    buy_price = tickToActivePrice(_buy_book, buy_tick);
                    
                    int64_t sell_base_price = _match_price_cage.getSellBasePrice();
                    if (sell_base_price <= 0) break;
                    int64_t lower_price = _match_price_cage.getSellCageLowerPrice(sell_base_price);
                    if (lower_price < _limitdown_price)
                    {
                        lower_price = _limitdown_price;
                    }
                    sell_tick = _sell_book.priceToTickChecked(lower_price);
                    sell_tick = _sell_book.lastTick(sell_tick);
                    sell_price = tickToActivePrice(_sell_book, sell_tick);
                }
                current_buy_price = buy_price;
                current_sell_price = sell_price;
                consumed += trade_vol;
            }
            _match_tick._buy_tick = buy_tick;
            _match_tick._sell_tick = sell_tick;
            return consumed;
        }

        inline bool hasCrossedMainBook() const
        {
            int64_t bid1 = _buy_book.bestPrice();
            int64_t ask1 = _sell_book.bestPrice();
            return bid1 > 0 && ask1 > 0 && bid1 >= ask1;
        }

        inline void resetAllMatchStatus()
        {
            auto resetBookMatchStatus = [this](auto &book)
            {
                for (int32_t tick = book.bestTick(); tick >= 0; tick = book.nextActiveTick(tick))
                {
                    PriceLevel *level = book.findByTick(tick);
                    if (!level)
                    {
                        continue;
                    }

                    for (uint32_t slot = level->head_slot; slot != OrderNode::INVALID_SLOT; )
                    {
                        OrderNode *order_node = _pool->getBySlot(slot);
                        if (!order_node || !order_node->is_use)
                        {
                            LOG_WARNING(app_log::logger(),
                                        "resetAllMatchStatus hit invalid order slot, date:{} code:{} price:{} slot:{}",
                                        _date, _code, level->price, slot);
                            break;
                        }

                        order_node->resetMatchStatus();
                        slot = order_node->next_slot;
                    }

                    level->resetMatchStatus();
                }
            };

            resetBookMatchStatus(_buy_book);
            resetBookMatchStatus(_sell_book);

            _match_changed_order_node_slots.clear();
            _match_changed_price_levels.clear();
            _match_status_reset = false;
            _match_tick._buy_tick = _buy_book.bestTick();
            _match_tick._sell_tick = _sell_book.bestTick();
        }

        inline void tryMatchCrossedMainBook(int64_t datetime)
        {
            if (!isContinuousCageTime()) return;
            
            bool need_match = hasCrossedMainBook();
            if (need_match)
            {
                matchCrossedMainBook(datetime);
                _match_status_reset = true;
            }
            else if (!_match_changed_order_node_slots.empty()
                     || !_match_changed_price_levels.empty())
            {
                for (auto &slot : _match_changed_order_node_slots)
                {
                    OrderNode *order_node = _pool->getBySlot(slot);
                    if (order_node)
                    {
                        order_node->resetMatchStatus();
                    }
                }
                _match_changed_order_node_slots.clear();

                for (auto level : _match_changed_price_levels)
                {
                    if (level)
                    {
                        level->resetMatchStatus();
                    }
                }
                _match_changed_price_levels.clear();
                _match_status_reset = false;

                _match_tick._buy_tick = _buy_book.bestTick();
                _match_tick._sell_tick = _sell_book.bestTick();
            }
        }

        // ============ 集合竞价撮合 ============

        /**
         * @brief 计算集合竞价成交价
         * @param is_open_call 深交所有效，是否开盘集合竞价/收盘集合竞价
         * @return 集合竞价成交价（单位：微元），无法形成成交价返回0
         * @note 选择规则：
         *       1. 成交量最大的价格
         *       2. 成交量相同时，选择不平衡量最小的价格
         *       3. 仍相同时：
         *          - 上交所取中间价（并按最小价位单位取整）
         *          - 深交所开盘取最接近前收，收盘取最接近最近成交
         */
        inline int64_t calculateAuctionPrice(bool is_open_call) const
        {
            if (_buy_book.empty() || _sell_book.empty())
                return 0;

            struct AuctionCandidate
            {
                int64_t price = 0;
                int64_t match_volume = 0;
                int64_t imbalance = std::numeric_limits<int64_t>::max();
            };

            // Step 1: 从 levels 直接收集有效档位，各自排序一次，计算前缀累计量
            // buy_sorted: 降序，buy_prefix[i] = 所有 price >= buy_sorted[i].price 的累计买量
            std::vector<std::pair<int64_t, int64_t>> buy_sorted;
            buy_sorted.reserve(_buy_book.size());
            std::vector<std::pair<int64_t, const PriceLevel*>> buy_book_buf(_buy_book.size());
            _buy_book.allToBuffer(buy_book_buf.data());
            for (const auto& [p, lv] : buy_book_buf)
            {
                if (lv->total_volume > 0)
                    buy_sorted.emplace_back(p, lv->total_volume);
            }
            std::sort(buy_sorted.begin(), buy_sorted.end(),
                      [](const auto& a, const auto& b){ return a.first > b.first; });

            // sell_sorted: 升序，sell_prefix[i] = 所有 price <= sell_sorted[i].price 的累计卖量
            std::vector<std::pair<int64_t, int64_t>> sell_sorted;
            sell_sorted.reserve(_sell_book.size());
            std::vector<std::pair<int64_t, const PriceLevel*>> sell_book_buf(_sell_book.size());
            _sell_book.allToBuffer(sell_book_buf.data());
            for (const auto& [p, lv] : sell_book_buf)
            {
                if (lv->total_volume > 0)
                    sell_sorted.emplace_back(p, lv->total_volume);
            }
            std::sort(sell_sorted.begin(), sell_sorted.end(),
                      [](const auto& a, const auto& b){ return a.first < b.first; });

            if (buy_sorted.empty() || sell_sorted.empty())
                return 0;

            std::vector<int64_t> buy_prefix(buy_sorted.size());
            buy_prefix[0] = buy_sorted[0].second;
            for (size_t i = 1; i < buy_sorted.size(); ++i)
                buy_prefix[i] = buy_prefix[i - 1] + buy_sorted[i].second;

            std::vector<int64_t> sell_prefix(sell_sorted.size());
            sell_prefix[0] = sell_sorted[0].second;
            for (size_t i = 1; i < sell_sorted.size(); ++i)
                sell_prefix[i] = sell_prefix[i - 1] + sell_sorted[i].second;

            // 二分查找：price >= P 的买量（buy_sorted 降序，找最后一个 >= P 的下标）
            auto getBuyVol = [&](int64_t P) -> int64_t {
                int lo = 0, hi = static_cast<int>(buy_sorted.size()) - 1, res = -1;
                while (lo <= hi)
                {
                    int mid = lo + (hi - lo) / 2;
                    if (buy_sorted[mid].first >= P) { res = mid; lo = mid + 1; }
                    else { hi = mid - 1; }
                }
                return res < 0 ? 0 : buy_prefix[res];
            };

            // 二分查找：price <= P 的卖量（sell_sorted 升序，找最后一个 <= P 的下标）
            auto getSellVol = [&](int64_t P) -> int64_t {
                int lo = 0, hi = static_cast<int>(sell_sorted.size()) - 1, res = -1;
                while (lo <= hi)
                {
                    int mid = lo + (hi - lo) / 2;
                    if (sell_sorted[mid].first <= P) { res = mid; lo = mid + 1; }
                    else { hi = mid - 1; }
                }
                return res < 0 ? 0 : sell_prefix[res];
            };

            // Step 2: 遍历买卖档位的并集，每个候选价格 O(log N) 查询
            std::unordered_set<int64_t> all_prices;
            all_prices.reserve(buy_sorted.size() + sell_sorted.size());
            for (const auto& [p, _] : buy_sorted) all_prices.insert(p);
            for (const auto& [p, _] : sell_sorted) all_prices.insert(p);

            int64_t max_volume = 0;
            int64_t min_imbalance = std::numeric_limits<int64_t>::max();
            std::vector<AuctionCandidate> candidates;
            candidates.reserve(all_prices.size());

            for (int64_t price : all_prices)
            {
                int64_t buy_vol  = getBuyVol(price);
                int64_t sell_vol = getSellVol(price);
                int64_t match_vol = std::min(buy_vol, sell_vol);
                if (match_vol <= 0) continue;

                int64_t imbalance = std::abs(buy_vol - sell_vol);
                candidates.push_back({price, match_vol, imbalance});
                if (match_vol > max_volume) max_volume = match_vol;
            }

            if (max_volume == 0 || candidates.empty()) return 0;

            // Step 3: 筛选最大成交量 + 最小不平衡量
            for (const auto& c : candidates)
            {
                if (c.match_volume == max_volume && c.imbalance < min_imbalance)
                    min_imbalance = c.imbalance;
            }

            std::vector<AuctionCandidate> finalists;
            finalists.reserve(candidates.size());
            for (const auto& c : candidates)
            {
                if (c.match_volume == max_volume && c.imbalance == min_imbalance)
                    finalists.push_back(c);
            }

            if (finalists.empty()) return 0;
            if (finalists.size() == 1) return finalists.front().price;

            // Step 4: 交易所规则打破平手
            if (_current_exchange == ExchangeType::SH)
            {
                // 上交所：取中间价，按最小变动单位取整
                std::sort(finalists.begin(), finalists.end(),
                    [](const AuctionCandidate& a, const AuctionCandidate& b){ return a.price < b.price; });
                double middle_price = (static_cast<double>(finalists.front().price) / 1000000.0 +
                                       static_cast<double>(finalists.back().price) / 1000000.0) / 2.0;
                return static_cast<int64_t>(roundTo(middle_price, 2) * 1000000);
            }
            else if (_current_exchange == ExchangeType::SZ)
            {
                // 深交所：取最接近基准价的档位（开盘→前收；收盘→最近成交）
                int64_t ref_price = _pre_close_price;
                if (!is_open_call)
                    ref_price = (_last_price > 0) ? _last_price : _pre_close_price;

                std::sort(finalists.begin(), finalists.end(),
                    [ref_price](const AuctionCandidate& a, const AuctionCandidate& b){
                        return std::abs(a.price - ref_price) < std::abs(b.price - ref_price);
                    });
                return finalists.front().price;
            }
            return 0;
        }

        inline void addOrder(int64_t id, int64_t price, int64_t volume, int64_t time, OrderSideType side_type, MarketOrderType order_type)
        {
            _current_time = time;
            // OrderPool 内部已经维护全局 id->slot 索引，这里直接复用
            if (_pool && _pool->find(id) != nullptr)
            {
                LOG_WARNING(app_log::logger(), "dup order id:{}, ignore addOrder", id);
                return;
            }

            // 处理延迟订单
            if (!_pending_trade.empty())
            {
                auto _pending_itor = _pending_trade.find(id);
                if (_pending_itor != _pending_trade.end())
                {
                    if (_pending_itor->second >= volume)
                    {
                        _pending_itor->second -= volume;

                        if (_pending_itor->second == 0)
                            _pending_trade.erase(_pending_itor);

                        return;
                    }

                    volume -= _pending_itor->second;
                    _pending_trade.erase(_pending_itor);
                }
            }

            if (volume == 0) return;

            OrderNode *op = _pool->alloc(id);
            if (!op)
            {
                return;
            }

            op->price = price;
            op->volume = volume;
            op->remaining_match_volume = volume;
            op->time = time;
            op->order_type = order_type;
            op->side_type = side_type;

            if (side_type == OrderSideType::BUY)
            {
                if (!linkNodeToBook(_buy_book, price, op))
                {
                    _pool->free(op);
                    return;
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                if (!linkNodeToBook(_sell_book, price, op))
                {
                    _pool->free(op);
                    return;
                }
            }
        }

        inline void syncMatchStateAfterBookUpdate(OrderNode* order_node, PriceLevel* level, bool is_cancel)
        {
            if (_match_status_reset && !is_cancel)
            {
                return;
            }

            if (order_node)
            {
                order_node->resetMatchStatus();
            }
            if (level)
            {
                level->resetMatchStatus();
            }
        }

        inline void dropOrder(int64_t id, int64_t volume, int64_t time, OrderSideType side_type, bool is_cancel = false, bool ignore = false)
        {
            _current_time = time;
            OrderNode* _order_node = _pool ? _pool->find(id) : nullptr;

            // 乱序成交
            if (_order_node == nullptr)
            {
                if (ignore)
                {
                    return;
                }
                _pending_trade[id] += volume;
                LOG_WARNING(app_log::logger(), "get loss order id:{} volume:{}", id, volume);
                return;
            }

            if (_order_node->price == -1)
            {
                LOG_ERROR(app_log::logger(), "drop order, null order node, id:{} volume:{}", id, volume);
                STDTHROW(STD_ERROR_CODE, "drop order, null order node, id:"<<id<<" volume:"<<volume, "drop order, null order node, id:"<<id<<" volume:"<<volume);
                return;
            }

            // 深交所 发单1000 成交500 剩余500撤单，这种情况下撤单的volume为1000，如果不特殊处理会导致total_volume超减
            // 取order->volume和need drop volume的最小值应对上述情况
            int64_t target_volume = std::min(_order_node->volume, volume);
            // 如果order的volume<=需要删除的vol则直接删除
            if (_order_node->volume <= target_volume)
            {
                PriceLevel* _price_level = getPriceLevel(_order_node);
                _pool->unlink(_order_node);
                syncMatchStateAfterBookUpdate(nullptr, _price_level, is_cancel);

                // 如果 volume 为 0 则删除价格档位；笼外订单仍在同一本簿内。
                eraseEmptyLevel(side_type, _order_node->price, _price_level);
                // 释放对象
                _pool->free(_order_node);
            }
            // 部分成交/撤单，直接修改订单中的volume
            else
            {
                // 对应价格档位总量减
                PriceLevel* level = getPriceLevel(_order_node);
                if (level)
                {
                    level->total_volume -= target_volume;
                }
                // OrderNode量减
                _order_node->volume -= target_volume;
                syncMatchStateAfterBookUpdate(_order_node, level, is_cancel);
            }
        }

        /**
         * @brief 根据时间判断当前交易阶段
         * @param time 时间戳（格式：HHMMSSmmm，如93000000表示9:30:00.000）
         * @return 对应的交易阶段枚举值
         * @note 交易阶段划分：
         *       - PRE_OPEN: <9:15 或 11:30-13:00（午休）
         *       - OPEN_CALL_AUCTION: 9:15-9:25（开盘集合竞价）
         *       - OPEN_CALL_MATCH: 9:25-9:30（开盘集合竞价撮合）
         *       - CONTINUOUS_TRADING: 9:30-11:30, 13:00-14:57（连续竞价）
         *       - CLOSE_CALL_AUCTION: 14:57-15:00（收盘集合竞价）
         *       - CLOSED: >=15:00
         */
        TradingPhase determinePhase(int64_t time) const
        {
            if (time < 91500000L)
                return TradingPhase::PRE_OPEN;
            else if (time >= 91500000L && time < 92500000L)
                return TradingPhase::OPEN_CALL_AUCTION;
            else if (time >= 92500000L && time < 93000000L)
                return TradingPhase::OPEN_CALL_MATCH;
            else if (time >= 93000000L && time < 113000000L)
                return TradingPhase::CONTINUOUS_TRADING;
            else if (time >= 113000000L && time < 130000000L)
                return TradingPhase::PRE_OPEN;  // 午休
            else if (time >= 130000000L && time < 145700000L)
                return TradingPhase::CONTINUOUS_TRADING;
            else if (time >= 145700000L && time < 150000000L)
                return TradingPhase::CLOSE_CALL_AUCTION;
            else
                return TradingPhase::CLOSED;
        }

        inline void handlePhaseTransition(TradingPhase new_phase, int64_t time)
        {
            // 最后一个订单是92500000L前收到，下一个订单是93000000L
            if (_current_phase == TradingPhase::OPEN_CALL_MATCH &&
                new_phase == TradingPhase::CONTINUOUS_TRADING)
            {
                resetAllMatchStatus();
            }

            _current_phase = new_phase;
        }

        template<typename Book>
        inline const std::vector<std::pair<int64_t, const PriceLevel*>> getTopN(const Book &book, int n) const
        {
            if (n <= 0)
            {
                return {};
            }

            std::vector<std::pair<int64_t, const PriceLevel*>> out(static_cast<size_t>(n));
            const int count = book.topNToBuffer(n, out.data());
            out.resize(static_cast<size_t>(count));
            return out;
        }
    
    private:
        // unordered_map<id, pending_volume> 乱序订单
        std::unordered_map<int64_t, int64_t> _pending_trade;
        // 对象池
        std::shared_ptr<OrderPool> _pool;
        // 跳过的订单id，主要存在于市价单，当对手方无对手价/本方无最优价，则发单立即撤单，视为废单，不进入订单簿
        std::unordered_set<int64_t> _skip_ids;

        // 价格队列
        PriceLevelBookGreat _buy_book; // 买单从最高价开始
        PriceLevelBookLess _sell_book; // 卖单从最高价开始

        // 价格笼子基准价相关
        int64_t _last_price = 0; // 最新价
        int64_t _pre_close_price = 0;  // 昨收价
        int64_t _limitup_price = 0;  // 涨停价
        int64_t _limitdown_price = 0;  // 跌停价
        bool _price_cage_enabled = false; // 是否开启价格笼子
        bool _price_cage_Amain = false;   // 是否主板价格笼子
        int64_t _current_time = 0; // 维护最新时间，用于检查订单簿
        PriceCage _price_cage;
        MatchingStartTick _match_tick;
        bool _match_status_reset = false;

        // 模拟撮合
        int64_t _match_id_counter = 0;  // 撮合自增id
        int64_t _match_last_price = 0;  // 撮合 最新价
        ExchangeType _current_exchange = ExchangeType::UNKNOWN; // 市场
        MatchCallback _match_callback = nullptr; // 回调指针
        PriceCage _match_price_cage; // 撮合 价格笼子
        std::unordered_set<PriceLevel*> _match_changed_price_levels; // 撮合期间修改的PriceLevel指针
        std::unordered_set<uint32_t> _match_changed_order_node_slots; // 撮合期间修改的OrderNode的Slot

        TradingPhase _current_phase = TradingPhase::PRE_OPEN;
        std::string _date = "";
        std::string _code = "";    
    };

}

#endif
