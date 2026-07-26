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
namespace orderbook
{

    struct MatchingStartTick
    {
        int32_t buyTick;
        int32_t sellTick;
    };

    class LimitOrderBook
    {
    public:
        using MatchCallback = std::function<void(const MatchRecord&)>;

    public:
        explicit LimitOrderBook(const std::string &date,
                                const std::string &code,
                                const uint8_t exchange,
                                std::shared_ptr<OrderPool> poolPtr,
                                const int64_t preClosePrice,
                                const int64_t minPrice,
                                const int64_t maxPrice):
            m_date(date), m_code(code), m_exchange(exchange), m_pool(poolPtr),
            m_preClosePrice(preClosePrice),
            m_buyBook(minPrice, maxPrice, 10000, code),
            m_sellBook(minPrice, maxPrice, 10000, code),
            m_limitupPrice(maxPrice), m_limitdownPrice(minPrice),
            m_matchTick{-1, -1}
        {
            if (!m_pool)
            {
                LOG_ERROR(app_log::logger(), "OrderPool is null, date:{} code:{}", date, code);
                STDTHROW(STD_ERROR_CODE, "OrderPool is null", "OrderPool is null");
            }

            m_pendingTrade.reserve(200);
            m_matchChangedOrderNodeSlots.reserve(200);
            m_matchChangedPriceLevels.reserve(10);

            // 价格笼子（试点阶段）：
            // 科创板从2019年7月22日起即运用价格笼子 + 废单处理，即价格笼子以外直接废单；
            // 创业板在试点注册制推广后从2020年6月12日开始，采用了价格笼子 + 订单暂存 → 等待条件满足再入撮合 的机制。
            // 参考材料：https://www.szse.cn/disclosure/notice/general/t20200612_578381.html

            // 2023年4月10日
            // 全面注册制+主板价格笼子
            // 主板、科创板、创业板均改外超过价格笼子即废单处理 （佐证材料：“当委托进入交易系统时，如果其价格超过有效价格范围或价格限制，该委托将被视为无效。”——引自证监会）

            if (((date >= "20200612" && !code.empty() && code[0] == '3' && exchange == 0)) ||
                (code.size() >= 2 && code[0] == '6' && code[1] == '8' && exchange == 1))
            {
                LOG_INFO(app_log::logger(), "enable price cage, date:{} code:{}", date, code);
                m_priceCageEnabled = true;
                m_priceCageAmain = false;
            }
            else if (date >= "20230410")
            {
                LOG_INFO(app_log::logger(), "enable all price cage, date:{} code:{}", date, code);
                m_priceCageEnabled = true;
                m_priceCageAmain = true;
            }

            if (exchange == 0)
            {
                m_currentExchange = ExchangeType::SZ;
            }
            else if (exchange == 1)
            {
                m_currentExchange = ExchangeType::SH;
            }

            // 价格笼子初始化
            m_priceCage.init(m_preClosePrice, m_priceCageAmain);
            m_matchPriceCage.init(m_preClosePrice, m_priceCageAmain);
        }

        ~LimitOrderBook()
        {
            m_pendingTrade.clear();
            m_skipIds.clear();
            m_matchChangedOrderNodeSlots.clear();
            m_matchChangedPriceLevels.clear();

            releaseBookOrders(m_buyBook);
            releaseBookOrders(m_sellBook);

            m_currentPhase = TradingPhase::PRE_OPEN;
        }

        inline bool isPriceCageEnabled() const { return m_priceCageEnabled; }

        inline int64_t getPreClose() const { return m_preClosePrice; }

        inline int64_t getLastPrice() const { return m_lastPrice; }

        inline int64_t getCurrentId() const { return m_currentId; }

        inline int64_t getLimitupPrice() const { return m_limitupPrice; }

        inline int64_t getLimitdownPrice() const { return m_limitdownPrice; }

        inline int64_t getCurrentTime() const { return m_currentTime; }

        inline int64_t getVolumes() const { return m_volumes; }

        inline int64_t getTurnover() const { return m_turnover; }

        inline int64_t getMatchLastPrice() const { return m_matchLastPrice; }

        inline int64_t getMatchVolumes() const { return m_matchVolumes; }

        inline int64_t getMatchTurnover() const { return m_matchTurnover; }

        const int64_t getBuyBestPrice() const { return m_buyBook.bestPrice(); }

        const int64_t getSellBestPrice() const { return m_sellBook.bestPrice(); }

        inline bool buyIsEmpty() { return m_buyBook.empty(); };

        inline bool sellIsEmpty() { return m_sellBook.empty(); };

        inline const PriceLevelBookGreat* getBuyBook() const { return &m_buyBook; }

        inline const PriceLevelBookLess* getSellBook() const { return &m_sellBook; }

        inline const MatchingStartTick* getMatchStartTick() const { return &m_matchTick; }

