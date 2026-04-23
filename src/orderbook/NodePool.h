#pragma once
#include <cstdint>
#include <limits>
#include <cassert>
#include "util/logger.h"

namespace marketdata
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
        static constexpr uint32_t INVALID_SLOT = std::numeric_limits<uint32_t>::max();

        // 是否在使用
        bool is_use = false;
        // 占位
        char remark[3] = {0, 0, 0};
        // 价格档位槽位
        uint32_t self_slot = INVALID_SLOT;
        // 价格
        int64_t price = -1;

        // 首订单节点指针
        uint32_t head_slot = INVALID_SLOT;
        // 尾订单节点指针
        uint32_t tail_slot = INVALID_SLOT;
        // 总委托量
        int64_t total_volume = 0;
        // 委托数量
        int32_t order_size = 0;

        void reset()
        {
            is_use = false;
            self_slot = INVALID_SLOT;
            price = -1;
            head_slot = INVALID_SLOT;
            tail_slot = INVALID_SLOT;
            total_volume = 0;
            order_size = 0;
        }

        bool empty() const
        {
            return head_slot == INVALID_SLOT;
        }
    };

    // 订单节点
    struct OrderNode
    {
        static constexpr uint32_t INVALID_SLOT = std::numeric_limits<uint32_t>::max();

        bool is_use = false;
        // 委托方向
        OrderSideType side_type = OrderSideType::NONE;
        // 委托类型
        MarketOrderType order_type = MarketOrderType::NONE;
        // 占位
        char remark = 0;
        // 用于五档即成剩撤统计
        int levels_count = 0;

        // id
        int64_t id = -1;
        // 委托价格
        int64_t price = -1;
        // 委托量
        int64_t volume = -1;
        // 原始委托量
        int64_t original_volume = -1;
        // 委托时间
        int64_t time = -1;

        // 当前订单slot
        uint32_t self_slot = INVALID_SLOT;
        // 前一个订单节点slot
        uint32_t prev_slot = INVALID_SLOT;
        // 后一个订单节点slot
        uint32_t next_slot = INVALID_SLOT;
        // 订单节点所在的价格档位slot
        uint32_t price_level_slot = INVALID_SLOT;

        void reset_runtime_fields()
        {
            is_use = false;
            side_type = OrderSideType::NONE;
            order_type = MarketOrderType::NONE;
            remark = 0;
            levels_count = 0;
            id = -1;
            price = -1;
            volume = -1;
            original_volume = -1;
            time = -1;
            prev_slot = INVALID_SLOT;
            next_slot = INVALID_SLOT;
            price_level_slot = INVALID_SLOT;
        }
    };

    class OrderIdIndex
    {
    public:
        static constexpr uint32_t INVALID_SLOT = OrderNode::INVALID_SLOT;
        static constexpr size_t TABLE_CAPACITY = 1u << 25;   // 按需要调整，必须是2的幂

        struct Entry
        {
            int64_t order_id = -1;
            uint32_t slot = INVALID_SLOT;
            uint8_t state = 0; // 0-empty, 1-used, 2-tombstone
        };

        void init()
        {
            clear();
            mask = TABLE_CAPACITY - 1;
        }

        bool insert(int64_t order_id, uint32_t slot)
        {
            if ((size_ + 1) * 10 >= TABLE_CAPACITY * 7)
            {
                LOG_ERROR(app_log::logger(), "OrderIdIndex load factor too high, size:{}, capacity:{}", size_, TABLE_CAPACITY);
                return false;
            }

            size_t idx = hash(order_id) & mask;
            size_t first_tombstone = TABLE_CAPACITY;

            for (;;)
            {
                Entry& e = table[idx];

                if (e.state == 0)
                {
                    size_t target = (first_tombstone != TABLE_CAPACITY) ? first_tombstone : idx;
                    table[target].order_id = order_id;
                    table[target].slot = slot;
                    table[target].state = 1;
                    ++size_;
                    return true;
                }

                if (e.state == 2)
                {
                    if (first_tombstone == TABLE_CAPACITY)
                    {
                        first_tombstone = idx;
                    }
                }
                else if (e.order_id == order_id)
                {
                    return false;
                }

                idx = (idx + 1) & mask;
            }
        }

        bool find(int64_t order_id, uint32_t& slot) const
        {
            size_t idx = hash(order_id) & mask;

            for (;;)
            {
                const Entry& e = table[idx];

                if (e.state == 0)
                {
                    return false;
                }

                if (e.state == 1 && e.order_id == order_id)
                {
                    slot = e.slot;
                    return true;
                }

                idx = (idx + 1) & mask;
            }
        }

        bool erase(int64_t order_id)
        {
            size_t idx = hash(order_id) & mask;

            for (;;)
            {
                Entry& e = table[idx];

                if (e.state == 0)
                {
                    return false;
                }

                if (e.state == 1 && e.order_id == order_id)
                {
                    e.state = 2;
                    e.slot = INVALID_SLOT;
                    --size_;
                    return true;
                }

                idx = (idx + 1) & mask;
            }
        }

        void clear()
        {
            for (size_t i = 0; i < TABLE_CAPACITY; ++i)
            {
                table[i] = Entry{};
            }
            size_ = 0;
            mask = TABLE_CAPACITY - 1;
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

        Entry table[TABLE_CAPACITY];
        size_t mask = TABLE_CAPACITY - 1;
        size_t size_ = 0;
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
        static constexpr size_t MAX_ORDER = 16'000'000;
        static constexpr uint32_t INVALID_SLOT = OrderNode::INVALID_SLOT;

        /**
         * @brief 构造函数，初始化内存池
         * @note 预分配MAX_ORDER个节点，通过next指针串成空闲链表
         */
        OrderPool()
        {
            for (uint32_t i = 0; i < MAX_ORDER; ++i)
            {
                nodes[i].self_slot = i;
                nodes[i].reset_runtime_fields();
                free_stack[i] = static_cast<uint32_t>(MAX_ORDER - 1 - i);
            }
            free_top = static_cast<uint32_t>(MAX_ORDER);
            id_index.init();
        }

        /**
         * @brief 从内存池分配一个节点
         * @return 节点指针，池耗尽返回nullptr
         */
        OrderNode* alloc(int64_t order_id)
        {
            if (free_top == 0) [[unlikely]]
            {
                LOG_ERROR(app_log::logger(), "OrderPool: out of max size:{}", MAX_ORDER);
                return nullptr;
            }

            uint32_t exist_slot = INVALID_SLOT;
            if (id_index.find(order_id, exist_slot)) [[unlikely]]
            {
                LOG_ERROR(app_log::logger(), "OrderPool: duplicated order id:{}", order_id);
                return nullptr;
            }

            const uint32_t slot = free_stack[--free_top];
            OrderNode& node = nodes[slot];
            node.reset_runtime_fields();
            node.is_use = true;
            node.id = order_id;

            if (!id_index.insert(order_id, slot)) [[unlikely]]
            {
                free_stack[free_top++] = slot;
                node.reset_runtime_fields();
                LOG_ERROR(app_log::logger(), "OrderPool: failed to insert id index, id:{}", order_id);
                return nullptr;
            }

            ++count;
            if (count > max_count)
            {
                max_count = count;
            }

            return &node;
        }

        OrderNode* find(int64_t order_id)
        {
            uint32_t slot = INVALID_SLOT;
            if (!id_index.find(order_id, slot))
            {
                return nullptr;
            }
            return &nodes[slot];
        }

        const OrderNode* find(int64_t order_id) const
        {
            uint32_t slot = INVALID_SLOT;
            if (!id_index.find(order_id, slot))
            {
                return nullptr;
            }
            return &nodes[slot];
        }

        /**
         * @brief 释放节点回内存池
         * @param p 要释放的节点指针
         * @note 重置节点字段并归还到空闲链表头部
         */
        void free(OrderNode* p, PriceLevel* level_pool)
        {
            if (!p) return;
            if (!p->is_use) return;

            if (p->price_level_slot != INVALID_SLOT)
            {
                unlink(level_pool, p);
            }

            id_index.erase(p->id);
            const uint32_t slot = p->self_slot;
            p->reset_runtime_fields();
            p->self_slot = slot;
            free_stack[free_top++] = slot;

            if (count > 0)
            {
                --count;
            }
        }

        /**
         * @brief 将订单节点追加到价格档位的尾部
         * @param level 目标价格档位指针
         * @param node 要追加的订单节点指针
         * @note 新订单正常情况下追加到尾部，遵循时间优先原则
         */
        inline void link(PriceLevel* level_pool, uint32_t level_slot, OrderNode* node)
        {
            assert(level_pool != nullptr);
            assert(node != nullptr);
            assert(node->is_use);
            assert(level_slot != PriceLevel::INVALID_SLOT);

            PriceLevel& level = level_pool[level_slot];
            assert(level.is_use);
            assert(node->price_level_slot == INVALID_SLOT);

            const uint32_t slot = node->self_slot;
            node->prev_slot = level.tail_slot;
            node->next_slot = INVALID_SLOT;

            if (level.tail_slot == INVALID_SLOT)
            {
                level.head_slot = slot;
                level.tail_slot = slot;
            }
            else
            {
                nodes[level.tail_slot].next_slot = slot;
                level.tail_slot = slot;
            }

            level.total_volume += node->volume;
            ++level.order_size;
            node->price_level_slot = level_slot;
        }

        /**
         * @brief 从价格档位中移除订单节点
         * @param node 要移除的订单节点指针
         * @note 维护双向链表的连接关系，更新价格档位的总量和订单数
         */
        inline void unlink(PriceLevel* level_pool, OrderNode* node)
        {
            assert(level_pool != nullptr);
            assert(node != nullptr);

            if (node->price_level_slot == INVALID_SLOT)
            {
                return;
            }

            PriceLevel& level = level_pool[node->price_level_slot];
            const uint32_t prev = node->prev_slot;
            const uint32_t next = node->next_slot;

            // 将当前OrderNode的next指针赋值给 前一个OrderNode的next指针
            if (prev != INVALID_SLOT)
            {
                nodes[prev].next_slot = next;
            }

            // 将当前OrderNode的prev指针赋值给 下一个OrderNode的prev指针
            if (next != INVALID_SLOT)
            {
                nodes[next].prev_slot = prev;
            }

            // 如果当前OrderNode位于队列的头，则将下一个OrderNode设置为队列头
            if (level.head_slot == node->self_slot)
            {
                level.head_slot = next;
            }

            // 如果当前OrderNode位于队列的尾，则将前一个OrderNode设置为队列尾
            if (level.tail_slot == node->self_slot)
            {
                level.tail_slot = prev;
            }

            // 修改总volume
            level.total_volume -= node->volume;
            --level.order_size;

            node->prev_slot = INVALID_SLOT;
            node->next_slot = INVALID_SLOT;
            node->price_level_slot = INVALID_SLOT;
        }

        void reset()
        {
            for (uint32_t i = 0; i < MAX_ORDER; ++i)
            {
                nodes[i].self_slot = i;
                nodes[i].reset_runtime_fields();
                free_stack[i] = static_cast<uint32_t>(MAX_ORDER - 1 - i);
            }
            free_top = static_cast<uint32_t>(MAX_ORDER);
            id_index.clear();
            count = 0;
            max_count = 0;
        }

        /** @brief 获取当前已分配节点数 */
        size_t getCount() const
        {
            return count;
        }

        /** @brief 获取历史最大已分配节点数 */
        size_t getMaxCount() const
        {
            return max_count;
        }

        OrderNode* getBySlot(uint32_t slot)
        {
            return (slot < MAX_ORDER) ? &nodes[slot] : nullptr;
        }

        const OrderNode* getBySlot(uint32_t slot) const
        {
            return (slot < MAX_ORDER) ? &nodes[slot] : nullptr;
        }

    private:
        OrderNode nodes[MAX_ORDER];
        uint32_t free_stack[MAX_ORDER];
        uint32_t free_top = 0;
        size_t count = 0;
        size_t max_count = 0;
        OrderIdIndex id_index;
    };

    /**
     * @brief 价格档位内存池
     * @note 预分配x个价格档位节点，使用slot管理空闲节点
     */
    class PriceLevelPool
    {
    public:
        static constexpr size_t MAX_LEVEL = 50'000'000;

        PriceLevelPool()
        {
            for (uint32_t i = 0; i < MAX_LEVEL; ++i)
            {
                levels[i].self_slot = i;
                levels[i].reset();
                free_stack[i] = static_cast<uint32_t>(MAX_LEVEL - 1 - i);
            }
            free_top = static_cast<uint32_t>(MAX_LEVEL);
        }

        PriceLevel* alloc(int64_t price)
        {
            if (free_top == 0)
            {
                LOG_ERROR(app_log::logger(), "PriceLevelPool: out of max size:{}", MAX_LEVEL);
                return nullptr;
            }

            const uint32_t slot = free_stack[--free_top];
            PriceLevel& level = levels[slot];
            level.reset();
            level.is_use = true;
            level.self_slot = slot;
            level.price = price;
            return &level;
        }

        void free(PriceLevel* p)
        {
            if (!p) return;
            if (!p->is_use) return;
            if (!p->empty()) return;

            const uint32_t slot = p->self_slot;
            p->reset();
            p->self_slot = slot;
            free_stack[free_top++] = slot;
        }

        PriceLevel* data()
        {
            return levels;
        }

        const PriceLevel* data() const
        {
            return levels;
        }

        PriceLevel* getBySlot(uint32_t slot)
        {
            return (slot < MAX_LEVEL) ? &levels[slot] : nullptr;
        }

        const PriceLevel* getBySlot(uint32_t slot) const
        {
            return (slot < MAX_LEVEL) ? &levels[slot] : nullptr;
        }

        void reset()
        {
            for (uint32_t i = 0; i < MAX_LEVEL; ++i)
            {
                levels[i].self_slot = i;
                levels[i].reset();
                free_stack[i] = static_cast<uint32_t>(MAX_LEVEL - 1 - i);
            }
            free_top = static_cast<uint32_t>(MAX_LEVEL);
        }

    private:
        PriceLevel levels[MAX_LEVEL];
        uint32_t free_stack[MAX_LEVEL];
        uint32_t free_top = 0;
    };
}