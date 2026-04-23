#ifndef ORDERBOOK_WORKERS_H
#define ORDERBOOK_WORKERS_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sstream>
#include "common/MarketDataStruct.h"
#include "common/MDTools.h"
#include "NodePool.h"
#include "LimitOrderBook.h"
#include "LimitOrderBookLite.h"
#include "MatchingEngine.h"
#include "util/logger.h"
#include "util/StdException.h"
#include "ringbuffer/RingBuffer.h"

// tryPush+丢弃/统计模型:1  阻塞式ringBuffer.push:0
#ifndef ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP
#define ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP 0
#endif

// 盘口初始化静态信息
struct LOBBaseOpenBeforeData
{
    std::string code;
    int64_t per_close = 0;
    int64_t limit_up_price = 0;
    int64_t limit_down_price = 0;
};

struct WorkerConfig
{
    int workerId = -1;
    int cpuId = -1;
    std::size_t ringBufferCapacity = 40960;
};

using SecurityKey = uint64_t;

constexpr SecurityKey kInvalidSecurityKey = 0;

static inline SecurityKey encode_security_key(char exchange_id,
                                       const char* security_id) noexcept
{
    if (!security_id || security_id[0] == '\0') return kInvalidSecurityKey;

    uint64_t code = 0;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(security_id);
         *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') return kInvalidSecurityKey;
        code = code * 10u + static_cast<uint64_t>(*p - '0');
        if (code > 0xffffffffULL) return kInvalidSecurityKey;
    }

    return (static_cast<uint64_t>(static_cast<uint8_t>(exchange_id)) << 32) |
           static_cast<uint32_t>(code);
}