        inline void processOrder(const Order *order)
        {
            if (order == nullptr)
            {
                return;
            }
            int64_t time = order->time;
            TradingPhase phase = determinePhase(time);
            handlePhaseTransition(phase, time);
            m_currentId = order->applSeqNum;

            if (order->channelNo < 10)
            {
                // 上交所输出的order是未成交的order委托，已成交的的不显示委托，只在trade中一带而过
                if (order->orderType == 'A')
                {
                    int64_t buyBestPrice = m_buyBook.bestPrice();
                    int64_t sellBestPrice = m_sellBook.bestPrice();
                    m_priceCage.set(buyBestPrice, sellBestPrice, m_lastPrice);

                    if (order->side == 'B')
                    {
                        addOrder(order->applSeqNum, order->price, order->volume, time, OrderSideType::BUY, MarketOrderType::NONE);
                    }
                    else if (order->side == 'S')
                    {
                        addOrder(order->applSeqNum, order->price, order->volume, time, OrderSideType::SELL, MarketOrderType::NONE);
                    }
                    checkAndMovePendingOrders();
                }
                else if (order->orderType == 'D')
                {
                    if (order->side == 'B')
                    {
                        dropOrder(order->applSeqNum, order->volume, time, OrderSideType::BUY);
                    }
                    else if (order->side == 'S')
                    {
                        dropOrder(order->applSeqNum, order->volume, time, OrderSideType::SELL);
                    }

                    int64_t buyBestPrice = m_buyBook.bestPrice();
                    int64_t sellBestPrice = m_sellBook.bestPrice();
                    m_priceCage.set(buyBestPrice, sellBestPrice, m_lastPrice);
                    checkAndMovePendingOrders();
                }

                // 集合竞价期间模拟撮合最新价
                if (phase == TradingPhase::OPEN_CALL_AUCTION)
                {
                    m_lastPrice = calculateAuctionPrice(true);
                }
                else if (phase == TradingPhase::CLOSE_CALL_AUCTION)
                {
                    m_lastPrice = calculateAuctionPrice(false);
                }
            }
            else if (order->channelNo > 2000)
            {
                int64_t buyBestPrice = m_buyBook.bestPrice();
                int64_t sellBestPrice = m_sellBook.bestPrice();
                m_priceCage.set(buyBestPrice, sellBestPrice, m_lastPrice);
                // 买 / 借入
                if (order->side == '1' || order->side == 'G')
                {
                    // 限价单
                    if (order->orderType == '2')
                    {
                        addOrder(order->applSeqNum, order->price, order->volume, time, OrderSideType::BUY, MarketOrderType::LIMIT);
                        // 集合竞价期间模拟撮合最新价
                        if (phase == TradingPhase::OPEN_CALL_AUCTION)
                        {
                            m_lastPrice = calculateAuctionPrice(true);
                        }
                        else if (phase == TradingPhase::CLOSE_CALL_AUCTION)
                        {
                            m_lastPrice = calculateAuctionPrice(false);
                        }
                    }
                    // 市价单情况特殊处理[除本方最优，其他四个市价单均以对手价成交]
                    else if (order->orderType == '1')
                    {
                        auto sell1Price = m_sellBook.bestPrice();
                        if (sell1Price <= 0)
                        {
                            m_skipIds.insert(order->applSeqNum);
                            return;
                        }
                        addOrder(order->applSeqNum, sell1Price, order->volume, time, OrderSideType::BUY, MarketOrderType::MARKET);
                    }
                    // 本方最优价格申报，以申报进入交易主机时"集中申报簿中本方队列的最优价格"为其申报价格，集中申报簿中本方无申报的，申报自动撤销。
                    else if (order->orderType == 'U')
                    {
                        auto buy1Price = m_buyBook.bestPrice();
                        if (buy1Price <= 0)
                        {
                            m_skipIds.insert(order->applSeqNum);
                            return;
                        }
                        addOrder(order->applSeqNum, buy1Price, order->volume, time, OrderSideType::BUY, MarketOrderType::MARKET_BEST_SELF);
                    }
                }
                // 卖 / 出借
                else if (order->side == '2' || order->side == 'F')
                {
                    if (order->orderType == '2')
                    {
                        addOrder(order->applSeqNum, order->price, order->volume, time, OrderSideType::SELL, MarketOrderType::LIMIT);
                        // 集合竞价期间模拟撮合最新价
                        if (phase == TradingPhase::OPEN_CALL_AUCTION)
                        {
                            m_lastPrice = calculateAuctionPrice(true);
                            m_matchLastPrice = m_lastPrice;
                        }
                        else if (phase == TradingPhase::CLOSE_CALL_AUCTION)
                        {
                            m_lastPrice = calculateAuctionPrice(false);
                            m_matchLastPrice = m_lastPrice;
                        }
                    }
                    else if (order->orderType == '1')
                    {
                        auto buy1Price = m_buyBook.bestPrice();
                        if (buy1Price <= 0)
                        {
                            m_skipIds.insert(order->applSeqNum);
                            return;
                        }
                        addOrder(order->applSeqNum, buy1Price, order->volume, time, OrderSideType::SELL, MarketOrderType::MARKET);
                    }
                    else if (order->orderType == 'U')
                    {
                        auto sell1Price = m_sellBook.bestPrice();
                        if (sell1Price <= 0)
                        {
                            m_skipIds.insert(order->applSeqNum);
                            return;
                        }
                        addOrder(order->applSeqNum, sell1Price, order->volume, time, OrderSideType::SELL, MarketOrderType::MARKET_BEST_SELF);
                    }
                }
                else
                {
                    LOG_ERROR(app_log::logger(), "not support order side:{}, order type:{}, order id:{}", order->side, order->orderType, order->applSeqNum);
                }
                // 价格笼子边界可能变化，刷新笼内 best。
                checkAndMovePendingOrders();

                // 模拟撮合
                tryMatchCrossedMainBook();
            }
        };

        inline void processTrade(const Trade *trade)
        {
            if (trade == nullptr)
            {
                return;
            }
            int64_t time = trade->time;
            TradingPhase phase = determinePhase(time);
            handlePhaseTransition(phase, time);
            m_currentId = trade->applSeqNum;

            if (trade->channelNo < 10)
            {
                // 主买 bid_appl_seq_num > offer_appl_seq_num or side=B 只存在卖单
                // 主卖 bid_appl_seq_num < offer_appl_seq_num or side=S 只存在买单

                // 连续竞价阶段
                if (time >= 93000000000000L && time < 145700000000000L)
                {
                    // 更新最新成交价
                    m_lastPrice = trade->price;

                    // 上交所成交考虑方向，如果order和trade方向相同则跳过，不同则删除

                    if (trade->bidApplSeqNum > trade->offerApplSeqNum || trade->side == 'B')
                    {
                        dropOrder(trade->offerApplSeqNum, trade->volume, time, OrderSideType::SELL);
                    }
                    else if (trade->bidApplSeqNum < trade->offerApplSeqNum || trade->side == 'S')
                    {
                        dropOrder(trade->bidApplSeqNum, trade->volume, time, OrderSideType::BUY);
                    }
                    // N仅发生在集合竞价阶段
                    else if (trade->side == 'N')
                    {
                        LOG_WARNING(app_log::logger(), "code:{} biz_index:{} bid id:{} offer id:{} side is 'N' unknow", trade->securityCode, trade->bizIndex, trade->bidApplSeqNum, trade->offerApplSeqNum);
                        dropOrder(trade->bidApplSeqNum, trade->volume, time, OrderSideType::BUY, false, true);
                        dropOrder(trade->offerApplSeqNum, trade->volume, time, OrderSideType::SELL, false, true);
                    }
                    else
                    {
                        LOG_WARNING(app_log::logger(), "code:{} biz_index:{} bid id:{} offer id:{} side is unknow", trade->securityCode, trade->bizIndex, trade->bidApplSeqNum, trade->offerApplSeqNum);
                        dropOrder(trade->bidApplSeqNum, trade->volume, time, OrderSideType::BUY, false, true);
                        dropOrder(trade->offerApplSeqNum, trade->volume, time, OrderSideType::SELL, false, true);
                    }
                }
                // 开盘集合竞价阶段
                else if (time < 93000000000000L)
                {
                    dropOrder(trade->bidApplSeqNum, trade->volume, time, OrderSideType::BUY);
                    dropOrder(trade->offerApplSeqNum, trade->volume, time, OrderSideType::SELL);
                }
                // 收盘集合竞价阶段
                else if (time >= 145700000000000L)
                {
                    dropOrder(trade->bidApplSeqNum, trade->volume, time, OrderSideType::BUY, false, true);
                    dropOrder(trade->offerApplSeqNum, trade->volume, time, OrderSideType::SELL, false, true);
                }
                m_volumes += trade->volume;
                m_turnover += trade->volume * trade->price;

                if (!hasCrossedMainBook())
                {
                    int64_t buyBestPrice = m_buyBook.bestPrice();
                    int64_t sellBestPrice = m_sellBook.bestPrice();
                    m_priceCage.set(buyBestPrice, sellBestPrice, m_lastPrice);
                    // 价格笼子边界可能变化，刷新笼内 best。
                    checkAndMovePendingOrders();
                }
            }
            else if (trade->channelNo > 2000)
            {
                int64_t buyBestPrice = m_buyBook.bestPrice();
                int64_t sellBestPrice = m_sellBook.bestPrice();
                m_priceCage.set(buyBestPrice, sellBestPrice, m_lastPrice);

                // 撤单
                if (trade->execType == '4')
                {
                    // 获取订单编号，撤买/撤卖
                    if (trade->bidApplSeqNum != 0)
                    {
                        if (m_skipIds.find(trade->bidApplSeqNum) != m_skipIds.end()) return;
                        dropOrder(trade->bidApplSeqNum, trade->volume, time, OrderSideType::BUY, true);
                    }
                    else if (trade->offerApplSeqNum != 0)
                    {
                        if (m_skipIds.find(trade->offerApplSeqNum) != m_skipIds.end()) return;
                        dropOrder(trade->offerApplSeqNum, trade->volume, time, OrderSideType::SELL, true);
                    }

                    // 集合竞价期间模拟撮合最新价
                    if (phase == TradingPhase::OPEN_CALL_AUCTION)
                    {
                        m_lastPrice = calculateAuctionPrice(true);
                        m_matchLastPrice = m_lastPrice;
                    }
                    else if (phase == TradingPhase::CLOSE_CALL_AUCTION)
                    {
                        m_lastPrice = calculateAuctionPrice(false);
                        m_matchLastPrice = m_lastPrice;
                    }

                    if (!hasCrossedMainBook())
                    {
                        int64_t buyBestPrice = m_buyBook.bestPrice();
                        int64_t sellBestPrice = m_sellBook.bestPrice();
                        m_priceCage.set(buyBestPrice, sellBestPrice, m_lastPrice);
                        // 价格笼子边界可能变化，刷新笼内 best。
                        checkAndMovePendingOrders();
                    }
                    // 模拟撮合
                    tryMatchCrossedMainBook();
                }
                // 成交
                else if (trade->execType == 'F')
                {
                    // 更新最新成交价
                    m_lastPrice = trade->price;
                    dropOrder(trade->bidApplSeqNum, trade->volume, time, OrderSideType::BUY);
                    dropOrder(trade->offerApplSeqNum, trade->volume, time, OrderSideType::SELL);
                    if (!hasCrossedMainBook())
                    {
                        int64_t buyBestPrice = m_buyBook.bestPrice();
                        int64_t sellBestPrice = m_sellBook.bestPrice();
                        m_priceCage.set(buyBestPrice, sellBestPrice, m_lastPrice);
                        // 价格笼子边界可能变化，刷新笼内 best。
                        checkAndMovePendingOrders();
                    }
                    m_volumes += trade->volume;
                    m_turnover += trade->volume * trade->price;
                    // 真实成交会改变主簿；若上一条订单触发过模拟撮合，需要在这里清理或继续撮合。
                    tryMatchCrossedMainBook();
                }
            }
        };

