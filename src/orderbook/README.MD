# Orderbook 框架说明

## 整体架构

```
外部调用方
  │
  │  submitOrder(MDOrder) / submitTrade(MDTrade)
  ▼
┌─────────────────────────────────────────────────────────────────┐
│  Workers<T>                                                     │
│                                                                 │
│  channel → worker 映射表                                        │
│  m_channelToWorker: unordered_map<channelNo, workerIndex>       │
│                                                                 │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────┐ │
│  │  SingleWorker    │  │  SingleWorker    │  │  ...         │ │
│  │  channel=1       │  │  channel=2       │  │              │ │
│  │  cpuId=0 (绑核) │  │  cpuId=1 (绑核) │  │              │ │
│  └────────┬─────────┘  └────────┬─────────┘  └──────────────┘ │
└───────────┼────────────────────┼─────────────────────────────┘
            │                    │
     (每个Worker独立线程)
            │
            ▼
   ┌─────────────────┐
   │ SpscRingBuffer  │  SPSC无锁环形队列（主线程写，Worker线程读）
   │ <WorkerMessage> │  WorkerMessage = union { MDOrder, MDTrade }
   └────────┬────────┘
            │ tryPop / workerLoop
            ▼
   ┌─────────────────┐
   │  LOBGroup<T>    │  同一 channel 内所有股票的 LOB 容器
   │                 │
   │  m_orderbooks:  │
   │  unordered_map  │
   │  <code, T*>     │  按 security_code 路由到对应股票的 LOB
   │                 │
   │  共享对象池：    │
   │  OrderPool      │  ←── LimitOrderBook / MatchingEngine 使用
   └────────┬────────┘
            │ onOrderDetail / onTransaction → processOrder / processTrade
            ▼
   ┌──────────────────────────────────────────────────────────┐
   │  LOB 实现（T 的三种选择）                                 │
   └──────────────────────────────────────────────────────────┘
```

---

## LOB 三种实现对比

| 类名                       | 定位                                   | 订单深度 | 对象池      | 节点类型        |
|----------------------------|----------------------------------------|----------|-------------|-----------------|
| `LimitOrderBook`           | 成交消除订单簿 + 更精准的模拟撮合订单簿 | 有       | `OrderPool` | `OrderNode`     |
| `LimitOrderBookLite`       | 无订单深度的成交消除订单簿             | 无       | 无          | `OrderLiteMeta` |
| `MatchingEngine`           | 用户订单模拟撮合引擎                   | 有       | `OrderPool` | `OrderNode`     |

> **"有订单深度"**：每个价格档位维护一条双向链表，可以精确追踪每笔委托的 id、volume、排队位置（time priority）。
> **"无订单深度"**：只记录每个价格档位的总挂单量（total_volume）和委托数（order_size），内存占用更小。

---

## 成交消除订单簿（LimitOrderBook / LimitOrderBookLite）

`LimitOrderBook` 和 `LimitOrderBookLite` 的共同目标是基于逐笔委托和逐笔成交做成交消除，还原市场订单簿。

其中：

- `LimitOrderBook` 除了真实簿子的成交消除结果，还额外维护一套更精准的模拟撮合状态，可用于输出撮合后的订单簿视角。
- `LimitOrderBookLite` 只保留价格档位聚合信息，不维护逐笔订单深度。

```
processOrder(MDOrder)            processTrade(MDTrade)
       │                                │
       ▼                                ▼
  addOrder(id, price,            dropOrder(id, volume, side)
           volume, time, side)          │
       │                                │
       │  id_map / id_meta_map          │  pending_trade（乱序成交暂存）
       │                                │
       ▼                                ▼
  PriceBook<Comp>               从 PriceBook 中按 id 找到节点并删减量
  buy_book  (greater, 价降)
  sell_book (less,    价升)
  buy_market_book  (市价单独 PriceLevel)
  sell_market_book

  价格笼子（2020/2023起）：
  pending_buy_book  (less,    超笼上限的买单暂存)
  pending_sell_book (greater, 超笼下限的卖单暂存)
  → 每次 addOrder / dropOrder / setLastPrice 后调用
    checkAndMovePendingOrders() 重新归位
```

### 核心数据结构（LimitOrderBook）

