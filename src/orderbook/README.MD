# Orderbook

本目录实现逐笔订单/成交驱动的订单簿重建，以及将重建工作按证券并行化的 `Workers`。当前生产链路由 `StrategyRunner` 使用 `WorkersLimitOrderBook`：逐笔数据先进入各 LOB worker，订单簿处理完成后再生成快照并发布给策略。

## 当前运行链路

```text
MixedRecord(Order / Trade)
          |
          v
MDReplayRunner
          |
          |  按 marketType + securityCode 稳定分片
          v
WorkersLimitOrderBook
  |                         |
  | shard 0                 | shard N
  v                         v
SingleWorker              SingleWorker
  |                         |
  | SpscRingBuffer          | SpscRingBuffer
  v                         v
LimitOrderBook            LimitOrderBook
  |                         |
  +---- ProcessCallback ---+
               |
               v
MDReplayRunner::onLOBProcessedMessage
  |  原始逐笔 + 处理后的 MDRapidSnapshot
  v
每 shard 一个 SPMC broadcast ring（由 StrategyRunner 创建）
  |
  v
多个策略独占线程消费
```

每个 `SingleWorker` 内部的订单簿与内存结构如下。`OrderPool` 虽定义在 `NodePool.h`，但它不是每只证券各一份，而是同一 worker 内全部证券共享的一份节点池。

```text
SingleWorker（一个 shard、一个处理线程）
├─ SpscRingBuffer<WorkerMessage>
├─ OrderPool  [NodePool.h；该 worker 内所有证券共享]
│  ├─ idIndex: scoped order id -> OrderNode slot
│  ├─ m_nodes[16,000,000]: 预分配的 OrderNode
│  └─ freeStack: 空闲 OrderNode slot
└─ unordered_map<SecurityKey, LimitOrderBook>
   └─ 一只证券一个 LimitOrderBook
      ├─ buyBook : PriceLevelBook<true>   （高价优先）
      │  └─ PriceLevel[]（按 tick 索引）
      │     └─ 每个活跃 PriceLevel: head/tail -> OrderNode 双向链表
      ├─ sellBook: PriceLevelBook<false>  （低价优先）
      │  └─ PriceLevel[]（按 tick 索引）
      │     └─ 每个活跃 PriceLevel: head/tail -> OrderNode 双向链表
      └─ OrderNode.priceLevel 指回所属 PriceLevel

OrderNode 实际存放在上方共享的 OrderPool 中；PriceLevelBook 只管理按价格定位的
PriceLevel 数组、活跃位图和最优 tick，不复制订单节点。
```

订单簿模块只负责“输入逐笔 → 更新对应 LOB → 在 worker 线程中回调”。SPMC 广播、策略线程、跨策略背压由 `src/StrategyRunner/RunStrategy.cpp` 负责；详见 [StrategyRunner README](../StrategyRunner/README.md)。

## 分片与线程模型

`Workers<T>` 创建固定数量的 `SingleWorker<T>`。路由键是 `SecurityKey = marketType + securityCode`，经过 `mix_security_key(key) % workerCount` 选择 shard。因此：

- 同一市场、同一证券的所有消息始终进入同一个 worker，保证该证券的 LOB 不会被并发修改。
- worker 并不按 `channelNo` 分配；不同 channel 的同一证券仍进入同一个 worker。
- 证券代码相同但市场不同会使用不同 `SecurityKey`，不会共用一个订单簿。
- 不同 shard 可以并行处理；不同 shard 之间不保证全局事件顺序。

每个 worker 有一条 `SpscRingBuffer<WorkerMessage>`，调用 `orderDetail()` / `transaction()` 的线程是生产者，worker 自己的线程是消费者。当前回放器只由一个提交线程调用这两个接口；若改为多个并发提交线程，不能继续直接使用这条 SPSC 队列，需要先串行化提交，或更换为适合多生产者的队列。

`stop()` 会停止接收新任务、继续处理 ring 中已入队的消息，并 join 所有 worker 线程。任何 worker 异常都会被保存，调用方应在提交过程与停止后调用 `rethrowIfWorkerFailed()`。

## Worker 生命周期与回调