        inline const std::vector<std::pair<int64_t, const PriceLevel*>> getBuyTopN(int n) const
        {
            return getTopN<PriceLevelBookGreat>(m_buyBook, n);
        }

        inline const std::vector<std::pair<int64_t, const PriceLevel*>> getSellTopN(int n) const
        {
            return getTopN<PriceLevelBookLess>(m_sellBook, n);
        }

        inline void getBuyTopN(int n, std::pair<int64_t, const PriceLevel*> *out) const
        {
            m_buyBook.topNToBuffer(n, out);
            return;
        }

        inline void getSellTopN(int n, std::pair<int64_t, const PriceLevel*> *out) const
        {
            m_sellBook.topNToBuffer(n, out);
            return;
        }

        // 按价格档位内的时间优先顺序遍历订单节点，对外屏蔽slot链表细节。
        template<typename Fn>
        inline void forEachLevelOrder(const PriceLevel* level, Fn&& fn) const
        {
            if (!level || !m_pool)
            {
                return;
            }

            const OrderPool* orderPool = m_pool.get();
            uint32_t slot = level->headSlot;
            while (slot != OrderNode::kInvalidSlot)
            {
                const OrderNode* node = orderPool->getBySlot(slot);
                if (!node)
                {
                    break;
                }
                fn(node);
                slot = node->nextSlot;
            }
        }

        inline const std::string printBuyPriceVolume(int64_t id, size_t count = 0) const
        {
            std::stringstream ss;
            ss << "buy " << m_buyBook.printPriceVolume(id, count) << "\n";
            return ss.str();
        }

        inline const std::string printSellPriceVolume(int64_t id, size_t count = 0) const
        {
            std::stringstream ss;
            ss << "sell " << m_sellBook.printPriceVolume(id, count) << "\n";
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
            if (!node || node->priceLevelPtr == nullptr)
            {
                return nullptr;
            }
            return node->priceLevelPtr;
        }

        inline OrderNode* getHeadNode(const PriceLevel* level) const
        {
            if (!level || !m_pool || level->headSlot == PriceLevel::kInvalidSlot)
            {
                return nullptr;
            }
            return m_pool->getBySlot(level->headSlot);
        }

        inline OrderNode* getNextNode(const OrderNode* node) const
        {
            if (!node || !m_pool || node->nextSlot == OrderNode::kInvalidSlot)
            {
                return nullptr;
            }
            return m_pool->getBySlot(node->nextSlot);
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
                LOG_ERROR(app_log::logger(), "getOrCreate price level failed, date:{} code:{} price:{}", m_date, m_code, price);
                return false;
            }
            m_pool->link(level, node);
            return true;
        }

        inline void eraseEmptyLevel(OrderSideType sideType, int64_t price, PriceLevel* level)
        {
            if (!level || level->totalVolume > 0)
            {
                return;
            }

            if (sideType == OrderSideType::BUY)
            {
                PriceLevel* lv = m_buyBook.find(price);
                if (lv && lv == level)
                {
                    m_buyBook.erase(price);
                }
            }
            else if (sideType == OrderSideType::SELL)
            {
                PriceLevel* lv = m_sellBook.find(price);
                if (lv && lv == level)
                {
                    m_sellBook.erase(price);
                }
            }
        }