```
OrderPool（对象池，最大 15,000,000 节点）
  └─ pool: vector<OrderNode>
  └─ free_list: 单向链表，O(1) alloc / free

OrderNode（双向链表节点）  [NodePool.h]
  ├─ id, price, volume, original_volume, time
  ├─ order_type (MarketOrderType)
  ├─ is_buy
  ├─ prev / next
  └─ price_level → PriceLevel

PriceLevel（价格档位）  [NodePool.h]
  ├─ head / tail → OrderNode 双向链表
  ├─ order_size
  └─ total_volume

PriceBook<Comp>  [PriceBook.h]
  └─ levels: map<price, PriceLevel, Comp>
             买盘 Comp=greater，卖盘 Comp=less
```

### LimitOrderBookLite 差异

```
无 OrderPool，无 OrderNode 链表
id_meta_map: unordered_map<id, OrderLiteMeta{price, remaining_volume}>
PriceLevel: 只有 order_size + total_volume（无链表指针）
```

---

## 用户订单模拟撮合引擎（MatchingEngine）

`MatchingEngine` 的定位不是替代真实市场订单簿回放，而是作为撮合引擎，用于用户订单模拟撮合。

它在订单簿结构基础上，额外支持：

```
1. 连续竞价撮合
   processOrder → addOrder → 尝试与对手盘撮合 → 触发 matchCallback
   支持订单类型：
     LIMIT                 限价单
     MARKET_BEST_OPPONENT  对手方最优
     MARKET_BEST_SELF      本方最优
     MARKET_FIVE_LEVEL     最优五档即时成交剩余撤销
     MARKET_IMMEDIATE      即时成交剩余撤销
     MARKET_ALL_OR_CANCEL  全额成交或撤销

2. 集合竞价撮合（开盘 9:15-9:25 / 收盘 14:57-15:00）
   TradingPhase 状态机控制不同阶段行为：
     PRE_OPEN → OPEN_CALL_AUCTION → OPEN_CALL_MATCH
     → CONTINUOUS_TRADING → CLOSE_CALL_AUCTION → CLOSED

3. 撮合回调
   setMatchCallback(fn)  → 每笔成交触发 MatchRecord 回调

4. 与 LimitOrderBook 比对和修正（可选）
   setLOBRef() → 用真实成交流校准模拟撮合结果
```

### 核心数据结构（MatchingEngine）

```
复用 NodePool.h 中的 OrderPool / OrderNode / PriceLevel
（与 LimitOrderBook 共享同一 OrderPool 实例，通过 LOBGroup 传入）

MatchRecord（成交记录）
  ├─ match_id, bid_order_id, offer_order_id
  ├─ price, volume, datetime
  └─ side
```

---

## Workers 并发模型

```
Workers<T>(date, vector<WorkerConfig>, poolSize=0)
  ├─ start() / stop()                  启动/停止所有 worker 线程
  ├─ isRunning()                       查询运行状态
  ├─ submitOrder(MDOrder) / submitTrade(MDTrade)
  │    按 exchange + security_code 计算 SecurityKey，再 hash 到对应 SingleWorker 的 ringBuffer
  │    注：date < "20210607" 时自动跳过上交所（'6'开头）数据 (m_skip_sse)
  │    onOrderDetail / onTransaction 为同名别名，语义相同
  ├─ hasWorker(workerId)               查询 workerId 是否存在
  ├─ setOrderCallback(fn)              每次 order 处理后触发（worker 线程中执行）
  ├─ setTradeCallback(fn)              每次 trade 处理后触发（worker 线程中执行）
  ├─ getLobGroup(workerId)             获取指定 worker 的 LOBGroup 指针
  ├─ getLobGroupBySecurity(channelNo, securityCode)
  │    根据证券所在桶定位对应 worker 的 LOBGroup
  ├─ getWorkerStats(workerId)          获取指定 worker 的统计信息
  ├─ getAllWorkerStats()               获取所有 worker 的统计信息
  └─ rethrowIfWorkerFailed()          若 worker 线程抛出异常则在调用方重新抛出

SingleWorker（内部结构）
  ├─ workerId: int                       worker 标识
  ├─ cpuId: int                           绑核 CPU id（-1 = 不绑核）
  ├─ ringBuffer: SpscRingBuffer           SPSC 无锁队列
  ├─ lobGroup: LOBGroup<T>                该 shard 的 LOB 容器
  └─ enqueued / dropped / processed       原子计数器

WorkerConfig（构建时传入）
  ├─ workerId: int
  ├─ cpuId: int
  └─ ringBufferCapacity: size_t（默认 40960）

// 便捷工厂函数
WorkerConfig makeWorkerConfig(workerId, cpuId, ringBufferCapacity=40960)

WorkerStats（getWorkerStats 返回值）
  ├─ workerId, cpuId
  ├─ enqueued / dropped / processed   累计计数
  ├─ pending                          队列中待处理条数
  └─ queueCapacity                    队列容量

入队模式（编译期选择）：
  ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP=0（默认）  阻塞式 push，不丢弃
  ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP=1          tryPush，队满则丢弃并计数
```

