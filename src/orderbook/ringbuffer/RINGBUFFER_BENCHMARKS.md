# RingBuffer Notes And Benchmark Records

## 1. 目录现状

当前 ringbuffer 实现已经集中到 `src/orderbook/ringbuffer/`，主要文件如下：

- `RingBuffer.h`
  聚合头，统一 `#include` 当前目录下的 10 种队列实现。
- `detail.h`
  公共细节实现，包含 `round2()`、`cpuRelax()`、`AdaptiveBackoff`、`SlotStorageImpl<T>`。
- 接口
  `SpscRingBuffer<T>`、`MpscRingBuffer<T>`、`SpmcRingBuffer<T>`、`SpmcBroadcastRingBuffer<T>`、`MpmcRingBuffer<T>`。


## 2. 接口

队列使用“push/pop 风格”的接口：

- 写侧：`tryPush`、`push`、`tryEmplace`、`emplace`
- 读侧：`tryPop`、`pop`
- 广播队列额外带 `consumerId`
  `tryPop(size_t consumerId, T &out)`、`pop(size_t consumerId, T &out)`、`pending(size_t consumerId)`

这些 `push/pop` 并不是真正的阻塞系统调用，而是基于 `cpuRelax()` / `AdaptiveBackoff` 的自旋等待。


## 3. 五类模型的实现差异

- `SpscRingBuffer<T>`
  1 producer + 1 consumer。没有 CAS，也没有 per-slot `sequence`，只有 `head/tail` 和两端缓存游标，热路径最短。
- `MpscRingBuffer<T>`
  M producers + 1 consumer。producer 通过 CAS 竞争 `tail`，consumer 单独推进 `head`，同步成本介于 `SPSC` 和 `MPMC` 之间。
- `SpmcRingBuffer<T>`
  1 producer + N consumers。producer 单独推进 `tail`，consumer 通过 CAS 竞争 `head`，槽位复用依赖 `sequence`。
- `SpmcBroadcastRingBuffer<T>`
  1 producer + N consumers 广播。每个 consumer 都有自己的读位置，producer 是否能覆盖旧槽位取决于最慢消费者。
- `MpmcRingBuffer<T>`
  M producers + N consumers。读写两边都要 CAS 抢位置，`sequence` 协议最完整，同步成本最高。

## 4. 类型约束

- 所有队列都要求 `T` 默认可构造。
- `SpmcRingBuffer<T>` 和 `MpmcRingBuffer<T>` 额外要求 `T` 是 `nothrow copy assignable` 和 `nothrow move assignable`。
- `SpmcBroadcastRingBuffer<T>` 额外要求 `T` 可拷贝赋值，因为广播语义下每个 consumer 都会读同一条消息，不能在读侧把槽位内容 move 走。
- ZC 版本把“对象拷贝/移动”从队列内部挪到了调用方，因此广播 ZC 不再要求 `copy assignable`，但生命周期管理责任也随之转移给调用方。

## 5. 当前 Benchmark 工具

当前 `benchmark/` 目录里和 ringbuffer 直接相关的 benchmark 入口有两类：

- `ringbuffer_all_impls_bench`
  当前主入口，覆盖目录里的 10 个实现：
  `SpscRingBuffer`、`MpscRingBuffer`、`SpmcRingBuffer`、`SpmcBroadcastRingBuffer`、`MpmcRingBuffer`
- `ringbuffer_libs_bench`
  旧对比 benchmark，用来和 `folly` / `tbb` / `boost` 做横向比较；只有在本机能找到 `folly` 和 `TBB` 时才会编译。

### 5.1 `ringbuffer_all_impls_bench` 覆盖范围

- payload：`8B / 72B / 128B / 256B`
- 模式：`try` / `blocking`
- 拓扑：`SPSC / MPSC / SPMC / SPMC_BCAST / MPMC`
- 统计项：
  `pub_mops`、`deliv_mops`、`avg_op_ns`、`lat_avg_ns`、`lat_p50`、`lat_p95`、`lat_p99`、`lat_max`、`push_retry`、`pop_retry`
- 线程绑定：
  默认读取当前进程可用 CPU mask，并按顺序把 worker 线程绑到核心
- payload 选择：
  支持 `--payloads 8,72,128` 这种逗号分隔列表，只跑指定 payload，避免每次都把 `256B` 一起跑完

### 5.2 编译与运行

配置：

```bash
cmake -S benchmark -B benchmark/build -DCMAKE_BUILD_TYPE=Release
```

编译综合 benchmark：

