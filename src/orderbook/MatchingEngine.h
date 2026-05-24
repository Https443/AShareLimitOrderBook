#ifndef MARKETDATA_ORDERBOOK_MATCHINGENGINGE_H
#define MARKETDATA_ORDERBOOK_MATCHINGENGINGE_H

#include "common/MarketDataStruct.h"
#include "MatchTypes.h"
#include "NodePool.h"
#include "PriceBook.h"
#include "util/Config.h"
#include "util/logger.h"
#include "util/Util.h"
#include <memory>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <cstdint>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <set>
#include <limits>
#include <mutex>
#include <queue>
#include "PriceCage.h"

namespace marketdata
{
    #ifndef REALTRADING
    #define REALTRADING 0
    #endif

    
    /**
     * @brief 模拟撮合引擎
     * @note 功能：
     *       1. 连续竞价撮合：限价单、5种市价单类型
     *       2. 集合竞价撮合：开盘/收盘集合竞价
     *       3. 价格笼子检查：主板/科创板/创业板规则，支持保留模式
     *       4. 撤单处理
     *       5. 上海/深圳交易所订单适配
     *       6. 与LimitOrderBook（成交消除法）比对和修正
     *
     *       数据结构：
     *       - buy_book/sell_book：正常订单簿
     *       - pending_buy_book/pending_sell_book：价格笼子外的临时订单
     *       - 每个价格档位通过 head_slot/tail_slot 串联订单slot，维持时间优先
     *       - OrderPool/PriceLevelPool 共享管理订单节点和价格档位
     *
     *       价格单位：微元（1元=1000000）
     *       时间格式：HHMMSSmmm（如93000000表示9:30:00.000）
     */
    class MatchingEngine
    {
    public:
        using MatchCallback = std::function<void(const MatchRecord&)>;

    public:
        MatchingEngine(const std::string &date, 
                                 const std::string &code, 
                                 std::shared_ptr<OrderPool> poolPtr,
                                 const int64_t pre_close_price,
                                 const int64_t min_price,
                                 const int64_t max_price): 
            _date(date), _code(code), _pool(poolPtr),
            _pre_close_price(pre_close_price),
            _buy_book(min_price, max_price, 10000, code),
            _sell_book(min_price, max_price, 10000, code)
        {
            if (!_pool)
            {
                LOG_ERROR(app_log::logger(), "OrderPool is null, date:{} code:{}", date, code);
                STDTHROW(STD_ERROR_CODE, "OrderPool is null", "OrderPool is null");
            }

            const bool is_cyb = (!code.empty() && code[0] == '3');
            const bool is_kcb = (code.size() >= 2 && code[0] == '6' && code[1] == '8');

            // 2023-04-10 起：主板/创业板/科创板均采用价格笼子+超范围无效处理
            if (date >= "20230410")
            {
                if (is_cyb || is_kcb)
                {
                    LOG_INFO(app_log::logger(),
                             "enable price cage(reject), date:{} code:{} board:CYB/KCB",
                             date,
                             code);
                    _price_cage_mode = PriceCageMode::REJECT;
                    _price_cage_Amain = false;
                }
                else
                {
                    LOG_INFO(app_log::logger(),
                             "enable price cage(reject), date:{} code:{} board:MAIN",
                             date,
                             code);
                    _price_cage_mode = PriceCageMode::REJECT;
                    _price_cage_Amain = true;
                }
            }
            // 科创板自2019-07-22起：价格笼子外直接无效
            else if (is_kcb && date >= "20190722")
            {
                LOG_INFO(app_log::logger(),
                         "enable price cage(reject), date:{} code:{} board:CKB",
                         date,
                         code);
                _price_cage_mode = PriceCageMode::REJECT;
                _price_cage_Amain = false;
            }
            // 创业板2020-06-12至2023-04-09：价格笼子外暂存，入笼后再参与撮合
            else if (is_cyb && date >= "20200612")
            {
                LOG_INFO(app_log::logger(),
                         "enable price cage(pending), date:{} code:{} board:CYB",
                         date,
                         code);
                _price_cage_mode = PriceCageMode::PENDING;
                _price_cage_Amain = false;
            }

            _price_cage.init(pre_close_price, _price_cage_Amain);

            _current_exchange = (!code.empty() && (code[0] == '3' || code[0] == '0')) ? ExchangeType::SZ : (!code.empty() && code[0] == '6') ?ExchangeType::SH : ExchangeType::UNKNOWN;
        }

        ~MatchingEngine()
        {
            releaseBookOrders(_buy_book);
            releaseBookOrders(_sell_book);

            _auction_price = 0;
            _last_price = 0;
            _pre_close_price = 0;
            _price_cage_Amain = false;
            _price_cage_mode = PriceCageMode::DISABLED;

            _match_id_counter = 0;
            _current_phase = TradingPhase::PRE_OPEN;
            _current_exchange = ExchangeType::UNKNOWN;

            _match_callback = nullptr;
            _date = "";
            _code = "";
        }

        // ============ 配置方法 ============

        /**
         * @brief 设置前收盘价
         * @param price 前收盘价（单位：微元）
         * @note 设置后会触发checkAndMovePendingOrders检查pending订单
         */
        inline void setPreClose(int64_t price)
        {
            _pre_close_price = price;
        }

        /**
         * @brief 设置最新成交价
         * @param price 最新成交价（单位：微元）
         * @note 设置后会触发checkAndMovePendingOrders检查pending订单
         */
        inline void setLastPrice(int64_t price)
        {
            _last_price = price;
            checkAndMovePendingOrders();
        }

        /** @brief 获取最新成交价（单位：微元） */
        inline int64_t getLastPrice() const { return _last_price; }