```cpp
using namespace marketdata::orderbook;

auto workers = std::make_unique<WorkersLimitOrderBook>(
    date, workerCount, openBeforeData);

workers->setProcessCallback(
    [](SubWorkerLimitOrderBook* worker, const WorkerMessage* message) {
        // 已完成本条消息对应的 LimitOrderBook 更新。
        // 回调运行在该 worker 的线程中；不得并发访问同一 worker 内的 LOB。
    });

workers->start();
workers->orderDetail(&order);
workers->transaction(&trade);
workers->stop();
workers->rethrowIfWorkerFailed();
```

回调必须在 `start()` 前设置，运行中修改会抛出异常。回调执行在 worker 线程，因而必须短小、无阻塞；慢回调会直接降低该 shard 的订单簿吞吐。回调所捕获对象必须至少存活到 `stop()` 返回。

三类回调的顺序为：

1. 将 `WorkerMessage` 复制成内部消息，并将订单号按 `channelNo` 做 scoped 转换；
2. `SingleWorker::onOrderDetail()` 或 `onTransaction()` 更新 `LimitOrderBook`；
3. 如有设置，调用 order/trade 回调；
4. 如有设置，调用 process 回调；
5. 增加 `processed` 计数。

order/trade/process 回调收到的是原始 `WorkerMessage` 内容；LOB 内部使用 scoped 订单号，避免不同 channel 的相同序号冲突。调用方若需要读取 LOB，应从回调参数的 `SingleWorker` 以 `marketType + securityCode` 查询。`MDReplayRunner` 采用这种方式，在第 4 步生成“本条逐笔处理之后”的 `MDRapidSnapshot`，随后将 snapshot 的 `id` 恢复为输入逐笔的原始 `applSeqNum` 后发布。

## 队列、背压与 CPU 绑定

`WorkerConfig::ringBufferCapacity` 和 `SpscRingBuffer` 容量均会向上取整为 2 的幂。当前 `Workers` 有两种**创建方式**，实际默认容量均为 `65536`：

| 创建方式 | 使用场景 | 行为 |
| --- | --- | --- |
| `Workers(date, workerCount, openBeforeData)` | 容器、开发环境或不希望固定核心时 | 创建指定数量的 worker，不请求 CPU 绑定；worker 的 `cpuId` 仅为其编号 |
| `Workers(date, "3,4,5", openBeforeData)` | 已规划 CPU 隔离和亲和性时 | 按逗号拆分 CPU 列表，创建相同数量的 worker，并使用 `pthread_setaffinity_np` 分别绑定到这些核心 |

`MDReplayRunner` 优先读取 `WorkerCpuIds`；该项为空时读取 `ReplayWorkerCount`。两种方式的证券路由算法完全一致，差别只在 worker 数量如何给出以及是否请求绑核。不要同时把策略消费线程和 LOB worker 绑定到同一组孤立 CPU，除非已经为它们做了明确的核位规划。

典型使用模式如下：

```cpp
using namespace marketdata::orderbook;

// 1. 每只证券在创建 LOB 前都需要有前收、涨跌停等静态数据。
auto base = std::make_shared<std::unordered_map<std::string, LOBBaseOpenBeforeData>>();
base->emplace("000001", LOBBaseOpenBeforeData{"000001", preClose, limitUp, limitDown});

// 2a. 仅指定 shard 数量。
WorkersLimitOrderBook workers(date, 6, base);

// 2b. 或由 CPU 列表同时确定 shard 数量和亲和性。
// WorkersLimitOrderBook workers(date, "3,4,5,6,7,8", base);

// 3. 在 start 前注册回调，然后由唯一提交线程写入 order/trade。
workers.setProcessCallback(onProcessed);
workers.start();
workers.orderDetail(&order);
workers.transaction(&trade);
workers.stop();
workers.rethrowIfWorkerFailed();
```

`SingleWorker` 会在首次处理某证券消息时，从 `openBeforeData` 找到该证券的静态数据并创建 `LimitOrderBook`。输入证券键无效时，提交接口返回 `false`；但找不到静态数据时，当前实现会使 `onOrderDetail()` / `onTransaction()` 返回 `false`，worker 跳过该消息及回调，而提交端已经成功入队。运行前必须保证 `openBeforeData` 覆盖回放证券，不能把它当作可忽略的缺失配置。

