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
#include "MatchingEngine.h"
#include "util/logger.h"
#include "util/StdException.h"
#include "util/ringbuffer/RingBuffer.h"

namespace marketdata
{
namespace orderbook
{

    // tryPush+丢弃/统计模型:1  阻塞式ringBuffer.push:0
    #ifndef ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP
    #define ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP 0
    #endif

    // 盘口初始化静态信息
    struct LOBBaseOpenBeforeData
    {
        std::string code;
        int64_t preClose = 0;
        int64_t limitUpPrice = 0;
        int64_t limitDownPrice = 0;
    };

    struct WorkerConfig
    {
        int workerId = -1;
        int cpuId = -1;
        std::size_t ringBufferCapacity = 65536;
        bool cpuBind = false;
    };

    enum class MessageType : uint8_t
    {
        NONE = 0,
        ORDER = 1,
        TRADE = 2
    };

    using SecurityKey = uint64_t;

    constexpr SecurityKey kInvalidSecurityKey = 0;

    static inline SecurityKey encode_security_key(
        char exchangeId,
        const char* securityId) noexcept
    {
        if (!securityId || securityId[0] == '\0') return kInvalidSecurityKey;

        uint64_t code = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(securityId); *p != '\0'; ++p)
        {
            if (*p < '0' || *p > '9') return kInvalidSecurityKey;
            code = code * 10u + static_cast<uint64_t>(*p - '0');
            if (code > 0xffffffffULL) return kInvalidSecurityKey;
        }

        return (static_cast<uint64_t>(exchangeId) << 32) | static_cast<uint32_t>(code);
    }

