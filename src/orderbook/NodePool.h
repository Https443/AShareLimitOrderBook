#pragma once
#include <cstdint>
#include <limits>
#include <cassert>
#include "util/logger.h"

namespace marketdata
{
namespace orderbook
{
    enum class OrderSideType : uint8_t
    {
        BUY = 0,                // 买
        SELL,                   // 卖
        NONE                    // 未知
    };

    // 订单类型
    enum class MarketOrderType : uint8_t
    {
        LIMIT = 0,              // 限价单
        MARKET_BEST_OPPONENT,   // 市价单-对手方最优（深交所）
        MARKET_BEST_SELF,       // 市价单-本方最优
        MARKET_FIVE_LEVEL,      // 市价单-最优五档即时成交剩余撤销（深交所）
        MARKET_IMMEDIATE,       // 市价单-即时成交剩余撤销（深交所）
        MARKET_ALL_OR_CANCEL,   // 市价单-全额成交或撤销（深交所）
        MARKET_FIVE_LEVEL_TO_LIMIT, // 市价单-最优五档即时成交剩余转限价（上交所）
        MARKET,                 // 未知市价单
        NONE                    // 未知（对应上交所）
    };

    // 价格档位
    struct PriceLevel
    {
        static constexpr uint32_t kInvalidSlot = std::numeric_limits<uint32_t>::max();

        // 是否在使用
        bool isUse = false;
        // 已被撮合
        bool matched = false;
        // 占位
        char remark[6] = {0};
        // 价格
        int64_t price = -1;

        // 首订单节点指针
        uint32_t headSlot = kInvalidSlot;
        // 尾订单节点指针
        uint32_t tailSlot = kInvalidSlot;
        // 总委托量
        int64_t totalVolume = 0;
        // 撮合后的总委托量
        int64_t matchTotalVolume = 0;
        // 委托数量
        int32_t orderSize = 0;
        // 撮合后的委托数量
        int32_t matchOrderSize = 0;

        void reset()
        {
            isUse = false;
            matched = false;
            price = -1;
            headSlot = kInvalidSlot;
            tailSlot = kInvalidSlot;
            totalVolume = 0;
            matchTotalVolume = 0;
            orderSize = 0;
            matchOrderSize = 0;
        }

        bool empty() const
        {
            return headSlot == kInvalidSlot;
        }

        void resetMatchStatus()
        {
            matched = false;
            matchTotalVolume = totalVolume;
            matchOrderSize = orderSize;
        }
    };

    // 订单节点
    struct OrderNode
    {
        static constexpr uint32_t kInvalidSlot = std::numeric_limits<uint32_t>::max();

        bool isUse = false;
        // 委托方向
        OrderSideType sideType = OrderSideType::NONE;
        // 委托类型
        MarketOrderType orderType = MarketOrderType::NONE;
        // 已撮合完成
        bool matched = false;

        // 当前订单slot
        uint32_t selfSlot = kInvalidSlot;
        // 前一个订单节点slot
        uint32_t prevSlot = kInvalidSlot;
        // 后一个订单节点slot
        uint32_t nextSlot = kInvalidSlot;

        // id
        int64_t id = -1;
        // 委托价格
        int64_t price = -1;
        // 委托量
        int64_t volume = -1;
        // 剩余委托量
        int64_t remainingMatchVolume = -1;
        // 委托时间
        int64_t time = -1;

        // 订单节点所在的价格档位指针
        PriceLevel *priceLevelPtr = nullptr;

        void reset()
        {
            isUse = false;
            sideType = OrderSideType::NONE;
            orderType = MarketOrderType::NONE;
            matched = false;
            id = -1;
            price = -1;
            volume = -1;
            remainingMatchVolume = -1;
            time = -1;
            prevSlot = kInvalidSlot;
            nextSlot = kInvalidSlot;
            priceLevelPtr = nullptr;
        }

        void resetMatchStatus()
        {
            matched = false;
            remainingMatchVolume = volume;
        }
    };

    class OrderIdIndex
    {
    public:
        static constexpr uint32_t kInvalidSlot = OrderNode::kInvalidSlot;
        static constexpr size_t kTableCapacity = 1u << 25;   // 按需要调整，必须是2的幂

        struct Entry
        {
            int64_t orderId = -1;
            uint32_t slot = kInvalidSlot;
            uint8_t state = 0; // 0-empty, 1-used, 2-tombstone
        };

        void init()
        {
            clear();
            m_mask = kTableCapacity - 1;
        }