入队策略也有两种**编译期实现方式**：

默认编译配置为：

```cpp
ORDERBOOK_WORKERS_ENQUEUE_MODE_TRY_DROP = 0
```

即队列满时 `push()` 自旋等待、不会主动丢逐笔。设为 `1` 后会改为 `tryPush()`：队满立即返回 `false` 并增加 `dropped` 计数。回放链路要求完整性，应保持默认阻塞模式。

在合并后的运行架构中有两段背压：LOB worker ring 满时会阻塞回放提交线程；策略 SPMC ring 满时会阻塞对应的 LOB worker 回调。因此最终吞吐由最慢的策略、LOB shard 或数据读取阶段共同决定。

## NodePool / OrderPool 实现逻辑

文件名 `NodePool.h` 保留自早期命名；当前核心类型是 `OrderPool`。它解决的问题是：逐笔回放期间不能为每一笔新增、撤销订单频繁 `new`/`delete`，同时还需要根据订单号快速定位节点。

### 数据结构

| 组件 | 当前实现 | 作用 |
| --- | --- | --- |
| `OrderNode` | 定长数组中的一个元素，以 slot 而非裸指针互相连接 | 保存 id、价格、剩余量、时间、方向、订单类型，以及 `prevSlot` / `nextSlot` / `priceLevelPtr` |
| `OrderPool::m_nodes` | `16,000,000` 个预分配 `OrderNode` | 节点存储；slot 在节点生命周期内稳定 |
| `m_freeStack` + `m_freeTop` | LIFO 空闲 slot 栈 | O(1) 取得与归还节点，不依赖堆分配器 |
| `OrderIdIndex` | 容量为 `1 << 25` 的开放寻址哈希表 | `scoped order id -> node slot`，线性探测，使用 empty/used/tombstone 三种状态 |
| `PriceLevel` | 价格档的头尾 slot、总量、订单数和匹配视图统计 | 将同一价位的订单串成时间优先队列 |

`OrderIdIndex` 的负载因子限制为 70%。插入时先检查重复 id，再从哈希位置线性探测；删除标记 tombstone，避免破坏同一探测链上的后续查找。查找、插入和删除在正常负载下均为 O(1) 期望复杂度，但不是并发容器，只能由所属 worker 线程访问。

### 分配、挂链与释放

```text
addOrder(id, price, volume)
  -> OrderPool::alloc(id)
       1. 从 freeStack 取一个 slot
       2. reset 节点并写入 id
       3. 在 OrderIdIndex 写入 id -> slot
  -> PriceLevelBook::getOrCreate(price)
  -> OrderPool::link(level, node)
       追加到 level.tailSlot，维护 prev/next、head/tail、总量和订单数

dropOrder(id, volume)
  -> OrderPool::find(id)
  -> 全量删除：unlink(node) -> 删除空 PriceLevel -> free(node)
  -> 部分删除：只扣减 node.volume 和 level.totalVolume

free(node)
  -> 若仍挂在 PriceLevel 上先 unlink
  -> 从 OrderIdIndex 删除 id
  -> reset 节点，保留 selfSlot，并把 slot 压回 freeStack
```

新订单总是追加到 `PriceLevel` 尾部，因此该价位的 `headSlot -> nextSlot` 就是时间优先顺序。slot 链接而非对象指针链接的好处是节点池连续、无需为链表额外分配内存，且回收后可快速复用。

`OrderPool` 是**每个 worker 一份**、同 worker 内全部证券共享，故 `OrderIdIndex` 中不能直接使用交易所的原始序号。`Workers::scopeOrderIds()` / `scopeTradeIds()` 会先将它转为 `makeScopedSeq(channelNo, rawSeq)`；订单、成交、撤单必须使用同一转换后的 id。策略回调仍拿到原始逐笔，不能把其中的原始 `applSeqNum` 直接用于内部 `OrderPool::find()`。

这份预分配内存会随 worker 数线性增长。调大 `ReplayWorkerCount` 前应先评估 `16,000,000` 节点和 `1 << 25` 哈希表在每个 worker 上的常驻内存，而不是只看 SPSC ring 的容量。

## PriceLevelBook 实现逻辑