    static inline uint64_t mix_security_key(SecurityKey key) noexcept
    {
        uint64_t z = key + 0x9e3779b97f4a7c15ULL;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    static inline std::size_t worker_index_for_security_key(
        SecurityKey key,
        std::size_t workerCount) noexcept
    {
        if (workerCount == 0) return 0;
        return static_cast<std::size_t>(mix_security_key(key) % workerCount);
    }

    static inline SecurityKey makeSecurityKey(uint8_t exchange, const char* securityCode) noexcept
    {
        return encode_security_key(exchange, securityCode);
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

    struct WorkerMessage
    {
        union Payload
        {
            marketdata::Order order;
            marketdata::Trade trade;
        };

        MessageType type = MessageType::NONE;
        Payload payload;

        WorkerMessage(): type(MessageType::NONE), payload() {}

        WorkerMessage(const WorkerMessage &other): type(MessageType::NONE), payload()
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

        static WorkerMessage fromOrder(const marketdata::Order &order)
        {
            WorkerMessage message;
            message.type = MessageType::ORDER;
            message.payload.order = order;
            return message;
        }

        static WorkerMessage fromTrade(const marketdata::Trade &trade)
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

    template<typename T>
    class SingleWorker
    {
        public:
            explicit SingleWorker(
                const std::string &date,
                std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> openBeforeData)
                : SingleWorker(date, std::move(openBeforeData), WorkerConfig{})
            {
            }

            SingleWorker(
                const std::string &date,
                std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> openBeforeData,
                const WorkerConfig &config):
                    workerId(config.workerId),
                    cpuId(config.cpuId),
                    cpuBind(config.cpuBind),
                    ringBuffer(std::max<std::size_t>(static_cast<std::size_t>(2), config.ringBufferCapacity)),
                    m_date(date),
                    m_openBeforeMap(std::move(openBeforeData))
            {
                m_orderbooks.reserve(1000);
                initPool();
            }

            T *getOrderbookPtr(const SecurityKey key)
            {
                auto iter = m_orderbooks.find(key);
                return iter == m_orderbooks.end() ? nullptr : iter->second.orderbook.get();
            }

            T *getOrderbookPtr(uint8_t exchange, const char *securityCode)
            {
                if (securityCode == nullptr)
                {
                    return nullptr;
                }

                const SecurityKey key = makeSecurityKey(exchange, securityCode);
                auto iter = m_orderbooks.find(key);
                return iter == m_orderbooks.end() ? nullptr : iter->second.orderbook.get();
            }

            const T *getOrderbookPtr(uint8_t exchange, const char *securityCode) const
            {
                if (securityCode == nullptr)
                {
                    return nullptr;
                }

                const SecurityKey key = makeSecurityKey(exchange, securityCode);
                auto iter = m_orderbooks.find(key);
                return iter == m_orderbooks.end() ? nullptr : iter->second.orderbook.get();
            }

            bool onOrderDetail(const marketdata::Order *order)
            {
                if (order == nullptr || order->volume <= 0)
                {
                    return false;
                }

                if (order->securityCode == nullptr)
                {
                    return false;
                }

                const SecurityKey key = makeSecurityKey(order->marketType, order->securityCode);
                T *lob = getOrCreateOrderbook(key, order->marketType, order->securityCode);
                if (lob == nullptr)
                {
                    return false;
                }
                lob->processOrder(order);
                return true;
            }

            bool onTransaction(const marketdata::Trade *trade)
            {
                if (trade == nullptr || trade->volume <= 0)
                {
                    return false;
                }

                if (trade->securityCode == nullptr)
                {
                    return false;
                }

                const SecurityKey key = makeSecurityKey(trade->marketType, trade->securityCode);
                T *lob = getOrCreateOrderbook(key, trade->marketType, trade->securityCode);
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

            std::size_t poolMaxSize()
            {
                if (m_orderPool)
                {
                    return m_orderPool->getMaxCount();
                }
                return 0;
            }

        private:
            struct OrderbookHolder
            {
                uint8_t exchange;
                std::string code;
                std::unique_ptr<T> orderbook;
            };

            inline void initPool()
            {
                if constexpr (std::is_same_v<T, LimitOrderBook>)
                {
                    m_orderPool = std::make_shared<OrderPool>();
                }
            }

            inline std::unique_ptr<T> createOrderbook(uint8_t exchange, const std::string &code)
            {
                if (!m_openBeforeMap)
                {
                    return nullptr;
                }

                auto iter = m_openBeforeMap->find(code);
                if (iter == m_openBeforeMap->end())
                {
                    return nullptr;
                }

                if constexpr (std::is_same_v<T, LimitOrderBook>)
                {
                    if (!m_orderPool)
                    {
                        LOG_ERROR(app_log::logger(), "create code:{} OrderBook error, object pool is nullptr", code);
                        STDTHROW(STD_ERROR_CODE, "create code:"<<code<<" OrderBook error, object pool is nullptr", "create code:"<<code<<" OrderBook error, object pool is nullptr");
                    }
                    return std::make_unique<T>(
                        m_date,
                        code,
                        exchange,
                        m_orderPool,
                        iter->second.preClose,
                        iter->second.limitDownPrice,
                        iter->second.limitUpPrice);
                }
                else
                {
                    return nullptr;
                }
            }

            // worker 内部真实主键改成 exchange+code，避免同代码跨市场/跨通道时互相覆盖。
            inline T *getOrCreateOrderbook(SecurityKey key, uint8_t exchange, const char* securityCode)
            {
                auto iter = m_orderbooks.find(key);
                if (iter == m_orderbooks.end())
                {
                    const std::string code = normalizeSecurityCode(securityCode);
                    OrderbookHolder holder;
                    holder.exchange = exchange;
                    holder.code = code;
                    holder.orderbook = createOrderbook(exchange, code);
                    auto [it, _] = m_orderbooks.emplace(key, std::move(holder));
                    iter = it;
                }
                return iter->second.orderbook.get();
            }

        public:
            int workerId = -1;
            int cpuId = -1;
            bool cpuBind = false;
            SpscRingBuffer<WorkerMessage> ringBuffer;
            std::thread thread;
            std::atomic<uint64_t> enqueued{0};
            std::atomic<uint64_t> dropped{0};
            std::atomic<uint64_t> processed{0};

        private:
            std::string m_date;
            std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> m_openBeforeMap;
            std::shared_ptr<OrderPool> m_orderPool;
            // 同一个 worker 内允许出现同 code 的多个 orderbook，因此 map key 必须带 market_type 信息。
            std::unordered_map<SecurityKey, OrderbookHolder> m_orderbooks;
    };


    template<typename T>
    class Workers
    {
        public:
            using OrderCallback = std::function<void(SingleWorker<T> *, const marketdata::Order *)>;
            using TradeCallback = std::function<void(SingleWorker<T> *, const marketdata::Trade *)>;
            using ProcessCallback = std::function<void(SingleWorker<T> *, const WorkerMessage *)>;

        public:
            Workers(
                const std::string &date,
                const std::string &cpuIds,
                std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> openBeforeData)
                : m_date(date), m_openBeforeData(openBeforeData)
            {
                initWorkers(cpuIds);
            }

            Workers(
                const std::string &date,
                const int32_t workerNum,
                std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> openBeforeData)
                : m_date(date), m_openBeforeData(openBeforeData)
            {
                initWorkers(workerNum);
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
                        SingleWorker<T> &worker = *workerPtr;
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

                        if (worker.cpuBind)
                        {
                            bindThreadToCpu(worker.thread, worker.cpuId);
                        }
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

            void setProcessCallback(ProcessCallback callback)
            {
                if (isRunning())
                {
                    throw std::logic_error("cannot set callback while workers are running");
                }
                m_processCallback = std::move(callback);
            }

            bool orderDetail(const marketdata::Order *order)
            {
                if (order == nullptr)
                {
                    return false;
                }
                if (m_skipSse && order->marketType == 1) return false;
                return submitMessage(makeSecurityKey(order->marketType, order->securityCode), std::move(WorkerMessage::fromOrder(*order)));
            }

            bool transaction(const marketdata::Trade *trade)
            {
                if (trade == nullptr)
                {
                    return false;
                }
                if (m_skipSse && trade->marketType == 1) return false;
                return submitMessage(makeSecurityKey(trade->marketType, trade->securityCode), std::move(WorkerMessage::fromTrade(*trade)));
            }

            SingleWorker<T> *getLobGroup(int workerId)
            {
                SingleWorker<T> *worker = findWorkerById(workerId);
                return worker;
            }

            const SingleWorker<T> *getLobGroup(int workerId) const
            {
                const SingleWorker<T> *worker = findWorkerById(workerId);
                return worker;
            }

            SingleWorker<T> *getLobGroupBySecurity(uint8_t exchange, const char* securityCode)
            {
                SingleWorker<T> *worker = findWorker(makeSecurityKey(exchange, securityCode));
                return worker;
            }

            const SingleWorker<T> *getLobGroupBySecurity(uint8_t exchange, const char* securityCode) const
            {
                const SingleWorker<T> *worker = findWorker(makeSecurityKey(exchange, securityCode));
                return worker;
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

            inline WorkerConfig makeWorkerConfig(
                int workerId,
                int cpuId,
                std::size_t ringBufferCapacity = 65536,
                bool cpuBind = false)
            {
                WorkerConfig config;
                config.workerId = workerId;
                config.cpuId = cpuId;
                config.ringBufferCapacity = ringBufferCapacity;
                config.cpuBind = cpuBind;
                return config;
            }

            inline void initWorkers(const std::string& cpuIds)
            {
                if (cpuIds.empty())
                {
                    throw std::invalid_argument("workers config must not be empty");
                }

                // "3,4,5,6,7" → [3,4,5,6,7]，分割出来的数量即为 worker 数量
                std::vector<int> workerCores;
                {
                    std::istringstream ss(cpuIds);
                    std::string token;
                    while (std::getline(ss, token, ','))
                    {
                        auto b = token.find_first_not_of(" \t");
                        if (b != std::string::npos)
                        {
                            workerCores.push_back(std::stoi(token.substr(b)));
                        }
                    }
                }
                int numWorkers = (int)workerCores.size();
                m_workers.reserve(numWorkers);
                for (int i = 0; i < numWorkers; ++i)
                {
                    WorkerConfig workerConfig = makeWorkerConfig(i, workerCores[i], 65536, true);
                    m_workers.emplace_back(std::make_unique<SingleWorker<T>>(m_date, m_openBeforeData, workerConfig));
                    const SingleWorker<T> *worker = m_workers.back().get();
                    LOG_INFO(app_log::logger(), "worker init, workerId:{}, cpu id:{}, ringbuffer size:{}", worker->workerId, worker->cpuId, worker->ringBuffer.capacity());
                }
                m_workersSize = m_workers.size();
                LOG_INFO(app_log::logger(), "workers init finish");
            }

            inline void initWorkers(const int32_t workerNum)
            {
                if (workerNum <= 0)
                {
                    throw std::invalid_argument("workers config must not be empty");
                }

                m_workers.reserve(workerNum);
                for (int i = 0; i < workerNum; ++i)
                {
                    WorkerConfig workerConfig = makeWorkerConfig(i, i);
                    m_workers.emplace_back(std::make_unique<SingleWorker<T>>(m_date, m_openBeforeData, workerConfig));
                    const SingleWorker<T> *worker = m_workers.back().get();
                    LOG_INFO(app_log::logger(), "worker init, workerId:{}, cpu id:{}, ringbuffer size:{}", worker->workerId, worker->cpuId, worker->ringBuffer.capacity());
                }
                m_workersSize = m_workers.size();
                LOG_INFO(app_log::logger(), "workers init finish");
            }

            inline void clearWorkerException()
            {
                std::lock_guard<std::mutex> lock(m_exceptionMutex);
                m_workerException = nullptr;
                m_hasWorkerException.store(false, std::memory_order_release);
            }

            inline void bindThreadToCpu(std::thread& th, int cpuId)
            {
                if (cpuId < 0)
                {
                    return;
                }

                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(cpuId, &cpuset);

                int rc = pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset);
                if (rc != 0)
                {
                    LOG_ERROR(app_log::logger(), "pthread_setaffinity_np failed, cpu_id:{}, rc:{}", cpuId, rc);
                }
                else
                {
                    LOG_INFO(app_log::logger(), "thread bind cpu id:{}", cpuId);
                }
            }

            inline void workerLoop(SingleWorker<T> &worker)
            {
                while (m_running.load(std::memory_order_acquire) || !worker.ringBuffer.empty())
                {
                    WorkerMessage message{};
                    if (!worker.ringBuffer.tryPop(message))
                    {
                        ringbuffer_detail::cpuRelax();
                        continue;
                    }

                    if (message.type == MessageType::ORDER)
                    {
                        marketdata::Order scopedOrder = message.payload.order;
                        scopeOrderIds(scopedOrder);
                        if (!worker.onOrderDetail(&scopedOrder))
                        {
                            // LOG_WARNING(app_log::logger(), "add order {} id {} failed", scopedOrder.security_code, scopedOrder.appl_seq_num);
                            continue;
                        }
                        if (m_orderCallback)
                        {
                            m_orderCallback(&worker, &message.payload.order);
                        }
                    }
                    else if (message.type == MessageType::TRADE)
                    {
                        marketdata::Trade scopedTrade = message.payload.trade;
                        scopeTradeIds(scopedTrade);
                        if (!worker.onTransaction(&scopedTrade))
                        {
                            // LOG_WARNING(app_log::logger(), "add trade {} id {} failed", scopedTrade.security_code, scopedTrade.appl_seq_num);
                            continue;
                        }
                        if (m_tradeCallback)
                        {
                            m_tradeCallback(&worker, &message.payload.trade);
                        }
                    }

                    if (m_processCallback && message.type != MessageType::NONE)
                    {
                        m_processCallback(&worker, &message);
                    }

                    worker.processed.fetch_add(1, std::memory_order_release);
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

            inline void setWorkerException(std::exception_ptr exceptionPtr)
            {
                std::lock_guard<std::mutex> lock(m_exceptionMutex);
                if (!m_workerException)
                {
                    m_workerException = std::move(exceptionPtr);
                }
                m_hasWorkerException.store(true, std::memory_order_release);
            }

            inline SingleWorker<T> *findWorker(SecurityKey key)
            {
                if (key == kInvalidSecurityKey || m_workers.empty())
                {
                    return nullptr;
                }
                // 单 worker 时把所有盘口都交给当前 worker；多 worker 时再按 exchange+code 做稳定分片。
                if (m_workers.size() == 1)
                {
                    return m_workers.front().get();
                }
                return m_workers[worker_index_for_security_key(key, m_workers.size())].get();
            }

            inline const SingleWorker<T> *findWorker(SecurityKey key) const
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

            inline bool submitMessage(SecurityKey key, WorkerMessage &&message)
            {
                if (!m_running.load(std::memory_order_acquire))
                {
                    return false;
                }

                SingleWorker<T> *worker = findWorker(key);
                if (worker == nullptr)
                {
                    return false;
                }

        #if ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP
                if (!worker->ringBuffer.tryPush(std::move(message)))
                {
                    worker->dropped.fetch_add(1, std::memory_order_release);
                    return false;
                }
        #else
                worker->ringBuffer.push(std::move(message));
        #endif

                worker->enqueued.fetch_add(1, std::memory_order_release);
                return true;
            }

            inline SingleWorker<T> *findWorkerById(int workerId)
            {
                if (workerId < 0 || static_cast<std::size_t>(workerId) >= m_workersSize) return nullptr;
                return m_workers[workerId].get();
            }

            inline const SingleWorker<T> *findWorkerById(int workerId) const
            {
                if (workerId < 0 || static_cast<std::size_t>(workerId) >= m_workersSize) return nullptr;
                return m_workers[workerId].get();
            }

            inline static void scopeOrderIds(marketdata::Order &order) noexcept
            {
                if (order.applSeqNum > 0)
                {
                    order.applSeqNum = makeScopedSeq(order.channelNo, order.applSeqNum);
                }
            }

            inline static void scopeTradeIds(marketdata::Trade &trade) noexcept
            {
                if (trade.applSeqNum > 0)
                {
                    trade.applSeqNum = makeScopedSeq(trade.channelNo, trade.applSeqNum);
                }
                if (trade.bidApplSeqNum > 0)
                {
                    trade.bidApplSeqNum = makeScopedSeq(trade.channelNo, trade.bidApplSeqNum);
                }
                if (trade.offerApplSeqNum > 0)
                {
                    trade.offerApplSeqNum = makeScopedSeq(trade.channelNo, trade.offerApplSeqNum);
                }
            }

        private:
            std::string m_date;
            bool m_skipSse = false;
            std::shared_ptr<const std::unordered_map<std::string, LOBBaseOpenBeforeData>> m_openBeforeData;
            std::vector<std::unique_ptr<SingleWorker<T>>> m_workers;
            size_t m_workersSize = 0;
            std::atomic<bool> m_running{false};
            std::atomic<bool> m_hasWorkerException{false};
            mutable std::mutex m_exceptionMutex;
            std::exception_ptr m_workerException = nullptr;
            OrderCallback m_orderCallback;
            TradeCallback m_tradeCallback;
            ProcessCallback m_processCallback;
    };

    using SubWorkerLimitOrderBook = SingleWorker<LimitOrderBook>;
    using WorkersLimitOrderBook = Workers<LimitOrderBook>;
}
}
#endif