        // 订单节点的 id 索引已经下沉到共享 OrderPool，
        // 析构时按本订单簿的各个价格簿遍历释放，避免误操作同 shard 里其他 orderbook 的节点。
        template<typename BookType>
        inline void releaseBookOrders(BookType& book) noexcept
        {
            if (!m_pool)
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
                    m_pool->free(node);
                    node = next;
                }
            }

            book.clear();
        }

        inline bool checkMoveBuyCage()
        {
            if (!m_priceCageEnabled) return false;
            int64_t basePrice = m_priceCage.getBuyBasePrice();
            if (basePrice <= 0)
            {
                return m_buyBook.clearCage();
            }

            return m_buyBook.refreshBestByCage([this](int64_t price)
            {
                return m_priceCage.isBuyPriceInCage(price);
            });
        }

        inline bool checkMoveSellCage()
        {
            if (!m_priceCageEnabled) return false;
            int64_t basePrice = m_priceCage.getSellBasePrice();
            if (basePrice <= 0)
            {
                return m_sellBook.clearCage();
            }

            const int64_t lowerPrice = m_priceCage.getSellCageLowerPrice(basePrice);
            return m_sellBook.refreshBestByCage([this](int64_t price)
            {
                return m_priceCage.isSellPriceInCage(price);
            });
        }

        // 按价格笼子刷新买卖簿的笼内 best_tick_ 与笼外 cage_tick_。
        inline void checkAndMovePendingOrders()
        {
            if (!m_priceCageEnabled)
            {
                return;
            }
            if (m_currentTime < 93000000000000L || m_currentTime >= 145700000000000L)
            {
                m_buyBook.clearCage();
                m_sellBook.clearCage();
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

        // ============ 连续竞价撮合核心实现 ============

        inline bool isContinuousCageTime() const
        {
            return m_currentTime >= 93000000000000L && m_currentTime < 145700000000000L;
        }

        /**
         * @brief 处理主订单簿中的价格相交（bid1 >= ask1）
         * @note 用于订单触发自动撮合，直到最优价不再相交
         */
        inline int64_t matchCrossedMainBook()
        {
            int32_t buyTick = m_buyBook.bestTick();
            int64_t buyPrice = tickToActivePrice(m_buyBook, buyTick);
            int32_t sellTick = m_sellBook.bestTick();
            int64_t sellPrice = tickToActivePrice(m_sellBook, sellTick);

            int64_t currentBuyPrice = buyPrice;
            int64_t currentSellPrice = sellPrice;

            int64_t consumed = 0;
            while (true)
            {
                // 涨跌停则跳过
                if (buyPrice <= 0 || sellPrice <= 0) break;
                // 撮合完成则跳过
                if (buyPrice < sellPrice)
                {
                    m_matchTick.buyTick = buyTick;
                    m_matchTick.sellTick = sellTick;
                    break;
                }

                PriceLevel *buyLevel = m_buyBook.find(buyPrice);
                PriceLevel *sellLevel = m_sellBook.find(sellPrice);
                if (!buyLevel || !sellLevel) break;

                // 获取订单队列
                OrderNode* buyNode = nullptr;
                bool buyNullLevel = false;
                uint32_t buyNodeSlot = OrderNode::kInvalidSlot;
                uint32_t buyNodeHeadSlot = OrderNode::kInvalidSlot;
                while (true)
                {
                    if (buyLevel->headSlot == PriceLevel::kInvalidSlot) [[unlinkly]]
                    {
                        buyNode = nullptr;
                        break;
                    }
                    if (buyLevel->matched || buyLevel->matchTotalVolume <= 0) [[unlinkly]]
                    {
                        buyNode = nullptr;
                        buyNullLevel = true;
                        break;
                    }
                    if (buyNodeHeadSlot == OrderNode::kInvalidSlot)  [[unlinkly]]
                    {
                        buyNodeHeadSlot = buyLevel->headSlot;
                        buyNodeSlot = buyNodeHeadSlot;
                    }
                    buyNode = m_pool->getBySlot(buyNodeSlot);
                    if (!buyNode || !buyNode->matched || buyNode->remainingMatchVolume > 0)
                    {
                        break;
                    }
                    buyNodeSlot = buyNode->nextSlot;
                    if (buyNodeHeadSlot == buyNodeSlot)  [[unlinkly]]
                    {
                        buyNode = nullptr;
                        buyNullLevel = true;
                        break;
                    }
                }
                if (buyNullLevel)
                {
                    buyTick = m_buyBook.nextActiveTick(buyTick);
                    buyPrice = tickToActivePrice(m_buyBook, buyTick);
                    continue;
                }

                OrderNode* sellNode = nullptr;
                bool sellNullLevel = false;
                uint32_t sellNodeSlot = OrderNode::kInvalidSlot;
                uint32_t sellNodeHeadSlot = OrderNode::kInvalidSlot;
                while (true)
                {
                    if (sellLevel->headSlot == PriceLevel::kInvalidSlot) [[unlinkly]]
                    {
                        sellNode = nullptr;
                        break;
                    }
                    if (sellLevel->matched || sellLevel->matchTotalVolume <= 0) [[unlinkly]]
                    {
                        sellNode = nullptr;
                        sellNullLevel = true;
                        break;
                    }
                    if (sellNodeHeadSlot == OrderNode::kInvalidSlot) [[unlinkly]]
                    {
                        sellNodeHeadSlot = sellLevel->headSlot;
                        sellNodeSlot = sellNodeHeadSlot;
                    }
                    sellNode = m_pool->getBySlot(sellNodeSlot);
                    if (!sellNode || !sellNode->matched || sellNode->remainingMatchVolume > 0)
                    {
                        break;
                    }
                    sellNodeSlot = sellNode->nextSlot;
                    if (sellNodeHeadSlot == sellNodeSlot) [[unlinkly]]
                    {
                        sellNode = nullptr;
                        sellNullLevel = true;
                        break;
                    }
                }
                if (sellNullLevel)
                {
                    sellTick = m_sellBook.nextActiveTick(sellTick);
                    sellPrice = tickToActivePrice(m_sellBook, sellTick);
                    continue;
                }
                if (!buyNode || !sellNode) break;

                // 获取这一次的撮合量
                int64_t tradeVol = std::min(buyNode->remainingMatchVolume, sellNode->remainingMatchVolume);
                if (tradeVol <= 0) break;

                // 获取是否主买/主卖
                bool isBuy = buyNode->id > sellNode->id;
                // 如果主买则以对手价成交
                int64_t matchPrice = isBuy ? sellPrice : buyPrice;

                buyNode->remainingMatchVolume -= tradeVol;
                sellNode->remainingMatchVolume -= tradeVol;
                buyLevel->matchTotalVolume -= tradeVol;
                sellLevel->matchTotalVolume -= tradeVol;

                m_matchLastPrice = matchPrice;
                m_matchVolumes += tradeVol;
                m_matchTurnover += m_matchLastPrice * tradeVol;

                m_matchChangedOrderNodeSlots.insert(buyNode->selfSlot);
                m_matchChangedOrderNodeSlots.insert(sellNode->selfSlot);
                m_matchChangedPriceLevels.insert(buyLevel);
                m_matchChangedPriceLevels.insert(sellLevel);

                if (buyNode->remainingMatchVolume <= 0)
                {
                    buyNode->matched = true;
                    buyNode->remainingMatchVolume = 0;
                    --buyLevel->matchOrderSize;
                }
                if (sellNode->remainingMatchVolume <= 0)
                {
                    sellNode->matched = true;
                    sellNode->remainingMatchVolume = 0;
                    --sellLevel->matchOrderSize;
                }
                if (buyLevel->matchTotalVolume <= 0)
                {
                    buyLevel->matched = true;
                    buyLevel->matchTotalVolume = 0;

                    buyTick = m_buyBook.nextActiveTick(buyTick);
                    buyPrice = tickToActivePrice(m_buyBook, buyTick);
                }
                if (sellLevel->matchTotalVolume <= 0)
                {
                    sellLevel->matched = true;
                    sellLevel->matchTotalVolume = 0;

                    sellTick = m_sellBook.nextActiveTick(sellTick);
                    sellPrice = tickToActivePrice(m_sellBook, sellTick);
                }

                // 撮合完成后，检查盘口是否变更，如果盘口变更则需要检查是否有价格入笼
                if (currentBuyPrice != buyPrice || currentSellPrice != sellPrice)
                {
                    m_matchPriceCage.set(buyPrice, sellPrice, m_matchLastPrice);
                    int64_t buyBasePrice = m_matchPriceCage.getBuyBasePrice();
                    if (buyBasePrice <= 0) break;
                    int64_t upperPrice = m_matchPriceCage.getBuyCageUpperPrice(buyBasePrice);
                    if (upperPrice > m_limitupPrice)
                    {
                        upperPrice = m_limitupPrice;
                    }
                    buyTick = m_buyBook.priceToTickChecked(upperPrice);
                    buyTick = m_buyBook.lastTick(buyTick);
                    buyPrice = tickToActivePrice(m_buyBook, buyTick);

                    int64_t sellBasePrice = m_matchPriceCage.getSellBasePrice();
                    if (sellBasePrice <= 0) break;
                    int64_t lowerPrice = m_matchPriceCage.getSellCageLowerPrice(sellBasePrice);
                    if (lowerPrice < m_limitdownPrice)
                    {
                        lowerPrice = m_limitdownPrice;
                    }
                    sellTick = m_sellBook.priceToTickChecked(lowerPrice);
                    sellTick = m_sellBook.lastTick(sellTick);
                    sellPrice = tickToActivePrice(m_sellBook, sellTick);
                }
                currentBuyPrice = buyPrice;
                currentSellPrice = sellPrice;
                consumed += tradeVol;
            }
            m_matchTick.buyTick = buyTick;
            m_matchTick.sellTick = sellTick;
            return consumed;
        }

        inline bool hasCrossedMainBook() const
        {
            int64_t bid1 = m_buyBook.bestPrice();
            int64_t ask1 = m_sellBook.bestPrice();
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

                    for (uint32_t slot = level->headSlot; slot != OrderNode::kInvalidSlot; )
                    {
                        OrderNode *orderNode = m_pool->getBySlot(slot);
                        if (!orderNode || !orderNode->isUse)
                        {
                            LOG_WARNING(app_log::logger(),
                                        "resetAllMatchStatus hit invalid order slot, date:{} code:{} price:{} slot:{}",
                                        m_date, m_code, level->price, slot);
                            break;
                        }

                        orderNode->resetMatchStatus();
                        slot = orderNode->nextSlot;
                    }

                    level->resetMatchStatus();
                }
            };

            resetBookMatchStatus(m_buyBook);
            resetBookMatchStatus(m_sellBook);

            m_matchChangedOrderNodeSlots.clear();
            m_matchChangedPriceLevels.clear();
            m_matchStatusReset = false;
            m_matchTick.buyTick = m_buyBook.bestTick();
            m_matchTick.sellTick = m_sellBook.bestTick();
        }

        inline void tryMatchCrossedMainBook()
        {
            if (!isContinuousCageTime()) return;

            bool needMatch = hasCrossedMainBook();
            if (needMatch)
            {
                matchCrossedMainBook();
                m_matchStatusReset = true;
            }
            else if (!m_matchChangedOrderNodeSlots.empty()
                     || !m_matchChangedPriceLevels.empty())
            {
                for (auto &slot : m_matchChangedOrderNodeSlots)
                {
                    OrderNode *orderNode = m_pool->getBySlot(slot);
                    if (orderNode)
                    {
                        orderNode->resetMatchStatus();
                    }
                }
                m_matchChangedOrderNodeSlots.clear();

                for (auto level : m_matchChangedPriceLevels)
                {
                    if (level)
                    {
                        level->resetMatchStatus();
                    }
                }
                m_matchChangedPriceLevels.clear();
                m_matchStatusReset = false;

                m_matchTick.buyTick = m_buyBook.bestTick();
                m_matchTick.sellTick = m_sellBook.bestTick();

                m_matchLastPrice = m_lastPrice;
                m_matchVolumes = m_volumes;
                m_matchTurnover = m_turnover;
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
#if 0
        // 原集合竞价价格计算实现，保留用于结果对照。
        inline int64_t calculateAuctionPrice(bool isOpenCall) const
        {
            if (m_buyBook.empty() || m_sellBook.empty())
                return 0;

            struct AuctionCandidate
            {
                int64_t price = 0;
                int64_t matchVolume = 0;
                int64_t imbalance = std::numeric_limits<int64_t>::max();
            };

            // Step 1: 从 levels 直接收集有效档位，各自排序一次，计算前缀累计量
            // buy_sorted: 降序，buy_prefix[i] = 所有 price >= buy_sorted[i].price 的累计买量
            std::vector<std::pair<int64_t, int64_t>> buySorted;
            buySorted.reserve(m_buyBook.size());
            std::vector<std::pair<int64_t, const PriceLevel*>> buyBookBuf(m_buyBook.size());
            m_buyBook.allToBuffer(buyBookBuf.data());
            for (const auto& [p, lv] : buyBookBuf)
            {
                if (lv->totalVolume > 0)
                    buySorted.emplace_back(p, lv->totalVolume);
            }
            std::sort(buySorted.begin(), buySorted.end(),
                      [](const auto& a, const auto& b){ return a.first > b.first; });

            // sell_sorted: 升序，sell_prefix[i] = 所有 price <= sell_sorted[i].price 的累计卖量
            std::vector<std::pair<int64_t, int64_t>> sellSorted;
            sellSorted.reserve(m_sellBook.size());
            std::vector<std::pair<int64_t, const PriceLevel*>> sellBookBuf(m_sellBook.size());
            m_sellBook.allToBuffer(sellBookBuf.data());
            for (const auto& [p, lv] : sellBookBuf)
            {
                if (lv->totalVolume > 0)
                    sellSorted.emplace_back(p, lv->totalVolume);
            }
            std::sort(sellSorted.begin(), sellSorted.end(),
                      [](const auto& a, const auto& b){ return a.first < b.first; });

            if (buySorted.empty() || sellSorted.empty())
                return 0;

            std::vector<int64_t> buyPrefix(buySorted.size());
            buyPrefix[0] = buySorted[0].second;
            for (size_t i = 1; i < buySorted.size(); ++i)
                buyPrefix[i] = buyPrefix[i - 1] + buySorted[i].second;

            std::vector<int64_t> sellPrefix(sellSorted.size());
            sellPrefix[0] = sellSorted[0].second;
            for (size_t i = 1; i < sellSorted.size(); ++i)
                sellPrefix[i] = sellPrefix[i - 1] + sellSorted[i].second;

            // 二分查找：price >= P 的买量（buy_sorted 降序，找最后一个 >= P 的下标）
            auto getBuyVol = [&](int64_t p) -> int64_t {
                int lo = 0, hi = static_cast<int>(buySorted.size()) - 1, res = -1;
                while (lo <= hi)
                {
                    int mid = lo + (hi - lo) / 2;
                    if (buySorted[mid].first >= p) { res = mid; lo = mid + 1; }
                    else { hi = mid - 1; }
                }
                return res < 0 ? 0 : buyPrefix[res];
            };

            // 二分查找：price <= P 的卖量（sell_sorted 升序，找最后一个 <= P 的下标）
            auto getSellVol = [&](int64_t p) -> int64_t {
                int lo = 0, hi = static_cast<int>(sellSorted.size()) - 1, res = -1;
                while (lo <= hi)
                {
                    int mid = lo + (hi - lo) / 2;
                    if (sellSorted[mid].first <= p) { res = mid; lo = mid + 1; }
                    else { hi = mid - 1; }
                }
                return res < 0 ? 0 : sellPrefix[res];
            };

            // Step 2: 遍历买卖档位的并集，每个候选价格 O(log N) 查询
            std::unordered_set<int64_t> allPrices;
            allPrices.reserve(buySorted.size() + sellSorted.size());
            for (const auto& [p, _] : buySorted) allPrices.insert(p);
            for (const auto& [p, _] : sellSorted) allPrices.insert(p);

            int64_t maxVolume = 0;
            int64_t minImbalance = std::numeric_limits<int64_t>::max();
            std::vector<AuctionCandidate> candidates;
            candidates.reserve(allPrices.size());

            for (int64_t price : allPrices)
            {
                int64_t buyVol  = getBuyVol(price);
                int64_t sellVol = getSellVol(price);
                int64_t matchVol = std::min(buyVol, sellVol);
                if (matchVol <= 0) continue;

                int64_t imbalance = std::abs(buyVol - sellVol);
                candidates.push_back({price, matchVol, imbalance});
                if (matchVol > maxVolume) maxVolume = matchVol;
            }

            if (maxVolume == 0 || candidates.empty()) return 0;

            // Step 3: 筛选最大成交量 + 最小不平衡量
            for (const auto& c : candidates)
            {
                if (c.matchVolume == maxVolume && c.imbalance < minImbalance)
                    minImbalance = c.imbalance;
            }

            std::vector<AuctionCandidate> finalists;
            finalists.reserve(candidates.size());
            for (const auto& c : candidates)
            {
                if (c.matchVolume == maxVolume && c.imbalance == minImbalance)
                    finalists.push_back(c);
            }

            if (finalists.empty()) return 0;
            if (finalists.size() == 1) return finalists.front().price;

            // Step 4: 交易所规则打破平手
            if (m_currentExchange == ExchangeType::SH)
            {
                // 上交所：取中间价，按最小变动单位取整
                std::sort(finalists.begin(), finalists.end(),
                    [](const AuctionCandidate& a, const AuctionCandidate& b){ return a.price < b.price; });
                double middlePrice = (static_cast<double>(finalists.front().price) / 1000000.0 +
                                       static_cast<double>(finalists.back().price) / 1000000.0) / 2.0;
                return static_cast<int64_t>(std::llround(roundTo(middlePrice, 2) * 1000000));
            }
            else if (m_currentExchange == ExchangeType::SZ)
            {
                // 深交所：取最接近基准价的档位（开盘→前收；收盘→最近成交）
                int64_t refPrice = m_preClosePrice;
                if (!isOpenCall)
                    refPrice = (m_lastPrice > 0) ? m_lastPrice : m_preClosePrice;

                std::sort(finalists.begin(), finalists.end(),
                    [refPrice](const AuctionCandidate& a, const AuctionCandidate& b){
                        return std::abs(a.price - refPrice) < std::abs(b.price - refPrice);
                    });
                return finalists.front().price;
            }
            return 0;
        }
#endif
        /*
         * 线性集合竞价价格计算。
         *
         * 一、候选价格和可成交量
         * --------------------------------
         * 集合竞价只需要检查“买盘价格与卖盘价格的并集”。假设当前候选价格为 P：
         *
         *   累计买量 buy(P)  = 所有委托价 >= P 的买单量
         *   累计卖量 sell(P) = 所有委托价 <= P 的卖单量
         *   可成交量         = min(buy(P), sell(P))
         *   不平衡量         = abs(buy(P) - sell(P))
         *
         * 最终先选择可成交量最大的价格；可成交量相同时，再选择不平衡量最小的价格。
         * 如果仍有多个价格并列，再按沪深交易所各自的规则决定最终价格。
         *
         * 二、为什么可以线性扫描
         * --------------------------------
         * PriceLevelBook::allToBuffer() 已经返回有序档位：
         *
         *   buyLevels  = 买盘价格从高到低，例如 [12, 10, 9]
         *   sellLevels = 卖盘价格从低到高，例如 [8, 10, 11]
         *
         * 从 buyLevels 尾部向前走，就能得到买盘升序 [9, 10, 12]。这样买卖两个数组
         * 都是升序，可以像归并排序一样，每次取两边较小的价格作为下一个候选价格。
         * 同一个价格同时出现在买卖两边时，只计算一次。
         *
         * 三、扫描时维护的两个不变量
         * --------------------------------
         *   buyRemaining：扫描到候选价 P 时，仍包含所有价格 >= P 的买量。
         *   sellAccumulated：扫描到候选价 P 时，已经包含所有价格 <= P 的卖量。
         *
         * 所以处理价格 P 的顺序必须是：
         *
         *   1. 先把价格 P 的卖量加入 sellAccumulated，使卖量包含“<= P”；
         *   2. 用当前 buyRemaining 和 sellAccumulated 计算 P 的成交量；
         *   3. 计算完成后才扣除价格 P 的买量，为下一个更高候选价做准备。
         *
         * 例如买盘为 12元/100、10元/200，卖盘为 9元/80、10元/150：
         *
         *   P=9 ：buy=300，sell=80， match=80
         *   P=10：buy=300，sell=230，match=230；计算后扣除10元买量200
         *   P=12：buy=100，sell=230，match=100
         *
         * 四、性能特征
         * --------------------------------
         * 整个过程只遍历一次买卖档位，复杂度为 O(B + S)。它不再创建 unordered_set、
         * 前缀和、候选数组，也不再排序和逐候选价二分查询。thread_local buffer 由每个
         * LOB worker 线程复用，只有遇到比历史更大的盘口时才可能扩容。
         */
        inline int64_t calculateAuctionPrice(bool isOpenCall) const
        {
            if (m_buyBook.empty() || m_sellBook.empty())
            {
                return 0;
            }

            using LevelItem = std::pair<int64_t, const PriceLevel *>;
            struct AuctionScratch
            {
                std::vector<LevelItem> buyLevels;
                std::vector<LevelItem> sellLevels;
            };
            // 每个 worker 线程持有一份临时数组，处理不同股票时重复使用，避免逐笔 new/free。
            thread_local AuctionScratch scratch;

            // resize 在容量足够时不会重新分配；allToBuffer 会覆盖本次实际使用的元素。
            scratch.buyLevels.resize(m_buyBook.size());
            scratch.sellLevels.resize(m_sellBook.size());

            // buyLevels 为高价到低价，sellLevels 为低价到高价。
            const int buyCount = m_buyBook.allToBuffer(scratch.buyLevels.data());
            const int sellCount = m_sellBook.allToBuffer(scratch.sellLevels.data());
            if (buyCount <= 0 || sellCount <= 0)
            {
                return 0;
            }

            // 升序扫描从最低候选价开始。最低价处“价格 >= P”的买量就是全部有效买量。
            int64_t buyRemaining = 0;
            for (int i = 0; i < buyCount; ++i)
            {
                const PriceLevel *level = scratch.buyLevels[static_cast<size_t>(i)].second;
                if (level != nullptr && level->totalVolume > 0)
                {
                    buyRemaining += level->totalVolume;
                }
            }

            if (buyRemaining <= 0)
            {
                return 0;
            }

            int buyIndex = buyCount - 1; // 买盘 buffer 为降序，从尾部开始得到升序。
            int sellIndex = 0;           // 卖盘 buffer 已经是升序。
            int64_t sellAccumulated = 0;

            // 前两个字段对应前两级选择规则：最大成交量、最小不平衡量。
            int64_t maxMatchVolume = 0;
            int64_t minImbalance = std::numeric_limits<int64_t>::max();

            // 上海规则只需要知道所有并列最终候选价的最低价和最高价，最后取二者中间价。
            int64_t finalistMinPrice = 0;
            int64_t finalistMaxPrice = 0;

            // 深圳规则直接在线保存“距离参考价最近”的候选价，不需要保留候选数组。
            int64_t szBestPrice = 0;
            uint64_t szBestDistance = std::numeric_limits<uint64_t>::max();

            const int64_t szReferencePrice = isOpenCall
                ? m_preClosePrice
                : ((m_lastPrice > 0) ? m_lastPrice : m_preClosePrice);

            // 正常情况下 active level 的总量应大于 0；这里跳过异常/过渡状态，保持原实现语义。
            auto skipInvalidBuyLevels = [&]()
            {
                while (buyIndex >= 0)
                {
                    const PriceLevel *level = scratch.buyLevels[static_cast<size_t>(buyIndex)].second;
                    if (level != nullptr && level->totalVolume > 0)
                    {
                        break;
                    }
                    --buyIndex;
                }
            };

            auto skipInvalidSellLevels = [&]()
            {
                while (sellIndex < sellCount)
                {
                    const PriceLevel *level = scratch.sellLevels[static_cast<size_t>(sellIndex)].second;
                    if (level != nullptr && level->totalVolume > 0)
                    {
                        break;
                    }
                    ++sellIndex;
                }
            };

            auto priceDistance = [](int64_t lhs, int64_t rhs) -> uint64_t
            {
                return lhs >= rhs
                    ? static_cast<uint64_t>(lhs) - static_cast<uint64_t>(rhs)
                    : static_cast<uint64_t>(rhs) - static_cast<uint64_t>(lhs);
            };

            skipInvalidBuyLevels();
            skipInvalidSellLevels();

            while (buyIndex >= 0 || sellIndex < sellCount)
            {
                // 某一侧遍历完成后用 max() 作为哨兵，候选价格自然取仍有数据的另一侧。
                const int64_t buyPrice = buyIndex >= 0
                    ? scratch.buyLevels[static_cast<size_t>(buyIndex)].first
                    : std::numeric_limits<int64_t>::max();

                const int64_t sellPrice = sellIndex < sellCount
                    ? scratch.sellLevels[static_cast<size_t>(sellIndex)].first
                    : std::numeric_limits<int64_t>::max();
                    
                const int64_t candidatePrice = std::min(buyPrice, sellPrice);

                // 记录当前候选价上的买量，但暂时不从 buyRemaining 扣除。
                // 原因是价格恰好等于 P 的买单满足“委托价 >= P”，必须参与 P 的计算。
                int64_t buyVolumeAtPrice = 0;
                if (buyPrice == candidatePrice)
                {
                    buyVolumeAtPrice = scratch.buyLevels[static_cast<size_t>(buyIndex)].second->totalVolume;
                    --buyIndex;
                }

                // 价格恰好等于 P 的卖单满足“委托价 <= P”，必须先加入再计算。
                if (sellPrice == candidatePrice)
                {
                    sellAccumulated += scratch.sellLevels[static_cast<size_t>(sellIndex)].second->totalVolume;
                    ++sellIndex;
                }

                // 此时两个累计量恰好分别代表 buy(P) 和 sell(P)。
                const int64_t matchVolume = std::min(buyRemaining, sellAccumulated);
                if (matchVolume > 0)
                {
                    const int64_t imbalance = buyRemaining >= sellAccumulated
                        ? buyRemaining - sellAccumulated
                        : sellAccumulated - buyRemaining;

                    // better：当前价格在“成交量优先、不平衡量次优”的字典序下更好。
                    // tied：前两级规则完全相同，需要交给交易所最终规则选择。
                    const bool better = matchVolume > maxMatchVolume ||
                        (matchVolume == maxMatchVolume && imbalance < minImbalance);
                    const bool tied = matchVolume == maxMatchVolume && imbalance == minImbalance;

                    if (better)
                    {
                        // 出现更优候选后，之前保存的所有并列候选全部失效，从当前价格重新开始。
                        maxMatchVolume = matchVolume;
                        minImbalance = imbalance;
                        finalistMinPrice = candidatePrice;
                        finalistMaxPrice = candidatePrice;
                        szBestPrice = candidatePrice;
                        szBestDistance = priceDistance(candidatePrice, szReferencePrice);
                    }
                    else if (tied)
                    {
                        // 上海只扩展并列候选区间；深圳则比较当前价格与参考价的距离。
                        finalistMinPrice = std::min(finalistMinPrice, candidatePrice);
                        finalistMaxPrice = std::max(finalistMaxPrice, candidatePrice);

                        const uint64_t distance = priceDistance(candidatePrice, szReferencePrice);
                        if (distance < szBestDistance ||
                            (distance == szBestDistance && candidatePrice < szBestPrice))
                        {
                            szBestPrice = candidatePrice;
                            szBestDistance = distance;
                        }
                    }
                }

                // 当前价格的买量只属于当前及更低候选价；扫描到更高价格前将其扣除。
                buyRemaining -= buyVolumeAtPrice;
                skipInvalidBuyLevels();
                skipInvalidSellLevels();
            }

            if (maxMatchVolume <= 0)
            {
                return 0;
            }

            if (m_currentExchange == ExchangeType::SH)
            {
                // 上海：多个最终候选价取最低价与最高价的中间价，再按最小价格单位取整。
                const double middlePrice =
                    (static_cast<double>(finalistMinPrice) / 1000000.0 +
                     static_cast<double>(finalistMaxPrice) / 1000000.0) / 2.0;
                return static_cast<int64_t>(std::llround(roundTo(middlePrice, 2) * 1000000));
            }
            if (m_currentExchange == ExchangeType::SZ)
            {
                // 深圳：开盘参考前收价，收盘参考最近成交价；等距离时固定选择较低价，
                // 避免原 unordered_set 遍历顺序导致结果不稳定。
                return szBestPrice;
            }
            return 0;
        }

        inline void addOrder(int64_t id, int64_t price, int64_t volume, int64_t time, OrderSideType sideType, MarketOrderType orderType)
        {
            m_currentTime = time;
            // OrderPool 内部已经维护全局 id->slot 索引，这里直接复用
            if (m_pool && m_pool->find(id) != nullptr)
            {
                LOG_WARNING(app_log::logger(), "dup order id:{}, ignore addOrder", id);
                return;
            }

            // 处理延迟订单
            if (!m_pendingTrade.empty())
            {
                auto pendingItor = m_pendingTrade.find(id);
                if (pendingItor != m_pendingTrade.end())
                {
                    if (pendingItor->second >= volume)
                    {
                        pendingItor->second -= volume;

                        if (pendingItor->second == 0)
                            m_pendingTrade.erase(pendingItor);

                        return;
                    }

                    volume -= pendingItor->second;
                    m_pendingTrade.erase(pendingItor);
                }
            }

            if (volume == 0) return;

            OrderNode *op = m_pool->alloc(id);
            if (!op)
            {
                return;
            }

            op->price = price;
            op->volume = volume;
            op->remainingMatchVolume = volume;
            op->time = time;
            op->orderType = orderType;
            op->sideType = sideType;

            if (sideType == OrderSideType::BUY)
            {
                if (!linkNodeToBook(m_buyBook, price, op))
                {
                    m_pool->free(op);
                    return;
                }
            }
            else if (sideType == OrderSideType::SELL)
            {
                if (!linkNodeToBook(m_sellBook, price, op))
                {
                    m_pool->free(op);
                    return;
                }
            }
        }

        inline void syncMatchStateAfterBookUpdate(OrderNode* orderNode, PriceLevel* level, bool isCancel)
        {
            if (m_matchStatusReset && !isCancel)
            {
                return;
            }

            if (orderNode)
            {
                orderNode->resetMatchStatus();
            }
            if (level)
            {
                level->resetMatchStatus();
            }
        }

        inline void dropOrder(int64_t id, int64_t volume, int64_t time, OrderSideType sideType, bool isCancel = false, bool ignore = false)
        {
            m_currentTime = time;
            OrderNode* orderNode = m_pool ? m_pool->find(id) : nullptr;

            // 乱序成交
            if (orderNode == nullptr)
            {
                if (ignore)
                {
                    return;
                }
                m_pendingTrade[id] += volume;
                LOG_WARNING(app_log::logger(), "get loss order id:{} volume:{}", id, volume);
                return;
            }

            if (orderNode->price == -1)
            {
                LOG_ERROR(app_log::logger(), "drop order, null order node, id:{} volume:{}", id, volume);
                STDTHROW(STD_ERROR_CODE, "drop order, null order node, id:"<<id<<" volume:"<<volume, "drop order, null order node, id:"<<id<<" volume:"<<volume);
                return;
            }

            // 深交所 发单1000 成交500 剩余500撤单，这种情况下撤单的volume为1000，如果不特殊处理会导致total_volume超减
            // 取order->volume和need drop volume的最小值应对上述情况
            int64_t targetVolume = std::min(orderNode->volume, volume);
            // 如果order的volume<=需要删除的vol则直接删除
            if (orderNode->volume <= targetVolume)
            {
                PriceLevel* priceLevel = getPriceLevel(orderNode);
                m_pool->unlink(orderNode);
                syncMatchStateAfterBookUpdate(nullptr, priceLevel, isCancel);

                // 如果 volume 为 0 则删除价格档位；笼外订单仍在同一本簿内。
                eraseEmptyLevel(sideType, orderNode->price, priceLevel);
                // 释放对象
                m_pool->free(orderNode);
            }
            // 部分成交/撤单，直接修改订单中的volume
            else
            {
                // 对应价格档位总量减
                PriceLevel* level = getPriceLevel(orderNode);
                if (level)
                {
                    level->totalVolume -= targetVolume;
                }
                // OrderNode量减
                orderNode->volume -= targetVolume;
                syncMatchStateAfterBookUpdate(orderNode, level, isCancel);
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
            if (time < 91500000000000L)
                return TradingPhase::PRE_OPEN;
            else if (time >= 91500000000000L && time < 92500000000000L)
                return TradingPhase::OPEN_CALL_AUCTION;
            else if (time >= 92500000000000L && time < 93000000000000L)
                return TradingPhase::OPEN_CALL_MATCH;
            else if (time >= 93000000000000L && time < 113000000000000L)
                return TradingPhase::CONTINUOUS_TRADING;
            else if (time >= 113000000000000L && time < 130000000000000L)
                return TradingPhase::PRE_OPEN;  // 午休
            else if (time >= 130000000000000L && time < 145700000000000L)
                return TradingPhase::CONTINUOUS_TRADING;
            else if (time >= 145700000000000L && time < 150000000000000L)
                return TradingPhase::CLOSE_CALL_AUCTION;
            else
                return TradingPhase::CLOSED;
        }

        inline void handlePhaseTransition(TradingPhase newPhase, int64_t time)
        {
            // 最后一个订单是92500000L前收到，下一个订单是93000000L
            if (m_currentPhase == TradingPhase::OPEN_CALL_MATCH &&
                newPhase == TradingPhase::CONTINUOUS_TRADING)
            {
                resetAllMatchStatus();
            }

            m_currentPhase = newPhase;
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
        std::unordered_map<int64_t, int64_t> m_pendingTrade;
        // 对象池
        std::shared_ptr<OrderPool> m_pool;
        // 跳过的订单id，主要存在于市价单，当对手方无对手价/本方无最优价，则发单立即撤单，视为废单，不进入订单簿
        std::unordered_set<int64_t> m_skipIds;

        // 价格队列
        PriceLevelBookGreat m_buyBook; // 买单从最高价开始
        PriceLevelBookLess m_sellBook; // 卖单从最高价开始

        // 统计
        int64_t m_currentTime = 0; // 维护最新时间，用于检查订单簿
        int64_t m_currentId = 0; // 最新id
        int64_t m_volumes = 0; // 总量
        int64_t m_turnover = 0; // 总成交额

        // 价格笼子基准价相关
        int64_t m_lastPrice = 0; // 最新价
        int64_t m_preClosePrice = 0;  // 昨收价
        int64_t m_limitupPrice = 0;  // 涨停价
        int64_t m_limitdownPrice = 0;  // 跌停价
        bool m_priceCageEnabled = false; // 是否开启价格笼子
        bool m_priceCageAmain = false;   // 是否主板价格笼子
        PriceCage m_priceCage;
        MatchingStartTick m_matchTick;
        bool m_matchStatusReset = false;

        // 模拟撮合
        int64_t m_matchIdCounter = 0;  // 撮合自增id
        int64_t m_matchLastPrice = 0;  // 撮合 最新价
        int64_t m_matchVolumes = 0; // 撮合总量
        int64_t m_matchTurnover = 0; // 撮合总金额
        ExchangeType m_currentExchange = ExchangeType::UNKNOWN; // 市场
        MatchCallback m_matchCallback = nullptr; // 回调指针
        PriceCage m_matchPriceCage; // 撮合 价格笼子
        std::unordered_set<PriceLevel*> m_matchChangedPriceLevels; // 撮合期间修改的PriceLevel指针
        std::unordered_set<uint32_t> m_matchChangedOrderNodeSlots; // 撮合期间修改的OrderNode的Slot

        TradingPhase m_currentPhase = TradingPhase::PRE_OPEN;
        uint8_t m_exchange;
        std::string m_date = "";
        std::string m_code = "";
    };

}
}

#endif