        bool insert(int64_t orderId, uint32_t slot)
        {
            if ((m_size + 1) * 10 >= kTableCapacity * 7)
            {
                LOG_ERROR(app_log::logger(), "OrderIdIndex load factor too high, size:{}, capacity:{}", m_size, kTableCapacity);
                return false;
            }

            size_t idx = hash(orderId) & m_mask;
            size_t firstTombstone = kTableCapacity;

            for (;;)
            {
                Entry& e = m_table[idx];

                if (e.state == 0)
                {
                    size_t target = (firstTombstone != kTableCapacity) ? firstTombstone : idx;
                    m_table[target].orderId = orderId;
                    m_table[target].slot = slot;
                    m_table[target].state = 1;
                    ++m_size;
                    return true;
                }

                if (e.state == 2)
                {
                    if (firstTombstone == kTableCapacity)
                    {
                        firstTombstone = idx;
                    }
                }
                else if (e.orderId == orderId)
                {
                    return false;
                }

                idx = (idx + 1) & m_mask;
            }
        }

        bool find(int64_t orderId, uint32_t& slot) const
        {
            size_t idx = hash(orderId) & m_mask;

            for (;;)
            {
                const Entry& e = m_table[idx];

                if (e.state == 0)
                {
                    return false;
                }

                if (e.state == 1 && e.orderId == orderId)
                {
                    slot = e.slot;
                    return true;
                }

                idx = (idx + 1) & m_mask;
            }
        }

        bool erase(int64_t orderId)
        {
            size_t idx = hash(orderId) & m_mask;

            for (;;)
            {
                Entry& e = m_table[idx];

                if (e.state == 0)
                {
                    return false;
                }

                if (e.state == 1 && e.orderId == orderId)
                {
                    e.state = 2;
                    e.slot = kInvalidSlot;
                    --m_size;
                    return true;
                }

                idx = (idx + 1) & m_mask;
            }
        }

        void clear()
        {
            for (size_t i = 0; i < kTableCapacity; ++i)
            {
                m_table[i] = Entry{};
            }
            m_size = 0;
            m_mask = kTableCapacity - 1;
        }

    private:
        static uint64_t hash(int64_t x)
        {
            uint64_t v = static_cast<uint64_t>(x);
            v ^= v >> 33;
            v *= 0xff51afd7ed558ccdULL;
            v ^= v >> 33;
            v *= 0xc4ceb9fe1a85ec53ULL;
            v ^= v >> 33;
            return v;
        }

        Entry m_table[kTableCapacity];
        size_t m_mask = kTableCapacity - 1;
        size_t m_size = 0;
    };

    /**
     * @brief 订单内存池
     * @note 预分配x个订单节点，使用链表管理空闲节点
     *       避免频繁的堆内存分配，提高性能
     */
    class OrderPool
    {
    public:
        // 选近十年逐笔最多数据的进行测试
        // 20250827共487643713条数据，最高占pool size 15411861
        // 20251231共362069115条数据，最高占pool size 12856644
        static constexpr size_t kMaxOrder = 16'000'000;
        static constexpr uint32_t kInvalidSlot = OrderNode::kInvalidSlot;

        /**
         * @brief 构造函数，初始化内存池
         * @note 预分配MAX_ORDER个节点，通过next指针串成空闲链表
         */
        OrderPool()
        {
            for (uint32_t i = 0; i < kMaxOrder; ++i)
            {
                m_nodes[i].selfSlot = i;
                m_nodes[i].reset();
                m_freeStack[i] = static_cast<uint32_t>(kMaxOrder - 1 - i);
            }
            m_freeTop = static_cast<uint32_t>(kMaxOrder);
            m_idIndex.init();
        }

        /**
         * @brief 从内存池分配一个节点
         * @return 节点指针，池耗尽返回nullptr
         */
        OrderNode* alloc(int64_t orderId)
        {
            if (m_freeTop == 0) [[unlikely]]
            {
                LOG_ERROR(app_log::logger(), "OrderPool: out of max size:{}", kMaxOrder);
                return nullptr;
            }

            uint32_t existSlot = kInvalidSlot;
            if (m_idIndex.find(orderId, existSlot)) [[unlikely]]
            {
                LOG_ERROR(app_log::logger(), "OrderPool: duplicated order id:{}", orderId);
                return nullptr;
            }

            const uint32_t slot = m_freeStack[--m_freeTop];
            OrderNode& node = m_nodes[slot];
            node.reset();
            node.isUse = true;
            node.id = orderId;

            if (!m_idIndex.insert(orderId, slot)) [[unlikely]]
            {
                m_freeStack[m_freeTop++] = slot;
                node.reset();
                LOG_ERROR(app_log::logger(), "OrderPool: failed to insert id index, id:{}", orderId);
                return nullptr;
            }

            ++m_count;
            if (m_count > m_maxCount)
            {
                m_maxCount = m_count;
            }

            return &node;
        }