```bash
cmake --build benchmark/build --target ringbuffer_all_impls_bench -j4
```

查看帮助：

```bash
./benchmark/build/ringbuffer_all_impls_bench --help
```

本次实际使用的命令：

```bash
./benchmark/build/ringbuffer_all_impls_bench --messages 20000 --cap 4096 --producers 4 --consumers 4 --latency-samples 2000 --bind-cores on --payloads 8,72,128
```

吞吐优先时建议额外跑一版：

```bash
./benchmark/build/ringbuffer_all_impls_bench --messages 20000 --cap 4096 --producers 4 --consumers 4 --throughput-only --bind-cores on --payloads 8,72,128
```

说明：

- `deliv_mops` 对广播队列表示“所有 consumer 收到的总消息数 / 时间”，因此会高于 `pub_mops`
- `try` 模式下的 `push_retry / pop_retry` 能直接反映队列竞争和忙等成本
- `--throughput-only` 和 `--latency-samples 0` 都会关闭热路径里的时间戳采样，不再分配 `publishTimes`，更适合看队列本体的纯吞吐
- `--producers` 会作用到 `MPSC / MPMC`；`--consumers` 会作用到 `SPMC / SPMC_BCAST / MPMC`
- 这次属于 smoke + 对比型 benchmark，重点看相对趋势，不要把单次结果当成绝对结论

## 6. 2026-04-06 综合实测

测试参数：

- `messages=20000`
- `cap=4096`
- `producers=4`
- `consumers=4`
- `bind_cores=on`
- `payloads=8,72,128`
- 每组先 warmup 1 轮，再连续跑 5 轮；下表统一取 5 轮中位数

### 6.1 Throughput-Only 吞吐结果

命令：

```bash
./benchmark/build/ringbuffer_all_impls_bench --messages 20000 --cap 4096 --producers 4 --consumers 4 --throughput-only --bind-cores on --payloads 8,72,128
```

表中吞吐统一使用 `deliv_mops`，同时保留 `avg_op_ns`：

- `SPSC / MPSC / SPMC / MPMC` 的 `deliv_mops` 就是实际消费吞吐
- `SPMC_BCAST` 的 `deliv_mops` 是 4 个 consumer 的总投递吞吐
- 这一组结果关闭了热路径时间戳采样，更适合看 ringbuffer 本体的纯吞吐
- 下表不是单次结果，而是相同命令连续跑 5 轮后，对每个 `class + mode + payload` 的 `deliv_mops` 和 `avg_op_ns` 取中位数

#### SPSC

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `SpscRingBuffer` | `try` | 107.97 | 9.26 | 22.01 | 45.43 | 17.43 | 57.36 |
| `SpscRingBuffer` | `blocking` | 126.11 | 7.93 | 17.85 | 56.03 | 18.56 | 53.87 |

#### MPSC

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `MpscRingBuffer` | `try` | 54.86 | 18.23 | 11.14 | 89.77 | 9.14 | 109.44 |
| `MpscRingBuffer` | `blocking` | 58.91 | 16.97 | 20.84 | 47.99 | 8.45 | 118.38 |

#### SPMC

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `SpmcRingBuffer` | `try` | 26.80 | 37.31 | 19.89 | 50.27 | 23.05 | 43.39 |
| `SpmcRingBuffer` | `blocking` | 32.33 | 30.93 | 33.19 | 30.13 | 23.32 | 42.88 |

#### SPMC_BCAST

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `SpmcBroadcastRingBuffer` | `try` | 47.42 | 21.09 | 13.88 | 72.06 | 13.12 | 76.20 |
| `SpmcBroadcastRingBuffer` | `blocking` | 57.97 | 17.25 | 14.08 | 71.03 | 13.22 | 75.64 |

#### MPMC

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `MpmcRingBuffer` | `try` | 21.33 | 46.88 | 9.42 | 106.11 | 7.57 | 132.14 |
| `MpmcRingBuffer` | `blocking` | 42.24 | 23.67 | 9.26 | 107.94 | 5.85 | 170.99 |

### 6.2 Sampled Latency 参考结果

命令：

```bash
./benchmark/build/ringbuffer_all_impls_bench --messages 20000 --cap 4096 --producers 4 --consumers 4 --latency-samples 2000 --bind-cores on --payloads 8,72,128
```

表中吞吐仍然记录 `deliv_mops`，但这里主要看 `lat_avg_ns` 和 `lat_p95_ns`：