`PriceLevelBook<Descending>` 是价格档索引，不是 `std::map`。构造时用涨跌停区间和固定 tick 建立连续数组：`LimitOrderBook` 当前传入 `tickSize = 10000`，即 0.01 元。

```text
price -> tick = (price - minPrice) / tickSize
                 |
                 v
        m_levels[tick] : PriceLevel
        m_bitmap[tick] : 该价格档是否活跃
```

实现思路是用可预测的数组下标代替树或哈希查找：

1. `getOrCreate(price)` 校验价格在 `[minPrice, maxPrice]` 内且对齐 tick；若位图未置位，则 reset 对应 `PriceLevel`、置位 bitmap、更新活跃档数和 `bestTick`。
2. `find` / `findByTick` 仅检查范围和 bitmap，无需分配。
3. 档位总量变为 0 后，`eraseIfEmpty` / `erase` reset 该 `PriceLevel`、清 bitmap；若删掉最优档，通过 bitmap 找下一个有效 tick。
4. `topNToBuffer` 从 `bestTick` 开始按活跃位图遍历，将结果写到调用方提供的 buffer，避免热路径临时分配。

买盘实例为 `PriceLevelBook<true>`：`bestTick` 最大，`nextActiveTick()` 向低价寻找；卖盘实例为 `PriceLevelBook<false>`：`bestTick` 最小，`nextActiveTick()` 向高价寻找。bitmap 以 64 个 tick 为一组，使用 `ctz/clz` 找到下一/上一位 set bit，稀疏盘口不必线性扫描完整价格区间。

`PriceLevelBook` 只管理价格档，不拥有订单节点。一个 `PriceLevel` 中保存 `headSlot`、`tailSlot`、`totalVolume`、`orderSize`；订单链的实际节点仍在 `OrderPool`。因此“找最优价格”与“在该价位按时间找到首笔订单”分别是 bitmap 与 slot 链表两条路径。

## PriceCage 实现逻辑

`PriceCage` 只计算价格笼子的基准价、上下界和单价是否在笼内，不保存订单，也不做物理迁移。

基准价优先级：

| 场景 | 基准价顺序 |
| --- | --- |
| 新买单/买盘可见性 | `ask1 -> bid1 -> lastPrice -> preClose` |
| 新卖单/卖盘可见性 | `bid1 -> ask1 -> lastPrice -> preClose` |

给定基准价 `base`，当前实现的边界为：

```text
普通规则（m_amain = false）
  buy upper = round_0.01(base * 1.02)
  sell lower = round_0.01(base * 0.98)

主板规则（m_amain = true）
  buy upper = round_0.01(max(base * 1.02, base + 0.10))
  sell lower = round_0.01(min(base * 0.98, base - 0.10))
```

`PriceCage` 内部为实现百分比和分位舍入，会暂时将微元转换为 `double`，按两位小数四舍五入后再转回微元；订单簿的价量状态本身仍以整数保存。修改边界计算时，应补充精度边界测试，避免浮点中间值改变一分的舍入结果。

`LimitOrderBook` 何时开启价格笼子由构造参数决定：当前代码对 2020-06-12 起的深市 `3xxxxx`、以及上交所 `68xxxx` 开启非主板规则；其他证券从 2023-04-10 起开启主板规则。连续竞价时，`checkAndMovePendingOrders()` 会调用 `PriceLevelBook::refreshBestByCage()`：

- 笼内档位成为 `bestTick`，用于快照和后续撮合视图；
- 最靠近笼子边界的笼外档位记入 `cageTick`；
- 笼外订单仍保留在同一个 `PriceLevelBook` 中，当前实现并没有单独的 pending book，也不会在这里删除订单；
- 非连续竞价时调用 `clearCage()`，恢复普通最优价规则。

所以 `checkAndMovePendingOrders` 是历史命名：当前实际语义是“根据笼子刷新可见最优档和边界档”，不是把节点搬进另一份容器。

## LimitOrderBook 实现逻辑

`LimitOrderBook` 是当前 `WorkersLimitOrderBook` 唯一实际创建的订单簿。它以一只证券为单位，组合 `OrderPool`、一买一卖两个 `PriceLevelBook`、逐笔规则、价格笼子和独立的撮合视图。

### 每条逐笔的主流程