static inline uint64_t mix_security_key(SecurityKey key) noexcept
{
    uint64_t z = key + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static inline std::size_t worker_index_for_security_key(SecurityKey key,
                                                        std::size_t worker_count) noexcept
{
    if (worker_count == 0) return 0;
    return static_cast<std::size_t>(mix_security_key(key) % worker_count);
}

static inline char exchangeIdFromChannelNo(int32_t channelNo) noexcept
{
    if (channelNo < 10)
    {
        return '1';
    }
    if (channelNo > 2000)
    {
        return '2';
    }
    return '\0';
}

static inline SecurityKey makeSecurityKey(int32_t channelNo, const char* securityCode) noexcept
{
    return encode_security_key(exchangeIdFromChannelNo(channelNo), securityCode);
}

inline WorkerConfig makeWorkerConfig(
    int workerId,
    int cpuId,
    std::size_t ringBufferCapacity = 40960)
{
    WorkerConfig config;
    config.workerId = workerId;
    config.cpuId = cpuId;
    config.ringBufferCapacity = ringBufferCapacity;
    return config;
}

static constexpr int64_t kScopedSeqBase = 10'000'000'000'000LL;
// 用于处理同一个pool里不同channel相同seq冲突的问题
// channelNo不能大于9999，seq不能大于9'000'000'000'000否则有溢出风险
static inline int64_t makeScopedSeq(int32_t channelNo, int64_t rawSeq) noexcept
{
    return kScopedSeqBase * static_cast<int64_t>(channelNo) + rawSeq;
}

static inline int64_t restoreScopedSeq(int32_t channelNo, int64_t scopedSeq) noexcept
{
    if (scopedSeq <= 0)
    {
        return scopedSeq;
    }
    return scopedSeq - kScopedSeqBase * static_cast<int64_t>(channelNo);
}


template<typename T>
class Workers
{
public:
    struct SingleWorker;
    using WorkerContext = SingleWorker;
    using OrderCallback = std::function<void(WorkerContext *, const marketdata::MDOrder *)>;
    using TradeCallback = std::function<void(WorkerContext *, const marketdata::MDTrade *)>;
    using MatchCallback = std::function<void(WorkerContext *, const std::string &, const marketdata::MatchRecord *)>;

    struct WorkerStats
    {
        int workerId = -1;
        int cpuId = -1;
        uint64_t enqueued = 0;
        uint64_t dropped = 0;
        uint64_t processed = 0;
        std::size_t pending = 0;
        std::size_t queueCapacity = 0;
    };

private:
    enum class MessageType : uint8_t
    {
        NONE = 0,
        ORDER = 1,
        TRADE = 2
    };

private:
    struct WorkerMessage
    {
        union Payload
        {
            marketdata::MDOrder order;
            marketdata::MDTrade trade;

            Payload()
            {
            }

            ~Payload()
            {
            }
        };

        MessageType type = MessageType::NONE;
        Payload payload;

        WorkerMessage()
            : type(MessageType::NONE), payload()
        {
        }

        WorkerMessage(const WorkerMessage &other)
            : type(MessageType::NONE), payload()
        {
            copyFrom(other);
        }

        WorkerMessage &operator=(const WorkerMessage &other)
        {
            if (this != &other)
            {
                copyFrom(other);
            }
            return *this;
        }

        WorkerMessage(WorkerMessage &&other) noexcept
            : type(MessageType::NONE), payload()
        {
            copyFrom(other);
        }

        WorkerMessage &operator=(WorkerMessage &&other) noexcept
        {
            if (this != &other)
            {
                copyFrom(other);
            }
            return *this;
        }

        static WorkerMessage fromOrder(const marketdata::MDOrder &order)
        {
            WorkerMessage message;
            message.type = MessageType::ORDER;
            message.payload.order = order;
            return message;
        }

        static WorkerMessage fromTrade(const marketdata::MDTrade &trade)
        {
            WorkerMessage message;
            message.type = MessageType::TRADE;
            message.payload.trade = trade;
            return message;
        }

    private:
        inline void copyFrom(const WorkerMessage &other)
        {
            type = other.type;
            if (type == MessageType::ORDER)
            {
                payload.order = other.payload.order;
            }
            else if (type == MessageType::TRADE)
            {
                payload.trade = other.payload.trade;
            }
        }
    };

public:
    struct SingleWorker
    {
        using InnerMatchCallback = std::function<void(const std::string &, const marketdata::MatchRecord &)>;

    private:
        struct OrderbookHolder
        {
            int32_t channelNo = 0;
            std::string code;
            std::unique_ptr<T> orderbook;
        };

        inline void initPool()
        {
            if constexpr (std::is_same_v<T, marketdata::LimitOrderBook>
                || std::is_same_v<T, marketdata::MatchingEngine>
                || std::is_same_v<T, marketdata::LimitOrderBookLite>)
            {
                // 每个 worker 内部的所有订单簿统一复用共享价格档位池；需要订单节点的实现再额外持有共享订单池。
                m_price_level_pool = std::make_shared<marketdata::PriceLevelPool>();
                if constexpr (std::is_same_v<T, marketdata::LimitOrderBook>
                    || std::is_same_v<T, marketdata::MatchingEngine>)
                {
                    m_order_pool = std::make_shared<marketdata::OrderPool>();
                }
            }
        }

        inline std::unique_ptr<T> createOrderbook(int32_t channelNo, const std::string &code)
        {
            (void)channelNo;

            if (!m_open_before_map)
            {
                return nullptr;
            }

            auto iter = m_open_before_map->find(code);
            if (iter == m_open_before_map->end())
            {
                return nullptr;
            }

            if constexpr (std::is_same_v<T, marketdata::LimitOrderBook>
                || std::is_same_v<T, marketdata::MatchingEngine>)
            {
                if (!m_order_pool || !m_price_level_pool)
                {
                    LOG_ERROR(app_log::logger(), "create code:{} OrderBook error, object pool is nullptr", code);
                    STDTHROW(STD_ERROR_CODE, "create code:"<<code<<" OrderBook error, object pool is nullptr", "create code:"<<code<<" OrderBook error, object pool is nullptr");
                }
                return std::make_unique<T>(
                    m_date,
                    code,
                    m_order_pool,
                    m_price_level_pool,
                    iter->second.per_close,
                    iter->second.limit_down_price,
                    iter->second.limit_up_price);
            }
            else if constexpr (std::is_same_v<T, marketdata::LimitOrderBookLite>)
            {
                if (!m_price_level_pool)
                {
                    LOG_ERROR(app_log::logger(), "create code:{} OrderBookLite error, price level pool is nullptr", code);
                    STDTHROW(STD_ERROR_CODE, "create code:"<<code<<" OrderBookLite error, price level pool is nullptr", "create code:"<<code<<" OrderBookLite error, price level pool is nullptr");
                }
                return std::make_unique<T>(
                    m_date,
                    code,
                    m_price_level_pool,
                    iter->second.per_close,
                    iter->second.limit_down_price,
                    iter->second.limit_up_price);
            }
            else
            {
                return std::make_unique<T>(m_date, code, iter->second.per_close, iter->second.limit_down_price, iter->second.limit_up_price);
            }
        }

        // worker 内部真实主键改成 channel+code，避免同代码跨市场/跨通道时互相覆盖。
        inline T *getOrCreateOrderbook(SecurityKey key, int32_t channelNo, const char* security_code)
        {
            auto iter = m_orderbooks.find(key);
            if (iter == m_orderbooks.end())
            {
                const std::string code = normalizeSecurityCode(security_code);
                OrderbookHolder holder;
                holder.channelNo = channelNo;
                holder.code = code;
                holder.orderbook = createOrderbook(channelNo, code);
                auto [it, _] = m_orderbooks.emplace(key, std::move(holder));
                iter = it;
                bindMatchCallback(iter->second.channelNo, iter->second.code, iter->second.orderbook.get());
            }
            return iter->second.orderbook.get();
        }

        inline void bindMatchCallback(int32_t channelNo, const std::string &code, T *orderbook)
        {
            if constexpr (std::is_same_v<T, marketdata::MatchingEngine>)
            {
                if (orderbook == nullptr)
                {
                    return;
                }

                orderbook->setMatchCallback([this, channelNo, code](const marketdata::MatchRecord &record)
                {
                    if (m_matchCallback)
                    {
                        marketdata::MatchRecord callbackRecord = record;
                        callbackRecord.channel_no = channelNo;
                        callbackRecord.bid_order_id = restoreScopedSeq(channelNo, callbackRecord.bid_order_id);
                        callbackRecord.offer_order_id = restoreScopedSeq(channelNo, callbackRecord.offer_order_id);
                        m_matchCallback(code, callbackRecord);
                    }
                });
            }
            else
            {
                (void)channelNo;
                (void)code;
                (void)orderbook;
            }
        }

    public:
        int workerId = -1;
        int cpuId = -1;
        SpscRingBufferZC<WorkerMessage> ringBuffer;
        std::thread thread;
        std::atomic<uint64_t> enqueued{0};
        std::atomic<uint64_t> dropped{0};
        std::atomic<uint64_t> processed{0};

        explicit SingleWorker(
            const std::string &date,
            std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> open_before_data)
            : SingleWorker(date, std::move(open_before_data), WorkerConfig{})
        {
        }

        SingleWorker(
            const std::string &date,
            std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> open_before_data,
            const WorkerConfig &config)
            : workerId(config.workerId),
              cpuId(config.cpuId),
              ringBuffer(std::max<std::size_t>(static_cast<std::size_t>(2), config.ringBufferCapacity)),
              m_date(date),
              m_open_before_map(std::move(open_before_data))
        {
            m_orderbooks.reserve(1000);
            initPool();
        }

        T *getOrderbookPtr(int32_t channelNo, const char *securityCode)
        {
            if (securityCode == nullptr)
            {
                return nullptr;
            }

            const SecurityKey key = makeSecurityKey(channelNo, securityCode);
            auto iter = m_orderbooks.find(key);
            return iter == m_orderbooks.end() ? nullptr : iter->second.orderbook.get();
        }

        const T *getOrderbookPtr(int32_t channelNo, const char *securityCode) const
        {
            if (securityCode == nullptr)
            {
                return nullptr;
            }

            const SecurityKey key = makeSecurityKey(channelNo, securityCode);
            auto iter = m_orderbooks.find(key);
            return iter == m_orderbooks.end() ? nullptr : iter->second.orderbook.get();
        }

        void setMatchCallback(InnerMatchCallback callback)
        {
            if constexpr (std::is_same_v<T, marketdata::MatchingEngine>)
            {
                m_matchCallback = std::move(callback);
                for (auto &[_, holder] : m_orderbooks)
                {
                    bindMatchCallback(holder.channelNo, holder.code, holder.orderbook.get());
                }
            }
            else
            {
                (void)callback;
            }
        }

        bool onOrderDetail(const marketdata::MDOrder *order)
        {
            if (order == nullptr || order->volume <= 0)
            {
                return false;
            }

            if (order->security_code == nullptr)
            {
                return false;
            }

            const SecurityKey key = makeSecurityKey(order->channel_no, order->security_code);
            T *lob = getOrCreateOrderbook(key, order->channel_no, order->security_code);
            if (lob == nullptr)
            {
                return false;
            }
            lob->processOrder(order);
            return true;
        }

        bool onTransaction(const marketdata::MDTrade *trade)
        {
            if (trade == nullptr || trade->volume <= 0)
            {
                return false;
            }

            if (trade->security_code == nullptr)
            {
                return false;
            }

            const SecurityKey key = makeSecurityKey(trade->channel_no, trade->security_code);
            T *lob = getOrCreateOrderbook(key, trade->channel_no, trade->security_code);
            if (lob == nullptr)
            {
                return false;
            }
            lob->processTrade(trade);
            return true;
        }

        std::size_t size() const
        {
            return m_orderbooks.size();
        }

        std::size_t poolMaxSize(bool isLob)
        {
            if (isLob && m_order_pool)
            {
                return m_order_pool->getMaxCount();
            }
            return 0;
        }

    private:
        std::string m_date;
        std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> m_open_before_map;
        std::shared_ptr<marketdata::OrderPool> m_order_pool;
        std::shared_ptr<marketdata::PriceLevelPool> m_price_level_pool;
        // 同一个 worker 内允许出现同 code 的多个 orderbook，因此 map key 必须带 channel 信息。
        std::unordered_map<SecurityKey, OrderbookHolder> m_orderbooks;
        InnerMatchCallback m_matchCallback;
    };

    Workers(
        const std::string &date,
        const std::string &cpuIds,
        std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> open_before_data)
        : m_date(date), m_open_before_data(open_before_data)
    {
        if (m_date < "20210607")
        {
            m_skip_sse = true;
        }

        initWorkers(cpuIds);
    }

    ~Workers()
    {
        stop();
    }

    Workers(const Workers &) = delete;
    Workers &operator=(const Workers &) = delete;
    Workers(Workers &&) = delete;
    Workers &operator=(Workers &&) = delete;

    void start()
    {
        if (m_running.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        clearWorkerException();

        try
        {
            for (auto &workerPtr : m_workers)
            {
                SingleWorker &worker = *workerPtr;
                worker.thread = std::thread([this, &worker]()
                {
                    try
                    {
                        workerLoop(worker);
                    }
                    catch (...)
                    {
                        setWorkerException(std::current_exception());
                        m_running.store(false, std::memory_order_release);
                    }
                });

                bindThreadToCpu(worker.thread, worker.cpuId);
            }
        }
        catch (...)
        {
            m_running.store(false, std::memory_order_release);
            joinWorkers();
            throw;
        }
    }

    void stop() noexcept
    {
        m_running.store(false, std::memory_order_release);
        joinWorkers();
    }

    bool isRunning() const
    {
        return m_running.load(std::memory_order_acquire);
    }

    std::size_t size() const
    {
        return m_workers.size();
    }

    bool hasWorker(int workerId) const
    {
        return findWorkerById(workerId) != nullptr;
    }

    // 注意：callback 内捕获的对象生命周期必须长于 stop() 返回（stop() 内部会 join 所有 worker
    // 线程，join 返回后 worker 线程已完全退出，callback 才可以安全失效）。
    void setOrderCallback(OrderCallback callback)
    {
        if (isRunning())
        {
            throw std::logic_error("cannot set order callback while workers are running");
        }
        m_orderCallback = std::move(callback);
    }

    void setTradeCallback(TradeCallback callback)
    {
        if (isRunning())
        {
            throw std::logic_error("cannot set trade callback while workers are running");
        }
        m_tradeCallback = std::move(callback);
    }

    void setMatchCallback(MatchCallback callback)
    {
        if (isRunning())
        {
            throw std::logic_error("cannot set match callback while workers are running");
        }

        m_matchCallback = std::move(callback);
        refreshMatchCallbacks();
    }

    bool onOrderDetail(const marketdata::MDOrder *order)
    {
        return submitOrder(order);
    }

    bool onTransaction(const marketdata::MDTrade *trade)
    {
        return submitTrade(trade);
    }

    bool submitOrder(const marketdata::MDOrder *order)
    {
        if (order == nullptr)
        {
            return false;
        }
        return submitOrder(*order);
    }

    bool submitTrade(const marketdata::MDTrade *trade)
    {
        if (trade == nullptr)
        {
            return false;
        }
        return submitTrade(*trade);
    }

    bool submitOrder(const marketdata::MDOrder &order)
    {
        if (m_skip_sse && order.security_code[0] == '6') return false;
        return submitMessage(makeSecurityKey(order.channel_no, order.security_code), WorkerMessage::fromOrder(order));
    }

    bool submitTrade(const marketdata::MDTrade &trade)
    {
        if (m_skip_sse && trade.security_code[0] == '6') return false;
        return submitMessage(makeSecurityKey(trade.channel_no, trade.security_code), WorkerMessage::fromTrade(trade));
    }

    WorkerContext *getLobGroup(int workerId)
    {
        SingleWorker *worker = findWorkerById(workerId);
        return worker;
    }

    const WorkerContext *getLobGroup(int workerId) const
    {
        const SingleWorker *worker = findWorkerById(workerId);
        return worker;
    }

    WorkerContext *getLobGroupBySecurity(int32_t channelNo, const char* securityCode)
    {
        SingleWorker *worker = findWorker(makeSecurityKey(channelNo, securityCode));
        return worker;
    }

    const WorkerContext *getLobGroupBySecurity(int32_t channelNo, const char* securityCode) const
    {
        const SingleWorker *worker = findWorker(makeSecurityKey(channelNo, securityCode));
        return worker;
    }

    WorkerStats getWorkerStats(int workerId) const
    {
        const SingleWorker *worker = findWorkerById(workerId);
        return worker == nullptr ? WorkerStats{} : makeWorkerStats(*worker);
    }

    std::vector<WorkerStats> getAllWorkerStats() const
    {
        std::vector<WorkerStats> stats;
        stats.reserve(m_workers.size());
        for (const auto &workerPtr : m_workers)
        {
            stats.push_back(makeWorkerStats(*workerPtr));
        }
        return stats;
    }

    void rethrowIfWorkerFailed() const
    {
        if (!m_hasWorkerException.load(std::memory_order_acquire))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_exceptionMutex);
        if (m_workerException)
        {
            std::rethrow_exception(m_workerException);
        }
    }

private:
    inline WorkerStats makeWorkerStats(const SingleWorker &worker) const
    {
        WorkerStats stats;
        stats.workerId = worker.workerId;
        stats.cpuId = worker.cpuId;
        stats.enqueued = worker.enqueued.load(std::memory_order_acquire);
        stats.dropped = worker.dropped.load(std::memory_order_acquire);
        stats.processed = worker.processed.load(std::memory_order_acquire);
        stats.pending = worker.ringBuffer.size();
        stats.queueCapacity = worker.ringBuffer.capacity();
        return stats;
    }

    inline void initWorkers(const std::string& cpuIds)
    {
        if (cpuIds.empty())
        {
            throw std::invalid_argument("workers config must not be empty");
        }

        // "3,4,5,6,7" → [3,4,5,6,7]，分割出来的数量即为 worker 数量
        std::vector<int> worker_cores;
        {
            std::istringstream ss(cpuIds);
            std::string token;
            while (std::getline(ss, token, ',')) {
                auto b = token.find_first_not_of(" \t");
                if (b != std::string::npos) worker_cores.push_back(std::stoi(token.substr(b)));
            }
        }
        int num_workers = (int)worker_cores.size();
        m_workers.reserve(num_workers);
        for (int i = 0; i < num_workers; ++i)
        {
            WorkerConfig worker_config = makeWorkerConfig(i, worker_cores[i]);
            m_workers.emplace_back(std::make_unique<SingleWorker>(m_date, m_open_before_data, worker_config));
            const SingleWorker &worker = *m_workers.back();
            LOG_INFO(app_log::logger(), "worker init, workerId:{}, cpu id:{}, ringbuffer size:{}", worker.workerId, worker.cpuId, worker.ringBuffer.capacity());
        }
        m_workers_size = m_workers.size();
        LOG_INFO(app_log::logger(), "workers init finish");
    }

    inline bool submitMessage(SecurityKey key, WorkerMessage message)
    {
        if (!m_running.load(std::memory_order_acquire))
        {
            return false;
        }

        SingleWorker *worker = findWorker(key);
        if (worker == nullptr)
        {
            return false;
        }

#if ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP
        typename SpscRingBufferZC<WorkerMessage>::WriteReservation reservation;
        if (!worker->ringBuffer.tryAcquireWrite(reservation))
        {
            worker->dropped.fetch_add(1, std::memory_order_release);
            return false;
        }
        *reservation.data = std::move(message);
        worker->ringBuffer.commitWrite(reservation);
#else
        typename SpscRingBufferZC<WorkerMessage>::WriteReservation reservation;
        WorkerMessage *slot = worker->ringBuffer.acquireWrite(reservation);
        *slot = std::move(message);
        worker->ringBuffer.commitWrite(reservation);
#endif

        worker->enqueued.fetch_add(1, std::memory_order_release);
        return true;
    }

    inline SingleWorker *findWorker(SecurityKey key)
    {
        if (key == kInvalidSecurityKey || m_workers.empty())
        {
            return nullptr;
        }
        // 单 worker 时把所有盘口都交给当前 worker；多 worker 时再按 channel+code 做稳定分片。
        if (m_workers.size() == 1)
        {
            return m_workers.front().get();
        }
        return m_workers[worker_index_for_security_key(key, m_workers.size())].get();
    }

    inline const SingleWorker *findWorker(SecurityKey key) const
    {
        if (key == kInvalidSecurityKey || m_workers.empty())
        {
            return nullptr;
        }
        // const 版本保持同样的路由规则，保证查询和写入命中同一个 worker。
        if (m_workers.size() == 1)
        {
            return m_workers.front().get();
        }
        return m_workers[worker_index_for_security_key(key, m_workers.size())].get();
    }

    inline SingleWorker *findWorkerById(int workerId)
    {
        if (workerId < 0 || static_cast<std::size_t>(workerId) >= m_workers_size) return nullptr;
        return m_workers[workerId].get();
    }

    inline const SingleWorker *findWorkerById(int workerId) const
    {
        if (workerId < 0 || static_cast<std::size_t>(workerId) >= m_workers_size) return nullptr;
        return m_workers[workerId].get();
    }

    inline static void scopeOrderIds(marketdata::MDOrder &order) noexcept
    {
        if (order.appl_seq_num > 0)
        {
            order.appl_seq_num = makeScopedSeq(order.channel_no, order.appl_seq_num);
        }
    }

    inline static void scopeTradeIds(marketdata::MDTrade &trade) noexcept
    {
        if (trade.appl_seq_num > 0)
        {
            trade.appl_seq_num = makeScopedSeq(trade.channel_no, trade.appl_seq_num);
        }
        if (trade.bid_appl_seq_num > 0)
        {
            trade.bid_appl_seq_num = makeScopedSeq(trade.channel_no, trade.bid_appl_seq_num);
        }
        if (trade.offer_appl_seq_num > 0)
        {
            trade.offer_appl_seq_num = makeScopedSeq(trade.channel_no, trade.offer_appl_seq_num);
        }
    }

    inline void refreshMatchCallbacks()
    {
        if constexpr (std::is_same_v<T, marketdata::MatchingEngine>)
        {
            for (auto &workerPtr : m_workers)
            {
                SingleWorker &worker = *workerPtr;
                worker.setMatchCallback([this, &worker](const std::string &code, const marketdata::MatchRecord &record)
                {
                    if (m_matchCallback)
                    {
                        m_matchCallback(&worker, code, &record);
                    }
                });
            }
        }
    }

    inline void workerLoop(SingleWorker &worker)
    {
        while (m_running.load(std::memory_order_acquire) || !worker.ringBuffer.empty())
        {
            typename SpscRingBufferZC<WorkerMessage>::ReadReservation reservation;
            if (!worker.ringBuffer.tryAcquireRead(reservation))
            {
                ringbuffer_detail::cpuRelax();
                continue;
            }

            const WorkerMessage *message = reservation.data;
            if (message->type == MessageType::ORDER)
            {
                marketdata::MDOrder scopedOrder = message->payload.order;
                scopeOrderIds(scopedOrder);
                worker.onOrderDetail(&scopedOrder);
                if (m_orderCallback)
                {
                    m_orderCallback(&worker, &message->payload.order);
                }
            }
            else if (message->type == MessageType::TRADE)
            {
                marketdata::MDTrade scopedTrade = message->payload.trade;
                scopeTradeIds(scopedTrade);
                worker.onTransaction(&scopedTrade);
                if (m_tradeCallback)
                {
                    m_tradeCallback(&worker, &message->payload.trade);
                }
            }

            worker.processed.fetch_add(1, std::memory_order_release);
            worker.ringBuffer.commitRead(reservation);
        }
    }

    inline void joinWorkers() noexcept
    {
        for (auto &workerPtr : m_workers)
        {
            if (workerPtr->thread.joinable())
            {
                workerPtr->thread.join();
            }
        }
    }

    inline void clearWorkerException()
    {
        std::lock_guard<std::mutex> lock(m_exceptionMutex);
        m_workerException = nullptr;
        m_hasWorkerException.store(false, std::memory_order_release);
    }

    inline void setWorkerException(std::exception_ptr exceptionPtr)
    {
        std::lock_guard<std::mutex> lock(m_exceptionMutex);
        if (!m_workerException)
        {
            m_workerException = std::move(exceptionPtr);
        }
        m_hasWorkerException.store(true, std::memory_order_release);
    }

    inline void bindThreadToCpu(std::thread& th, int cpu_id)
    {
        if (cpu_id < 0)
        {
            return;
        }

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);

        int rc = pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset);
        if (rc != 0)
        {
            LOG_ERROR(app_log::logger(), "pthread_setaffinity_np failed, cpu_id:{}, rc:{}", cpu_id, rc);
        }
        else
        {
            LOG_INFO(app_log::logger(), "thread bind cpu id:{}", cpu_id);
        }
    }

private:
    std::string m_date;
    bool m_skip_sse = false;
    std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> m_open_before_data;
    std::vector<std::unique_ptr<SingleWorker>> m_workers;
    size_t m_workers_size = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_hasWorkerException{false};
    mutable std::mutex m_exceptionMutex;
    std::exception_ptr m_workerException = nullptr;
    OrderCallback m_orderCallback;
    TradeCallback m_tradeCallback;
    MatchCallback m_matchCallback;
};

using WorkersLimitOrderBook = Workers<marketdata::LimitOrderBook>;
using WorkersLimitOrderBookLite = Workers<marketdata::LimitOrderBookLite>;
using WorkersMatchingEngine = Workers<marketdata::MatchingEngine>;
template<typename LobType>
using LOBGroup = typename Workers<LobType>::WorkerContext;
using LOBGroupLimitOrderBook = LOBGroup<marketdata::LimitOrderBook>;
using LOBGroupLimitOrderBookLite = LOBGroup<marketdata::LimitOrderBookLite>;
using LOBGroupMatchingEngine = LOBGroup<marketdata::MatchingEngine>;

#endif