        /** @brief 获取最新模拟撮合成交价（单位：微元） */
        inline int64_t getAuctionPrice() const { return _auction_price; }

        /** @brief 获取前收盘价（单位：微元） */
        inline int64_t getPreClose() const { return _pre_close_price; }

        /**
         * @brief 设置成交回调函数
         * @param cb 回调函数，每笔成交时调用
         */
        inline void setMatchCallback(MatchCallback cb) { _match_callback = std::move(cb); }

        // ============ 获取订单簿 ============

        /** @brief 获取买单簿指针 */
        const auto* getBuyBook() const { return &_buy_book; }

        /** @brief 获取卖单簿指针 */
        const auto* getSellBook() const { return &_sell_book; }

        const int64_t getBuyBestPrice() const { return _buy_book.bestPrice(); }

        const int64_t getSellBestPrice() const { return _sell_book.bestPrice(); }

        // topN
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
            while (slot != PriceLevel::INVALID_SLOT)
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

        inline bool buyIsEmpty()
        {
            return _buy_book.empty();
        };

        inline bool sellIsEmpty()
        {
            return _sell_book.empty();
        };

        // ============ 订单接收 ============

        inline void processOrder(const MDOrder *order)
        {
            if (order == nullptr)
            {
                return;
            }

            if (order->channel_no > 2000)
            {
                // 深交所，模拟撮合新增订单
                processSZOrder(order);
            }
        };

        inline void processTrade(const MDTrade *trade)
        {
            if (trade == nullptr)
            {
                return;
            }
            // std::string code = trade->security_code;
            int64_t time = trade->datetime % 1000000000L;

            if (trade->channel_no > 2000)
            {
                // 深交所，模拟撮合撤单
                processSZCancel(trade);
                if (time >= 150000000L && !_close_auction_statue)
                {
                    finalize();
                    _close_auction_statue = true;
                }
            }
        };

        inline void executeOpenAuction(bool time)
        {
#if REALTRADING
            executeCallAuction(time, true);
#endif
        }

        inline void executeCloseAuction(bool time)
        {
#if REALTRADING
            executeCallAuction(time, false);
#endif
        }

        inline void setOrderBookClose()
        {
            finalize();
        }

        // ============ 状态检测 ============

        /** @brief 获取当前交易阶段 */
        TradingPhase getCurrentPhase() const { return _current_phase; }

        // ============ 调试方法 ============

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
        // ============ 时间检查 ============

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
#if !REALTRADING
            // 最后一个订单是92500000L前收到，下一个订单是93000000L
            if (_current_phase == TradingPhase::OPEN_CALL_AUCTION &&
                new_phase != TradingPhase::OPEN_CALL_AUCTION)
            {
                executeCallAuction(time, true);
            }
            else if (_current_phase == TradingPhase::CLOSE_CALL_AUCTION &&
                     new_phase != TradingPhase::CLOSE_CALL_AUCTION)
            {
                executeCallAuction(time, false);
            }
#endif

            _current_phase = new_phase;
        }

        inline bool isCancelAllowed(int64_t time) const
        {
            // 9:20-9:25、14:57-15:00 不接受撤单
            if ((time >= 92000000L && time < 92500000L) ||
                (time >= 145700000L && time < 150000000L))
            {
                return false;
            }
            return true;
        }

        // ============ 价格笼子检查 ============

        /**
         * @brief 获取买单基准价
         * @return 基准价格（单位：微元，1元=1000000）
         * @note 优先级：ask1 -> bid1 -> lastprice -> preclose
         *       按照 对方 -> 本方 -> 最新价 -> 前一天收盘价顺序取第一个有效值
         */
        inline int64_t getBuyBasePrice() const
        {
            // ask1
            int64_t ask1 = _sell_book.bestPrice();
            if (ask1 > 0) return ask1;
            // bid1
            int64_t bid1 = _buy_book.bestPrice();
            if (bid1 > 0) return bid1;
            // lastprice
            if (_last_price > 0) return _last_price;
            // preclose
            return _pre_close_price;
        }

        /**
         * @brief 获取卖单基准价
         * @return 基准价格（单位：微元，1元=1000000）
         * @note 优先级：bid1 -> ask1 -> lastprice -> preclose
         *       按照 对方 -> 本方 -> 最新价 -> 前一天收盘价顺序取第一个有效值
         */
        inline int64_t getSellBasePrice() const
        {
            // bid1
            int64_t bid1 = _buy_book.bestPrice();
            if (bid1 > 0) return bid1;
            // ask1
            int64_t ask1 = _sell_book.bestPrice();
            if (ask1 > 0) return ask1;
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

            if (_price_cage_Amain)
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

            if (_price_cage_Amain)
            {
                double lower_by_tick = origin_price - 0.01 * 10;
                lower = std::min(lower_by_ratio, lower_by_tick);
            }

            return static_cast<int64_t>(roundTo(lower, 2) * 1000000);
        }

        /**
         * @brief 检查买单价格是否在笼子内
         * @param price 买单价格（单位：微元）
         * @return true表示在笼子内，false表示超出笼子
         * @note 主板：price <= max(基准价*102%, 基准价+10*tick)
         *       科创板/创业板：price <= 基准价*102%
         *       其中102%按最小变动单位四舍五入，若上限与基准价差<1tick则按基准价+1tick，
         *       上限<1tick时按1tick处理。
         */
        inline bool isBuyPriceInCage(int64_t price) const
        {
            if (_price_cage_mode == PriceCageMode::DISABLED) return true;
            int64_t base_price = getBuyBasePrice();
            if (base_price <= 0) return true;
            return price <= getBuyCageUpperPrice(base_price);
        }