```text
Order / Trade
  -> determinePhase(time) + handlePhaseTransition()
  -> 根据 channelNo 区分沪市（< 10）与深市（> 2000）字段语义
  -> addOrder / dropOrder 更新真实订单簿
  -> 更新最新价、量额、价格笼子可见档
  -> 连续竞价中若买一 >= 卖一，更新独立的匹配视图
  -> worker ProcessCallback 发布“本条逐笔处理后”的快照
```

`processOrder()` 对沪市处理普通委托 `A` 和撤单 `D`；对深市处理买/借入 `1/G`、卖/出借 `2/F`，以及限价 `2`、对手方最优 `1`、本方最优 `U` 等当前支持的申报类型。无可用对手价或本方最优价的市价类订单会记入 `m_skipIds`，不进入订单簿。

`processTrade()` 依据市场、交易阶段、成交方向和 bid/offer 订单号确定要扣减的一侧或两侧订单，并累计 `m_lastPrice`、`m_volumes`、`m_turnover`。若成交先于对应订单到达，`dropOrder()` 找不到节点时会把待扣数量写入 `m_pendingTrade[id]`；后续 `addOrder()` 遇到该 id 时先抵消这部分数量。这是对乱序逐笔的补偿，不代表任意顺序都可无损恢复。

### 交易阶段、集合竞价和匹配视图

当前交易阶段划分为：开盘前、9:15–9:25 开盘集合竞价、9:25–9:30 撮合过渡、连续竞价（9:30–11:30、13:00–14:57）、14:57–15:00 收盘集合竞价以及收盘。由开盘集合竞价过渡到连续竞价时会重置匹配视图状态。

集合竞价价格计算不会扫描每一个价格单位。它读取买卖活跃 `PriceLevel`：买盘高到低、卖盘低到高；将两者做线性归并，维护候选价的累计可买量和累计可卖量，按“最大可成交量 → 最小不平衡量 → 交易所规则”选择价格。上海对最终并列区间取中间价，深圳按开/收盘的参考价选最近候选价。复杂度为 O(买方活跃档数 + 卖方活跃档数)。

连续竞价的 `tryMatchCrossedMainBook()` 不会从真实回放簿中删除订单。它使用 `OrderNode::remainingMatchVolume`、`PriceLevel::matchTotalVolume` 等独立 `match*` 字段模拟买卖盘相交后的可见盘口，并在真实簿变化时按变更节点/价位增量重置。`ReplayUseMatchSnapshot=1` 选择该视图，`0` 则输出真实簿视图。

`MakeSnapshot` 读取选定视图并输出 `MDRapidSnapshot`。普通视图调用 `updateSnapshot()`；匹配视图调用 `updateMatchingSnapshot()`。两者都在 LOB worker 内、且在本条逐笔处理完成后生成，因此策略无需自己重建订单簿即可使用盘口。

价格与金额字段均使用整数缩放值；价格单位为微元（1 元 = 1,000,000）。不要在订单簿更新路径中改用浮点数。

## 主要文件

| 文件 | 作用 |
| --- | --- |
| `Workers.h` | `Workers`、`SingleWorker`、证券分片、SPSC 队列和 worker 生命周期 |
| `LimitOrderBook.h` | 逐笔订单簿重建、成交消除、交易阶段和价格笼子处理 |
| `MakeSnapshot.h` | 从 `LimitOrderBook` 生成 `MDRapidSnapshot` |
| `NodePool.h` | `OrderPool`、订单节点和价格档位节点 |
| `PriceBook.h` | 买卖价格档位容器和最优档查询 |
| `PriceCage.h` | 价格笼子计算 |
| `MatchingEngine.h` / `MatchTypes.h` | 独立的模拟撮合组件；当前回放 worker 不创建它 |

## 验证建议

修改订单簿或 worker 路由后，先用单日、少量证券执行：

```bash
cd app/mrr3
TZ=Asia/Shanghai ../../build/src/StrategyRunner \
  config/app_v2.txt run 20260717 300261
```

然后用 `OutputLimitOrderBookStrategy` 或 `OutputMarketDataStrategy` 对比盘口和逐笔输出。验证并行分片时，再以相同配置运行 `runALL` / `runByDates`，检查 worker 异常、`shardSeq` 连续性及策略结果。