        OrderNode* find(int64_t orderId)
        {
            uint32_t slot = kInvalidSlot;
            if (!m_idIndex.find(orderId, slot))
            {
                return nullptr;
            }
            return &m_nodes[slot];
        }

        const OrderNode* find(int64_t orderId) const
        {
            uint32_t slot = kInvalidSlot;
            if (!m_idIndex.find(orderId, slot))
            {
                return nullptr;
            }
            return &m_nodes[slot];
        }

        /**
         * @brief 释放节点回内存池
         * @param p 要释放的节点指针
         * @note 重置节点字段并归还到空闲链表头部
         */
        void free(OrderNode* p)
        {
            if (!p) return;
            if (!p->isUse) return;

            if (p->priceLevelPtr != nullptr)
            {
                unlink(p);
            }

            m_idIndex.erase(p->id);
            const uint32_t slot = p->selfSlot;
            p->reset();
            p->selfSlot = slot;
            m_freeStack[m_freeTop++] = slot;

            if (m_count > 0)
            {
                --m_count;
            }
        }

        /**
         * @brief 将订单节点追加到价格档位的尾部
         * @param level 目标价格档位指针
         * @param node 要追加的订单节点指针
         * @note 新订单正常情况下追加到尾部，遵循时间优先原则
         */
        inline void link(PriceLevel* level, OrderNode* node)
        {
            assert(level != nullptr);
            assert(node != nullptr);
            assert(node->isUse);

            const uint32_t slot = node->selfSlot;
            node->prevSlot = level->tailSlot;
            node->nextSlot = kInvalidSlot;

            if (level->tailSlot == kInvalidSlot)
            {
                level->headSlot = slot;
                level->tailSlot = slot;
            }
            else
            {
                m_nodes[level->tailSlot].nextSlot = slot;
                level->tailSlot = slot;
            }

            level->totalVolume += node->volume;
            level->matchTotalVolume = level->totalVolume;

            ++level->orderSize;
            level->matchOrderSize = level->orderSize;

            node->priceLevelPtr = level;
        }

        /**
         * @brief 从价格档位中移除订单节点
         * @param node 要移除的订单节点指针
         * @note 维护双向链表的连接关系，更新价格档位的总量和订单数
         */
        inline void unlink(OrderNode* node)
        {
            assert(node != nullptr);

            if (node->priceLevelPtr == nullptr)
            {
                return;
            }

            PriceLevel *level = node->priceLevelPtr;
            const uint32_t prev = node->prevSlot;
            const uint32_t next = node->nextSlot;

            // 将当前OrderNode的next指针赋值给 前一个OrderNode的next指针
            if (prev != kInvalidSlot)
            {
                m_nodes[prev].nextSlot = next;
            }

            // 将当前OrderNode的prev指针赋值给 下一个OrderNode的prev指针
            if (next != kInvalidSlot)
            {
                m_nodes[next].prevSlot = prev;
            }

            // 如果当前OrderNode位于队列的头，则将下一个OrderNode设置为队列头
            if (level->headSlot == node->selfSlot)
            {
                level->headSlot = next;
            }

            // 如果当前OrderNode位于队列的尾，则将前一个OrderNode设置为队列尾
            if (level->tailSlot == node->selfSlot)
            {
                level->tailSlot = prev;
            }

            // 修改总volume
            level->totalVolume -= node->volume;
            --level->orderSize;

            node->prevSlot = kInvalidSlot;
            node->nextSlot = kInvalidSlot;

            node->priceLevelPtr = nullptr;
        }

        void reset()
        {
            for (uint32_t i = 0; i < kMaxOrder; ++i)
            {
                m_nodes[i].selfSlot = i;
                m_nodes[i].reset();
                m_freeStack[i] = static_cast<uint32_t>(kMaxOrder - 1 - i);
            }
            m_freeTop = static_cast<uint32_t>(kMaxOrder);
            m_idIndex.clear();
            m_count = 0;
            m_maxCount = 0;
        }

        /** @brief 获取当前已分配节点数 */
        size_t getCount() const
        {
            return m_count;
        }

        /** @brief 获取历史最大已分配节点数 */
        size_t getMaxCount() const
        {
            return m_maxCount;
        }

        OrderNode* getBySlot(uint32_t slot)
        {
            return (slot < kMaxOrder) ? &m_nodes[slot] : nullptr;
        }

        const OrderNode* getBySlot(uint32_t slot) const
        {
            return (slot < kMaxOrder) ? &m_nodes[slot] : nullptr;
        }

    private:
        OrderNode m_nodes[kMaxOrder];
        uint32_t m_freeStack[kMaxOrder];
        uint32_t m_freeTop = 0;
        size_t m_count = 0;
        size_t m_maxCount = 0;
        OrderIdIndex m_idIndex;
    };
}
}