---

## SpscRingBuffer 性能

`RingBuffer.h` 提供两种无锁队列：

| 队列             | SPSC Mops/s | SPSC Avg ns/op | MPMC Mops/s |
|------------------|------------:|---------------:|------------:|
| `SpscRingBuffer` |       87.59 |          11.42 |         N/A |
| `MpmcRingBuffer` |       10.43 |          95.83 |       63.83 |

Workers 内部使用 `SpscRingBuffer`（主线程写、worker 线程读）。容量自动向上取 2^n，支持 `tryPush`/`tryPop`（非阻塞）和 `push`/`pop`（自旋等待）。

---

## 类型别名速查

```cpp
// Workers
using WorkersLimitOrderBook           = Workers<marketdata::LimitOrderBook>;
using WorkersLimitOrderBookLite       = Workers<marketdata::LimitOrderBookLite>;
using WorkersMatchingEngine = Workers<marketdata::MatchingEngine>;

// LOBGroup
using LOBGroupLimitOrderBook           = LOBGroup<marketdata::LimitOrderBook>;
using LOBGroupLimitOrderBookLite       = LOBGroup<marketdata::LimitOrderBookLite>;
using LOBGroupMatchingEngine = LOBGroup<marketdata::MatchingEngine>;
```

---

## 文件结构

| 文件                              | 内容                                                         |
|-----------------------------------|--------------------------------------------------------------|
| `NodePool.h`                      | `MarketOrderType`, `OrderNode`, `PriceLevel`, `OrderPool`    |
| `PriceBook.h`                     | `PriceBook<Comp>`, `PriceBookGreat`, `PriceBookLess`         |
| `LimitOrderBook.h`                | 成交消除订单簿（有深度），并维护更精准的模拟撮合订单簿状态，依赖 `OrderPool` |
| `LimitOrderBookLite.h`            | 无订单深度的成交消除订单簿，`OrderLiteMeta`                   |
| `MatchingEngine.h`                | 用户订单模拟撮合引擎，`TradingPhase`, `MatchRecord`           |
| `LOBGroup.h`                      | 按 security_code 管理多只股票的 LOB 容器                      |
| `RingBuffer.h`                    | `SpscRingBuffer`, `MpmcRingBuffer`                           |
| `Workers.h`                       | 多线程 worker 框架，按证券 code hash 到对应 LOBGroup          |

---

## 交易所区分

| channel_no | 交易所 | 订单方向字段    | 撤单判断      |
|------------|--------|-----------------|---------------|
| < 10       | 上交所 | side: B/S       | order_type=D  |
| > 2000     | 深交所 | side: 1/2/G/F   | exec_type=4   |

---

## 价格笼子规则

| 市场             | 生效时间   | 上限（买单）              | 下限（卖单）              | 超限处理  |
|------------------|------------|---------------------------|---------------------------|-----------|
| 科创板（68xxxx） | 2019-07-22 | base × 102%               | base × 98%                | 废单      |
| 创业板（3xxxxx） | 2020-06-12 | base × 102%               | base × 98%                | 暂存→等待 |
| 全市场           | 2023-04-10 | max(102%, base+10tick)    | min(98%, base-10tick)     | 废单/暂存 |

基准价优先级（买单）：ask1 → bid1 → lastPrice → preClose
基准价优先级（卖单）：bid1 → ask1 → lastPrice → preClose

---

## 数值精度

所有价格、成交量字段均为整数，**价格单位为微元（1元 = 1,000,000）**。

---

## 已知问题

2024-09-27 ~ 2025-10-14 期间行情活跃，可能因交易所性能导致行情乱序。

代表案例：code `301618`，`20241008093107340` 最后一条数据到下一条 `20241008094108000` 延迟约1分钟。
id `11473527` 已发生价格相交，但直到同一 id 才完成成交。

## 快速验证

20220114 300261 这票包含了 价格笼子、现价单、多种市价单、涨停的情况，如果这只票模拟撮合验证成功，基本上订单簿重构的没有问题