- 这组结果会在热路径里做时间戳采样，因此吞吐会明显低于 `throughput-only`
- 适合观察各实现的延迟层级和 tail，不适合拿来做“纯吞吐”排序
- 和吞吐表一样，下表同样是 warmup 后连续 5 轮的中位数，不再放单轮样本

#### SPSC

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `SpscRingBuffer` | `try` | 64.84 | 12368.17 | 19227.00 | 20.31 | 176.40 | 221.00 | 20.29 | 196.74 | 221.00 |
| `SpscRingBuffer` | `blocking` | 83.96 | 22432.46 | 48073.00 | 19.07 | 173.67 | 210.00 | 17.13 | 156.91 | 190.00 |

#### MPSC

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `MpscRingBuffer` | `try` | 17.33 | 184776.82 | 329935.00 | 10.21 | 8126.98 | 27994.00 | 5.99 | 2552.98 | 15019.00 |
| `MpscRingBuffer` | `blocking` | 9.28 | 2408.07 | 1393.00 | 10.75 | 49494.57 | 118879.00 | 5.98 | 1028.41 | 1052.00 |

#### SPMC

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `SpmcRingBuffer` | `try` | 15.59 | 220312.71 | 515051.00 | 24.29 | 41969.99 | 125412.00 | 25.44 | 55497.77 | 90494.00 |
| `SpmcRingBuffer` | `blocking` | 13.83 | 253738.64 | 457621.00 | 31.95 | 18794.84 | 55537.00 | 23.04 | 11845.07 | 38574.00 |

#### SPMC_BCAST

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `SpmcBroadcastRingBuffer` | `try` | 43.12 | 67419.59 | 391112.00 | 12.37 | 1663.11 | 3277.00 | 12.41 | 1766.97 | 4389.00 |
| `SpmcBroadcastRingBuffer` | `blocking` | 42.18 | 20933.11 | 138356.00 | 14.01 | 1504.87 | 2475.00 | 12.49 | 1387.95 | 4438.00 |

#### MPMC

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `MpmcRingBuffer` | `try` | 18.76 | 13031.94 | 28144.00 | 6.82 | 1762.60 | 881.00 | 6.48 | 1683.65 | 761.00 |
| `MpmcRingBuffer` | `blocking` | 19.34 | 17612.70 | 46269.00 | 8.23 | 1009.47 | 772.00 | 7.87 | 1585.11 | 4499.00 |


## 7. 2026-05-21 当前 POSIX SHM 实测

本节使用新增的 `shm_current_bench` 测当前 `ShmWriter<MDMixedRecord>` / `ShmReader<MDMixedRecord>` 跨进程共享内存路径：

- payload：`MDMixedRecord`，当前大小 `73B`
- writer：父进程，单个 `ShmWriter`
- reader：fork 出来的独立子进程，每个进程一个 `ShmReader`
- 语义：广播读，3 readers 表示每个 reader 都独立读取完整数据流
- capacity：`2097152` slots，和 `MarketDataReplay` 当前默认 `TickShmCapacity` 一致
- CPU 绑定：开启；1 reader 为 `cpu_plan=0,1`，3 readers 为 `cpu_plan=0,1,2,3`
- 统计方式：warmup 1 轮，正式 5 轮，表中取 5 轮中位数

构建：

```bash
cmake -S benchmark -B benchmark/build -DCMAKE_BUILD_TYPE=Release
cmake --build benchmark/build --target shm_current_bench -j4
```

### 7.1 Throughput-Only 吞吐结果

命令：

```bash
./benchmark/build/shm_current_bench --messages 10000000 --capacity 2097152 --readers 1 --repeats 5 --warmup 1 --latency-sample-every 0 --bind-cores on
./benchmark/build/shm_current_bench --messages 10000000 --capacity 2097152 --readers 3 --repeats 5 --warmup 1 --latency-sample-every 0 --bind-cores on
```

说明：

- `writer_pub_mops` 是 writer 发布记录数 / writer 写入耗时。
- `aggregate_reader_mops` 是所有 reader 实际收到的总投递吞吐；3 readers 下约等于单 reader 吞吐乘以 3。
- `slowest_reader_mops` 是每轮最慢 reader 的吞吐。
- `read_left_behind` / `sequence_gaps` 都为 0，表示本组测试没有检测到 reader 被覆盖或序号断裂。

| readers | writer_pub_mops | aggregate_reader_mops | slowest_reader_mops | read_left_behind | sequence_gaps |
|---:|---:|---:|---:|---:|---:|
| 1 | 22.00 | 22.00 | 22.00 | 0 | 0 |
| 3 | 12.28 | 36.86 | 12.29 | 0 | 0 |