        /**
         * @brief 检查卖单价格是否在笼子内
         * @param price 卖单价格（单位：微元）
         * @return true表示在笼子内，false表示超出笼子
         * @note 主板：price >= min(基准价*98%, 基准价-10*tick)
         *       科创板/创业板：price >= 基准价*98%
         *       其中98%按最小变动单位四舍五入，若下限与基准价差<1tick则按基准价-1tick，
         *       下限<1tick时按1tick处理。
         */
        inline bool isSellPriceInCage(int64_t price) const
        {
            if (_price_cage_mode == PriceCageMode::DISABLED) return true;
            int64_t base_price = getSellBasePrice();
            if (base_price <= 0) return true;
            return price >= getSellCageLowerPrice(base_price);
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

        inline bool hasOrders(const PriceLevel* level) const
        {
            return level && level->head_slot != PriceLevel::INVALID_SLOT && level->total_volume > 0;
        }

        // 将已迁出的档位清空为“可释放”状态，随后交给book.erase()归还共享池。
        inline void clearDetachedLevel(PriceLevel* level)
        {
            if (!level)
            {
                return;
            }
            level->head_slot = PriceLevel::INVALID_SLOT;
            level->tail_slot = PriceLevel::INVALID_SLOT;
            level->total_volume = 0;
            level->order_size = 0;
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

        inline bool checkMoveBuyCage(PriceCage &price_cage)
        {
            if (_price_cage_mode == PriceCageMode::DISABLED) return false;
            int64_t base_price = price_cage.getBuyBasePrice();
            if (base_price <= 0)
            {
                return _buy_book.clearCage();
            }

            const int64_t upper_price = price_cage.getBuyCageUpperPrice(base_price);
            return _buy_book.refreshBestByCage([upper_price](int64_t price) 
            {
                return price <= upper_price;
            });
        }

        inline bool checkMoveSellCage(PriceCage &price_cage)
        {
            if (_price_cage_mode == PriceCageMode::DISABLED) return false;
            int64_t base_price = price_cage.getSellBasePrice();
            if (base_price <= 0)
            {
                return _sell_book.clearCage();
            }

            const int64_t lower_price = price_cage.getSellCageLowerPrice(base_price);
            return _sell_book.refreshBestByCage([lower_price](int64_t price) 
            {
                return price >= lower_price;
            });
        }

        /**
         * @brief 检查并重平衡正常book与pending book
         * @note 在每次新增订单、撤单、成交后调用
         */
        inline void checkAndMovePendingOrders()
        {
            if (_price_cage_mode != PriceCageMode::PENDING) return;
            // 价格笼子只在连续竞价阶段生效。集合竞价期间买卖盘允许交叉，
            // 若在撤单后按当前 bid1/ask1 重平衡，会把正常的集合竞价挂单误移出主簿。
            if (_current_phase != TradingPhase::CONTINUOUS_TRADING) return;
            while (true)
            {
                int64_t buy_price = _buy_book.bestPrice();
                int64_t sell_price = _sell_book.bestPrice();
                if (buy_price > 0 && sell_price > 0 && buy_price >= sell_price) break;
                _price_cage.set(buy_price, sell_price, _last_price);
                
                bool changed = false;
                changed |= checkMoveBuyCage(_price_cage);
                changed |= checkMoveSellCage(_price_cage);
                if (!changed) break;
            }
        }

        // ============ 成交生成 ============

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

        /**
         * @brief 从订单队列中删除指定节点并从对象池回收
         * @param node 节点指针
         */
        inline void eraseOrderNode(OrderNode* node)
        {
            if (!node) return;

            if (node->price_level_ptr != nullptr)
            {
                _pool->unlink(node);
            }
            _pool->free(node);
            node = nullptr;
        }

        /**
         * @brief 市价单主动撮合指定档位的订单
         * @param id 主动方订单ID
         * @param datetime 成交时间
         * @param side_type 是否买单
         * @param max_levels 最大撮合档位数量
         * @return 实际消耗的量
         * @note 按时间优先顺序从队列头部开始撮合，完全成交的订单会被释放
         *       用于“五档即成剩撤”、“即成剩撤”、“全部成交或全部撤单”
         */
        inline int64_t matchMarketOrder(
            int64_t id,
            int64_t volume,
            int64_t datetime,
            OrderSideType side_type,
            int max_levels)
        {
            if (side_type == OrderSideType::NONE || volume <= 0)
            {
                return 0;
            }

            if (_pool && _pool->find(id) != nullptr)
            {
                LOG_ERROR(app_log::logger(), "dup order id:{}, ignore new order", id);
                return 0;
            }

            int64_t remaining = volume;
            int levels_consumed = 0;

            while (remaining > 0)
            {
                checkAndMovePendingOrders();

                if (max_levels > 0 && levels_consumed >= max_levels)
                {
                    break;
                }

                int64_t level_price = side_type == OrderSideType::BUY ? _sell_book.bestPrice() : _buy_book.bestPrice();
                if (level_price <= 0)
                {
                    break;
                }

                PriceLevel* level = side_type == OrderSideType::BUY ? _sell_book.find(level_price) : _buy_book.find(level_price);
                if (!hasOrders(level))
                {
                    if (side_type == OrderSideType::BUY)
                    {
                        _sell_book.erase(level_price);
                    }
                    else
                    {
                        _buy_book.erase(level_price);
                    }
                    continue;
                }

                ++levels_consumed;

                OrderNode* node = getHeadNode(level);
                while (node && remaining > 0)
                {
                    OrderNode* next = getNextNode(node);
                    int64_t trade_vol = std::min(node->volume, remaining);
                    char side = side_type == OrderSideType::BUY ? 'B' : 'S';

                    if (side_type == OrderSideType::BUY)
                    {
                        generateMatch(id, node->id, level_price, trade_vol, datetime, side);
                    }
                    else
                    {
                        generateMatch(node->id, id, level_price, trade_vol, datetime, side);
                    }
                    _last_price = level_price;

                    remaining -= trade_vol;
                    node->volume -= trade_vol;
                    level->total_volume -= trade_vol;

                    if (node->volume == 0)
                    {
                        eraseOrderNode(node);
                    }

                    node = next;
                }

                if (!hasOrders(level))
                {
                    if (side_type == OrderSideType::BUY)
                    {
                        _sell_book.erase(level_price);
                    }
                    else
                    {
                        _buy_book.erase(level_price);
                    }
                }
            }

            return volume - remaining;
        }

        /**
         * @brief 处理主订单簿中的价格相交（bid1 >= ask1）
         * @param datetime 成交时间
         * @note 用于订单触发自动撮合，直到最优价不再相交
         */
        inline int64_t matchCrossedMainBook(int64_t datetime)
        {
            int64_t consumed = 0;
            while (true)
            {
                // 撮合前检查是否有pending订单可以移入
                checkAndMovePendingOrders();

                int64_t bid1 = _buy_book.bestPrice();
                int64_t ask1 = _sell_book.bestPrice();
                // 涨跌停则跳过
                if (bid1 <= 0 || ask1 <= 0) break;
                // 撮合完成则跳过
                if (bid1 < ask1) break;

                // 获取买卖档位
                PriceLevel* buy_level = _buy_book.find(bid1);
                PriceLevel* sell_level = _sell_book.find(ask1);
                if (!buy_level || !sell_level) break;

                // 获取订单队列
                OrderNode* buy_node = getHeadNode(buy_level);
                OrderNode* sell_node = getHeadNode(sell_level);
                if (!buy_node || !sell_node) break;

                // 获取这一次的撮合量
                int64_t trade_vol = std::min(buy_node->volume, sell_node->volume);

                // 获取是否主买/主卖
                bool is_buy = buy_node->id > sell_node->id;
                // 如果主买则以对手价成交
                int64_t match_price = is_buy ? ask1 : bid1;
                char side = is_buy ? 'B' : 'S';
                generateMatch(buy_node->id, sell_node->id, match_price, trade_vol, datetime, side);
                // 更新最新价
                _last_price = match_price;

                buy_node->volume -= trade_vol;
                sell_node->volume -= trade_vol;
                buy_level->total_volume -= trade_vol;
                sell_level->total_volume -= trade_vol;
                consumed += trade_vol;

                if (buy_node->volume == 0)
                {
                    eraseOrderNode(buy_node);
                }
                if (sell_node->volume == 0)
                {
                    eraseOrderNode(sell_node);
                }

                if (buy_level->total_volume <= 0)
                {
                    _buy_book.erase(bid1);
                }

                if (sell_level->total_volume <= 0)
                {
                    _sell_book.erase(ask1);
                }
            }
            return consumed;
        }

        /**
         * @brief 获取bid或ask的全部量，用于市价单-全部成交或撤销
         * @param side_type 订单方向
         */
        inline int64_t getContraAvailableVolume(OrderSideType side_type) const
        {
            int64_t available = 0;

            if (side_type == OrderSideType::BUY)
            {
                std::vector<std::pair<int64_t, const marketdata::PriceLevel*>> buf(_sell_book.size());
                const int count = _sell_book.allToBuffer(buf.data());
                for (int i = 0; i < count; ++i)
                {
                    const PriceLevel* level = buf[i].second;
                    if (hasOrders(level))
                    {
                        available += level->total_volume;
                    }
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                std::vector<std::pair<int64_t, const marketdata::PriceLevel*>> buf(_buy_book.size());
                const int count = _buy_book.allToBuffer(buf.data());
                for (int i = 0; i < count; ++i)
                {
                    const PriceLevel* level = buf[i].second;
                    if (hasOrders(level))
                    {
                        available += level->total_volume;
                    }
                }
            }

            return available;
        }

        // ============ 连续竞价核心方法 ============

        /**
         * @brief 处理限价单
         * @param id 订单ID
         * @param price 委托价格
         * @param volume 委托量
         * @param datetime 委托时间
         * @param side_type 是否买单
         * @param order_type 订单类型
         * @note 流程：
         *       1. 检查并移动pending订单
         *       2. 价格笼子检查（超出笼子：保留模式存pending，拒绝模式返回全量）
         *       3. 与买/卖单簿撮合（价格匹配则成交）
         *       4. 剩余量加入买/卖单簿
         *       5. 再次检查pending订单
         */
        inline void processLimitPrice(int64_t id, int64_t price, int64_t volume, int64_t time, OrderSideType side_type, MarketOrderType order_type = MarketOrderType::LIMIT)
        {
            // 防止重复委托导致共享 OrderPool 内的 id 索引冲突，进而破坏订单簿结构
            if (_pool && _pool->find(id) != nullptr)
            {
                LOG_ERROR(app_log::logger(), "dup order id:{}, ignore new order", id);
                return;
            }

            OrderNode* node = _pool->alloc(id);
            if (!node) return;
            node->price = price;
            node->volume = volume;
            node->remaining_match_volume = volume;
            node->time = time;
            node->order_type = order_type;
            node->side_type = side_type;

            // 价格笼子检查
            bool cage_flag = side_type == OrderSideType::BUY ? !isBuyPriceInCage(price) : !isSellPriceInCage(price);
            if (_price_cage_mode != PriceCageMode::DISABLED && cage_flag)
            {
                // 当前规则下超出价格笼子
                // 直接废单
                if (_price_cage_mode == PriceCageMode::REJECT)
                {
                    _pool->free(node);
                    return;
                }

                // 暂存等待入笼
                if (_price_cage_mode == PriceCageMode::PENDING)
                {
                    if (side_type == OrderSideType::BUY)
                    {
                        if (!linkNodeToBook(_buy_book, price, node))
                        {
                            _pool->free(node);
                            return;
                        }
                    }
                    else if (side_type == OrderSideType::SELL)
                    {
                        if (!linkNodeToBook(_sell_book, price, node))
                        {
                            _pool->free(node);
                            return;
                        }
                    }
                    
                    return; // 订单已保存
                }
                return;
            }
            else
            {
                if (side_type == OrderSideType::BUY)
                {
                    if (!linkNodeToBook(_buy_book, price, node))
                    {
                        _pool->free(node);
                        return;
                    }
                }
                else if (side_type == OrderSideType::SELL)
                {
                    if (!linkNodeToBook(_sell_book, price, node))
                    {
                        _pool->free(node);
                        return;
                    }
                }
                
            }
            // match
            matchCrossedMainBook(time);
        }

        // ============ 连续竞价市价单核心方法 ============

        /**
         * @brief 处理市价单-对手方最优
         * @param id 订单ID
         * @param volume 委托量
         * @param datetime 委托时间
         * @param side_type 买单/卖单
         * @return 未成交量
         * @note 以对手方最优价格作为限价，买单取offer1价格，卖单取bid1价格
         *       若对手方无挂单则订单不成交
         *       深交所: 对手方最优价格申报，以申报进入交易主机时"集中申报簿中对手方队列的最优价格"为其申报价格，集中申报簿中对手方无申报的，申报自动撤销。(https://docs.static.szse.cn/www/lawrules/publicadvice/W020230201595730281095.pdf)
         *       上交所: 对手方最优价格申报，即该申报以其进入交易主机时，"集中申报簿中对手方最优报价"为其申报价格。对手方最优价格申报进入交易主机时，集中申报簿中对手方无申报的，申报自动撤销。(https://www.sse.com.cn/lawandrules/sselawsrules2025/stocks/exchange/c/10779396/files/2895ff8836144daab8f4200b67dac671.docx)
         */
        inline void processMarketBestOpponent(int64_t id, int64_t volume, int64_t datetime, OrderSideType side_type)
        {
            if (side_type == OrderSideType::BUY)
            {
                int64_t limit_price = _sell_book.bestPrice();
                if (limit_price <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }

                processLimitPrice(id, limit_price, volume, datetime, OrderSideType::BUY, MarketOrderType::MARKET_BEST_OPPONENT);
            }
            else if (side_type == OrderSideType::SELL)
            {
                int64_t limit_price = _buy_book.bestPrice();
                if (limit_price <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }

                processLimitPrice(id, limit_price, volume, datetime, OrderSideType::SELL, MarketOrderType::MARKET_BEST_OPPONENT);
            }
            return;
        }

        /**
         * @brief 处理市价单-本方最优
         * @param id 订单ID
         * @param volume 委托量
         * @param datetime 委托时间
         * @param side_type 买单/卖单
         * @return 未成交量
         * @note 以本方最优价格作为限价
         *       深交所: 本方最优价格申报，以申报进入交易主机时"集中申报簿中本方队列的最优价格"为其申报价格，集中申报簿中本方无申报的，申报自动撤销。(https://docs.static.szse.cn/www/lawrules/publicadvice/W020230201595730281095.pdf)
         *       上交所: 本方最优价格申报，即该申报以其进入交易主机时，"集中申报簿中本方最优报价"为其申报价格。本方最优价格申报进入交易主机时，集中申报簿中本方无申报的，申报自动撤销；(https://www.sse.com.cn/lawandrules/sselawsrules2025/stocks/exchange/c/10779396/files/2895ff8836144daab8f4200b67dac671.docx)
         */
        inline void processMarketBestSelf(int64_t id, int64_t volume, int64_t datetime, OrderSideType side_type)
        {
            if (side_type == OrderSideType::BUY)
            {
                int64_t limit_price = _buy_book.bestPrice();
                if (limit_price <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }

                processLimitPrice(id, limit_price, volume, datetime, OrderSideType::BUY, MarketOrderType::MARKET_BEST_SELF);
            }
            else if (side_type == OrderSideType::SELL)
            {
                int64_t limit_price = _sell_book.bestPrice();
                if (limit_price <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }

                processLimitPrice(id, limit_price, volume, datetime, OrderSideType::SELL, MarketOrderType::MARKET_BEST_SELF);
            }
            return;
        }

        /**
         * @brief 处理市价单-最优五档即时成交剩余撤销
         * @param id 订单ID
         * @param volume 委托量
         * @param datetime 委托时间
         * @param side_type 买单/卖单
         * @return 未成交量（转限价成功通常返回0；若无本方报价则返回剩余量表示撤销）
         * @note 与对手方最优5个价格档位撮合，超过5档的剩余量自动撤销
         *       深交所: 最优五档即时成交剩余撤销申报，以"对手方价格为成交价"，与申报进入交易主机时"集中申报簿中对手方最优五个价位的申报队列依次成交"，未成交部分自动撤销。(https://docs.static.szse.cn/www/lawrules/publicadvice/W020230201595730281095.pdf)
         *       上交所: 最优5档即时成交剩余撤销申报，即该申报在"对手方'实时最优5个价位'内以对手方价格为成交价逐次成交"，剩余未成交部分自动撤销；(https://www.sse.com.cn/lawandrules/sselawsrules2025/stocks/exchange/c/10779396/files/2895ff8836144daab8f4200b67dac671.docx)
         */
        inline void processMarketFiveLevel(int64_t id, int64_t volume, int64_t datetime, OrderSideType side_type)
        {
            if (side_type == OrderSideType::BUY)
            {
                if (_sell_book.bestPrice() <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }
                if (matchMarketOrder(id, volume, datetime, OrderSideType::BUY, 5) > 0)
                {
                    matchCrossedMainBook(datetime);
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                if (_buy_book.bestPrice() <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }
                if (matchMarketOrder(id, volume, datetime, OrderSideType::SELL, 5) > 0)
                {
                    matchCrossedMainBook(datetime);
                }
            }
            return;
        }

        /**
         * @brief 处理市价单-即时成交剩余撤销
         * @param id 订单ID
         * @param volume 委托量
         * @param datetime 委托时间
         * @param side_type 买单/卖单
         * @return 未成交量（剩余部分被撤销）
         * @note 与对手方所有可成交订单撮合，剩余量自动撤销
         *       深交所: 即时成交并撤销申报，以"对手方价格为成交价"，与申报进入交易主机时"集中申报簿中对手方所有申报队列依次成交"，未成交部分自动撤销。(https://docs.static.szse.cn/www/lawrules/publicadvice/W020230201595730281095.pdf)
         */
        inline void processMarketImmediate(int64_t id, int64_t volume, int64_t datetime, OrderSideType side_type)
        {
            if (side_type == OrderSideType::BUY)
            {
                if (_sell_book.bestPrice() <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }
                if (matchMarketOrder(id, volume, datetime, OrderSideType::BUY, -1) > 0)
                {
                    matchCrossedMainBook(datetime);
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                if (_buy_book.bestPrice() <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }
                if (matchMarketOrder(id, volume, datetime, OrderSideType::SELL, -1) > 0)
                {
                    matchCrossedMainBook(datetime);
                }
            }
            return;
        }

        /**
         * @brief 处理市价单-全额成交或撤销
         * @param id 订单ID
         * @param volume 委托量
         * @param datetime 委托时间
         * @param side_type true为买单，false为卖单
         * @return 未成交量（要么全部成交返回0，要么全部撤销返回原量）
         * @note 先检查对手方总量是否足够，不足则全部撤销足够则执行即时成交撮合
         *       深交所: 全额成交或撤销申报，以对手方价格为成交价，如与申报进入交易主机时"集中申报簿中对手方所有申报队列依次成交能够使其完全成交的，则依次成交，否则申报全部自动撤销"。(https://docs.static.szse.cn/www/lawrules/publicadvice/W020230201595730281095.pdf)
         */
        inline void processMarketAllOrCancel(int64_t id, int64_t volume, int64_t datetime, OrderSideType side_type)
        {
            if (side_type == OrderSideType::NONE || volume <= 0)
            {
                return;
            }

            if (_pool && _pool->find(id) != nullptr)
            {
                LOG_ERROR(app_log::logger(), "dup order id:{}, ignore new order", id);
                return;
            }

            checkAndMovePendingOrders();

            int64_t available = getContraAvailableVolume(side_type);

            // 如果不能全量成交则返回
            if (available < volume)
            {
                _skip_ids.insert(id);
                return;
            }

            if (side_type == OrderSideType::BUY)
            {
                if (_sell_book.bestPrice() <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }
                int64_t consumed = matchMarketOrder(id, volume, datetime, OrderSideType::BUY, -1);
                if (consumed > 0)
                {
                    matchCrossedMainBook(datetime);
                }
                if (consumed != volume)
                {
                    LOG_ERROR(app_log::logger(), "market all or cancel partial fill, id:{}, volume:{}, consumed:{}", id, volume, consumed);
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                if (_buy_book.bestPrice() <= 0)
                {
                    _skip_ids.insert(id);
                    return;
                }
                int64_t consumed = matchMarketOrder(id, volume, datetime, OrderSideType::SELL, -1);
                if (consumed > 0)
                {
                    matchCrossedMainBook(datetime);
                }
                if (consumed != volume)
                {
                    LOG_ERROR(app_log::logger(), "market all or cancel partial fill, id:{}, volume:{}, consumed:{}", id, volume, consumed);
                }
            }
        }

        // ============ 集合竞价撮合 ============

        /**
         * @brief 添加订单到集合竞价簿
         * @param id 订单ID
         * @param price 委托价格（单位：微元）
         * @param volume 委托量
         * @param datetime 委托时间
         * @param side_type true为买单，false为卖单
         * @note 集合竞价期间订单不立即撮合，存入buy_book/sell_book
         *       在集合竞价撮合时点统一撮合
         */
        inline void addToAuctionBook(int64_t id, int64_t price, int64_t volume, int64_t time, OrderSideType side_type)
        {
            OrderNode* node = _pool->alloc(id);
            if (!node) return;

            node->price = price;
            node->volume = volume;
            node->remaining_match_volume = volume;
            node->time = time;
            node->order_type = MarketOrderType::LIMIT;
            node->side_type = side_type;

            if (side_type == OrderSideType::BUY)
            {
                if (!linkNodeToBook(_buy_book, price, node))
                {
                    _pool->free(node);
                    return;
                }
            }
            else if (side_type == OrderSideType::SELL)
            {
                if (!linkNodeToBook(_sell_book, price, node))
                {
                    _pool->free(node);
                    return;
                }
            }

        }

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

        /**
         * @brief 将订单簿量为0的节点清除
         */
        inline void mergeAuctionToMain()
        {
            // 买单
            std::vector<std::pair<int64_t, const PriceLevel*>> buy_book_buf(_buy_book.size());
            _buy_book.allToBuffer(buy_book_buf.data());
            for (const auto& [price, level] : buy_book_buf)
            {
                OrderNode* node = getHeadNode(level);
                while (node)
                {
                    OrderNode* next = getNextNode(node);
                    if (node->volume <= 0)
                    {
                        _pool->unlink(node);
                        _pool->free(node);
                    }
                    node = next;
                }
                if (level->total_volume <= 0)
                {
                    _buy_book.erase(price);
                    continue;
                }
            }

            // 卖单
            std::vector<std::pair<int64_t, const PriceLevel*>> sell_book_buf(_sell_book.size());
            _sell_book.allToBuffer(sell_book_buf.data());
            for (const auto& [price, level] : sell_book_buf)
            {
                OrderNode* node = getHeadNode(level);
                while (node)
                {
                    OrderNode* next = getNextNode(node);
                    if (node->volume <= 0)
                    {
                        _pool->unlink(node);
                        _pool->free(node);
                    }
                    node = next;
                }
                if (level->total_volume <= 0)
                {
                    _sell_book.erase(price);
                    continue;
                }
            }
        }

        /**
         * @brief 执行集合竞价撮合
         * @param datetime 撮合时间
         * @param is_open_call true=开盘集合竞价，false=收盘集合竞价
         * @note 流程：
         *       1. 计算集合竞价成交价
         *       2. 按价格优先、时间优先收集参与成交订单
         *       3. 逐笔撮合
         *       4. 未成交订单转入连续竞价订单簿
         */
        inline void executeCallAuction(int64_t datetime, bool is_open_call)
        {
            int64_t _auction_price = calculateAuctionPrice(is_open_call);

            if (_auction_price == 0)
            {
                // 无法形成成交价，订单全部转入连续竞价
                mergeAuctionToMain();
                return;
            }

            std::vector<OrderNode*> buy_orders;
            std::vector<OrderNode*> sell_orders;

            // 买单簿是降序map：价格优先；同价链表顺序：时间优先
            std::vector<std::pair<int64_t, const PriceLevel*>> buy_book_buf(_buy_book.size());
            _buy_book.allToBuffer(buy_book_buf.data());
            for (auto& _pair : buy_book_buf)
            {
                if (_pair.first < _auction_price)
                    break;
                OrderNode* node = getHeadNode(_pair.second);
                while (node)
                {
                    buy_orders.push_back(node);
                    node = getNextNode(node);
                }
            }

            // 卖单簿是升序map：价格优先；同价链表顺序：时间优先
            std::vector<std::pair<int64_t, const PriceLevel*>> sell_book_buf(_sell_book.size());
            _sell_book.allToBuffer(sell_book_buf.data());
            for (auto& _pair : sell_book_buf)
            {
                if (_pair.first > _auction_price)
                    break;
                OrderNode* node = getHeadNode(_pair.second);
                while (node)
                {
                    sell_orders.push_back(node);
                    node = getNextNode(node);
                }
            }

            // 开始集合竞价撮合
            size_t buy_idx = 0, sell_idx = 0;
            while (buy_idx < buy_orders.size() && sell_idx < sell_orders.size())
            {
                OrderNode* buy_node = buy_orders[buy_idx];
                OrderNode* sell_node = sell_orders[sell_idx];

                if (buy_node->volume == 0) 
                { 
                    buy_idx++;
                    continue;
                }
                if (sell_node->volume == 0) 
                { 
                    sell_idx++;
                    continue;
                }

                int64_t trade_vol = std::min(buy_node->volume, sell_node->volume);

                // 集合竞价无严格主动方概念，这里沿用先到先撮合标识
                char aggressor = (buy_node->id <= sell_node->id) ? 'B' : 'S';
                generateMatch(buy_node->id, sell_node->id, _auction_price, trade_vol, datetime, aggressor);
                // 更新最新价
                _last_price = _auction_price;

                buy_node->volume -= trade_vol;
                sell_node->volume -= trade_vol;
                if (buy_node->volume == 0)
                {
                    buy_idx++;
                }
                if (sell_node->volume == 0)
                {
                    sell_idx++;
                }

                PriceLevel* buy_level = getPriceLevel(buy_node);
                PriceLevel* sell_level = getPriceLevel(sell_node);
                if (buy_level)
                {
                    buy_level->total_volume -= trade_vol;
                }
                if (sell_level)
                {
                    sell_level->total_volume -= trade_vol;
                }

            }

            // 清理已成交订单，未成交订单转入连续竞价
            mergeAuctionToMain();
            _last_price = _auction_price;
        }

        // ============ 交易所适配 ============

        /**
         * @brief 撤销订单
         * @param id 要撤销的订单ID
         * @param time 
         * @return 被撤销的量，订单不存在返回0
         * @note 从订单簿（正常book、pending book）中移除订单
         *       空的价格档位会被自动清理
         *       撤单后会检查pending订单是否可以移入正常book
         */
        inline int64_t cancelOrder(int64_t id, int64_t time)
        {
            OrderNode* node = _pool ? _pool->find(id) : nullptr;
            if (node == nullptr)
                return 0;

            int64_t cancelled_volume = node->volume;
            int64_t price = node->price;
            OrderSideType side_type = node->side_type;
            PriceLevel* _price_level = getPriceLevel(node);

            _pool->unlink(node);

            // 清理空的价格档位（可能在正常book、pending book）
            eraseEmptyLevel(side_type, price, _price_level);

            _pool->free(node);

            // 撤单后检查是否有pending订单可以移入
            checkAndMovePendingOrders();

            return cancelled_volume;
        }

        /**
         * @brief 处理深圳交易所订单
         * @param order 订单数据指针（MDOrder结构）
         * @note 根据时间自动判断交易阶段：
         *       - 集合竞价阶段：仅接受限价单（order_type='2'）
         *       - 连续竞价阶段：限价单'2'、市价单'1'、本方最优'U'
         *       - 阶段切换时自动触发集合竞价撮合
         *       深圳买卖方向：'1'/'G'=买，'2'/'F'=卖
         */
        inline void processSZOrder(const MDOrder* order)
        {
            if (!order) return;

            int64_t time = order->datetime % 1000000000L;
            TradingPhase phase = determinePhase(time);
            handlePhaseTransition(phase, time);

            OrderSideType side_type = (order->side == '1' || order->side == 'G') ? OrderSideType::BUY : OrderSideType::SELL;

            // 连续竞价
            if (phase == TradingPhase::CONTINUOUS_TRADING)
            {
                // 限价单
                if (order->order_type == '2')
                {
                    processLimitPrice(order->appl_seq_num, order->price, order->volume, time, side_type, MarketOrderType::LIMIT);
                }
                // 市价单
                else if (order->order_type == '1')
                {
                    // 这里的市价单很难确定是 “对手方最优”还是“最优五档即时成交剩余撤销”还是“即时成交剩余撤销”还是“全额成交或撤销”
                    // 无论哪一种都可能引入数据错误
                    // processMarketBestOpponent(order->appl_seq_num, order->volume, time, side_type);
                    processMarketImmediate(order->appl_seq_num, order->volume, time, side_type);
                    // processMarketFiveLevel(order->appl_seq_num, order->volume, time, side_type);
                    // processMarketAllOrCancel(order->appl_seq_num, order->volume, time, side_type);
                }
                // 本方最优
                else if (order->order_type == 'U')
                {
                    processMarketBestSelf(order->appl_seq_num, order->volume, time, side_type);
                }
            }
            // 集合竞价
            else if (phase == TradingPhase::OPEN_CALL_AUCTION ||
                     phase == TradingPhase::CLOSE_CALL_AUCTION)
            {
                // 集合竞价只接受限价单
                // 上交所：市价申报只适用于连续竞价（规则 3.3.6）。
                // 深交所：市价申报只适用于有涨跌幅限制证券的连续竞价，其他时段不接受市价（规则 3.3.5）
                if (order->order_type == '2')
                {
                    addToAuctionBook(order->appl_seq_num, order->price, order->volume, time, side_type);
                    if (phase == TradingPhase::OPEN_CALL_AUCTION)
                        _auction_price = calculateAuctionPrice(true);
                    else if (phase == TradingPhase::CLOSE_CALL_AUCTION)
                        _auction_price = calculateAuctionPrice(false);
                }
            }
        };

        /**
         * @brief 处理深圳交易所撤单（通过成交记录）
         * @param trade 成交记录指针（MDTrade结构）
         * @note 深圳撤单通过exec_type='4'的成交记录传递
         *       根据bid_appl_seq_num或offer_appl_seq_num确定要撤销的订单
         */
        inline void processSZCancel(const MDTrade* trade)
        {
            if (!trade) return;
            int64_t bid_id = trade->bid_appl_seq_num;
            int64_t offer_id = trade->offer_appl_seq_num;
            int64_t time = trade->datetime % 1000000000L;
            TradingPhase phase = determinePhase(time);
            handlePhaseTransition(phase, time);

            if ((phase != TradingPhase::OPEN_CALL_AUCTION &&
                 phase != TradingPhase::CONTINUOUS_TRADING &&
                 phase != TradingPhase::CLOSE_CALL_AUCTION) ||
                !isCancelAllowed(time))
                return;

            if (trade->exec_type == '4')
            {
                if (bid_id != 0)
                {
                    if (_skip_ids.find(bid_id) != _skip_ids.end()) return;
                    cancelOrder(bid_id, time);
                }
                else if (offer_id != 0)
                {
                    if (_skip_ids.find(offer_id) != _skip_ids.end()) return;
                    cancelOrder(offer_id, time);
                }
            }
        };

        /**
         * @brief 数据流结束时的收尾处理
         * @param time 收尾时间（默认15:00:00.000）
         * @note 用于触发停留在集合竞价阶段时未被动触发的撮合
         */
        inline void finalize(int64_t time = 150000000L)
        {
            if (time < 150000000L)
            {
                time = 150000000L;
            }
            handlePhaseTransition(TradingPhase::CLOSED, time);
        };

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
        // 对象池
        std::shared_ptr<OrderPool> _pool;
        // 跳过的订单id，主要存在于市价单，当对手方无对手价/本方无最优价，则发单立即撤单，视为废单，不进入订单簿
        std::unordered_set<int64_t> _skip_ids;

        // 买单簿（价格降序）
        PriceLevelBookGreat _buy_book;
        // 卖单簿（价格升序）
        PriceLevelBookLess _sell_book;

        // 集合竞价模拟最新价格
        int64_t _auction_price = 0;

        // 价格笼子相关
        int64_t _last_price = 0; // 最新价
        int64_t _pre_close_price = 0;  //昨收价
        PriceCageMode _price_cage_mode = PriceCageMode::DISABLED;
        bool _price_cage_Amain = false;  // 是否主板
        PriceCage _price_cage;

        int64_t _match_id_counter = 0;
        TradingPhase _current_phase = TradingPhase::PRE_OPEN;
        ExchangeType _current_exchange = ExchangeType::UNKNOWN;

        MatchCallback _match_callback = nullptr;
        bool _close_auction_statue = false;  // 是否已集合竞价
        std::string _date = "";
        std::string _code = "";


    };

} // namespace marketdata

#endif // MARKETDATA_ORDERBOOK_MATCHENGINE_H