### 7.2 Sampled Latency 参考结果

命令：

```bash
./benchmark/build/shm_current_bench --messages 10000000 --capacity 2097152 --readers 1 --repeats 5 --warmup 1 --latency-sample-every 1000 --bind-cores on
./benchmark/build/shm_current_bench --messages 10000000 --capacity 2097152 --readers 3 --repeats 5 --warmup 1 --latency-sample-every 1000 --bind-cores on
```

延迟采样方式：

- writer 在采样消息 `push()` 前把 `clock_gettime(CLOCK_MONOTONIC_RAW)` 写进记录字段。
- reader 读到同一条采样消息后再次打时间戳。
- 因此这里是端到端延迟，包含排队/积压时间，不是单独一次 `push()` 或 `tryPopStatus()` 的指令级耗时。

| readers | writer_pub_mops | aggregate_reader_mops | lat_avg_ns | lat_p50_ns | lat_p95_ns | lat_p99_ns | lat_max_ns | read_left_behind | sequence_gaps |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 21.37 | 21.37 | 1483448 | 180 | 8661101 | 10060549 | 10385709 | 0 | 0 |
| 3 | 12.42 | 37.26 | 1748 | 280 | 10390 | 19497 | 77698 | 0 | 0 |

### 7.3 长跑 sanity check

额外跑了一次 5000 万条的单 reader 长跑，用来确认默认容量下是否会触发覆盖：

```bash
./benchmark/build/shm_current_bench --messages 50000000 --capacity 2097152 --readers 1 --repeats 1 --warmup 0 --latency-sample-every 0 --bind-cores on
```

结果：

| readers | messages | writer_pub_mops | aggregate_reader_mops | slowest_reader_mops | read_left_behind | sequence_gaps |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 50000000 | 20.37 | 20.37 | 20.37 | 0 | 0 |

### 7.4 当前 SHM 结果怎么读

- 1 writer + 1 reader 在 `73B` 行情混合结构上可持续约 `22 M records/s`。
- 1 writer + 3 readers 时，writer 发布吞吐降到约 `12.3 M records/s`，但总投递吞吐约 `36.9 M deliveries/s`。
- 3 readers 下 writer 降速主要来自多个 reader 同时读同一批 slot 带来的 cache coherency / memory bandwidth 压力。
- 1 reader sampled latency 的 p50 很低但 p95/p99 到毫秒级，说明 writer/reader 速率接近时会出现短时积压；3 readers 下 writer 被共享读压力压慢，端到端排队反而更少。
- 本节所有主测试和长跑 sanity check 均未出现 `readLeftBehind` 或 sequence gap。


## 8. 历史本地测试记录

下面这些结果保留为历史记录，时间是 `2026-04-05`。它们反映的是当时本地实验中对 origin / shm / blocking-nonblocking 的对比，不等同于“当前 CMake 可直接复现的 benchmark 目标”。


### 8.1 `ringbuffer_modes_bench`

| class | 8B try/block | 72B try/block | 128B try/block |
|---|---:|---:|---:|
| SPSC | 127.83 / 121.70 | 33.21 / 32.23 | 20.79 / 20.72 |
| SPMC | 43.43 / 28.85 | 38.13 / 29.68 | 29.15 / 29.14 |
| SPMC_BCAST | 111.97 / 127.77 | 26.11 / 31.79 | 16.42 / 16.71 |
| MPMC | 8.73 / 9.94 | 8.90 / 8.98 | 9.17 / 9.50 |

### 8.2 `spsc_origin_zc_shm_bench`

| payload | origin | shm |
|---|---:|---:|
| 8B | 226.41 | 33.17 |
| 72B | 26.61 | 15.06 |
| 128B | 33.29 | 11.74 |

### 8.3 `spsc_vs_shm_bench --case all --iterations 1000000 --buffer-bytes 536870912`

| case | payload | origin | shm |
|---|---:|---:|---:|
| uint64 | 8B | 92.41 | 32.45 |
| lev2_txn | 136B | 15.20 | 10.52 |
| order_intent | 56B | 19.40 | 16.86 |
| log_record | 512B | 13.70 | 2.55 |

## 9. 历史结果怎么读

- `MPMC` 仍然主要受 CAS 和 `sequence` 协调成本约束。
- `shm` 方案在同进程线程 benchmark 里通常不如进程内 ringbuffer，但它解决的是跨进程共享内存，不是同一个问题域。
