# RingBuffer Notes And Benchmark Records

## 1. 目录现状

当前 ringbuffer 实现已经集中到 `src/orderbook/ringbuffer/`，主要文件如下：

- `RingBuffer.h`
  聚合头，统一 `#include` 当前目录下的 10 种队列实现。
- `detail.h`
  公共细节实现，包含 `round2()`、`cpuRelax()`、`AdaptiveBackoff`、`SlotStorageImpl<T>`。
- 原版拷贝式接口
  `SpscRingBuffer<T>`、`MpscRingBuffer<T>`、`SpmcRingBuffer<T>`、`SpmcBroadcastRingBuffer<T>`、`MpmcRingBuffer<T>`。
- zero-copy 接口
  `SpscRingBufferZC<T>`、`MpscRingBufferZC<T>`、`SpmcRingBufferZC<T>`、`SpmcBroadcastRingBufferZC<T>`、`MpmcRingBufferZC<T>`。

也就是说，当前目录里不是“只有几类 zero-copy 队列”，而是“5 类并发模型，每类都有 origin 和 ZC 两套实现”。

## 2. 接口划分

### 2.1 原版接口

原版队列使用“push/pop 风格”的接口：

- 写侧：`tryPush`、`push`、`tryEmplace`、`emplace`
- 读侧：`tryPop`、`pop`
- 广播队列额外带 `consumerId`
  `tryPop(size_t consumerId, T &out)`、`pop(size_t consumerId, T &out)`、`pending(size_t consumerId)`

这些 `push/pop` 并不是真正的阻塞系统调用，而是基于 `cpuRelax()` / `AdaptiveBackoff` 的自旋等待。

### 2.2 Zero-Copy 接口

ZC 队列统一使用 reservation 风格接口：

- 写侧：`tryAcquireWrite` / `acquireWrite`
- 读侧：`tryAcquireRead` / `acquireRead`
- 发布与释放：`commitWrite` / `commitRead`

使用顺序固定为：

1. producer 调用 `acquireWrite` 或 `tryAcquireWrite` 拿到槽位。
2. producer 直接往槽位里写 payload。
3. producer 调用 `commitWrite` 发布数据。
4. consumer 调用 `acquireRead` 或 `tryAcquireRead` 拿到槽位。
5. consumer 直接读取槽位里的 payload。
6. consumer 调用 `commitRead` 释放槽位。

注意：

- `commitWrite` / `commitRead` 必须和对应的 acquire 在同一线程内配对完成。
- consumer 从槽位拿到的指针或引用，只能在 `commitRead` 之前使用；提交之后不能跨线程或异步保存。
- 广播 ZC 队列同样带 `consumerId`，并保留 `pending(size_t consumerId)` 用于看单个消费者积压。

## 3. 五类模型的实现差异

- `SpscRingBuffer<T>` / `SpscRingBufferZC<T>`
  1 producer + 1 consumer。没有 CAS，也没有 per-slot `sequence`，只有 `head/tail` 和两端缓存游标，热路径最短。
- `MpscRingBuffer<T>` / `MpscRingBufferZC<T>`
  M producers + 1 consumer。producer 通过 CAS 竞争 `tail`，consumer 单独推进 `head`，同步成本介于 `SPSC` 和 `MPMC` 之间。
- `SpmcRingBuffer<T>` / `SpmcRingBufferZC<T>`
  1 producer + N consumers。producer 单独推进 `tail`，consumer 通过 CAS 竞争 `head`，槽位复用依赖 `sequence`。
- `SpmcBroadcastRingBuffer<T>` / `SpmcBroadcastRingBufferZC<T>`
  1 producer + N consumers 广播。每个 consumer 都有自己的读位置，producer 是否能覆盖旧槽位取决于最慢消费者。
- `MpmcRingBuffer<T>` / `MpmcRingBufferZC<T>`
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
  `SpscRingBuffer`、`SpscRingBufferZC`、`MpscRingBuffer`、`MpscRingBufferZC`、
  `SpmcRingBuffer`、`SpmcRingBufferZC`、`SpmcBroadcastRingBuffer`、`SpmcBroadcastRingBufferZC`、
  `MpmcRingBuffer`、`MpmcRingBufferZC`。
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
| `SpscRingBufferZC` | `try` | 169.84 | 5.89 | 19.94 | 50.15 | 21.99 | 45.47 |
| `SpscRingBufferZC` | `blocking` | 162.01 | 6.17 | 17.77 | 56.27 | 18.20 | 54.95 |

#### MPSC

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `MpscRingBuffer` | `try` | 54.86 | 18.23 | 11.14 | 89.77 | 9.14 | 109.44 |
| `MpscRingBuffer` | `blocking` | 58.91 | 16.97 | 20.84 | 47.99 | 8.45 | 118.38 |
| `MpscRingBufferZC` | `try` | 50.68 | 19.73 | 19.96 | 50.10 | 8.03 | 124.51 |
| `MpscRingBufferZC` | `blocking` | 66.44 | 15.05 | 16.54 | 60.46 | 8.05 | 124.27 |

#### SPMC

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `SpmcRingBuffer` | `try` | 26.80 | 37.31 | 19.89 | 50.27 | 23.05 | 43.39 |
| `SpmcRingBuffer` | `blocking` | 32.33 | 30.93 | 33.19 | 30.13 | 23.32 | 42.88 |
| `SpmcRingBufferZC` | `try` | 26.37 | 37.93 | 27.35 | 36.56 | 26.18 | 38.20 |
| `SpmcRingBufferZC` | `blocking` | 14.31 | 69.86 | 31.02 | 32.23 | 30.29 | 33.02 |

#### SPMC_BCAST

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `SpmcBroadcastRingBuffer` | `try` | 47.42 | 21.09 | 13.88 | 72.06 | 13.12 | 76.20 |
| `SpmcBroadcastRingBuffer` | `blocking` | 57.97 | 17.25 | 14.08 | 71.03 | 13.22 | 75.64 |
| `SpmcBroadcastRingBufferZC` | `try` | 102.15 | 9.79 | 17.32 | 57.73 | 18.18 | 54.99 |
| `SpmcBroadcastRingBufferZC` | `blocking` | 105.06 | 9.52 | 18.80 | 53.19 | 17.94 | 55.76 |

#### MPMC

| class | mode | 8B mops | 8B avg ns | 72B mops | 72B avg ns | 128B mops | 128B avg ns |
|---|---|---:|---:|---:|---:|---:|---:|
| `MpmcRingBuffer` | `try` | 21.33 | 46.88 | 9.42 | 106.11 | 7.57 | 132.14 |
| `MpmcRingBuffer` | `blocking` | 42.24 | 23.67 | 9.26 | 107.94 | 5.85 | 170.99 |
| `MpmcRingBufferZC` | `try` | 45.46 | 22.00 | 9.73 | 102.77 | 8.56 | 116.79 |
| `MpmcRingBufferZC` | `blocking` | 42.66 | 23.44 | 9.44 | 105.88 | 6.17 | 162.05 |

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
| `SpscRingBufferZC` | `try` | 84.40 | 26177.40 | 47021.00 | 19.62 | 175.66 | 230.00 | 18.84 | 164.36 | 221.00 |
| `SpscRingBufferZC` | `blocking` | 79.39 | 28508.50 | 59405.00 | 19.38 | 165.44 | 210.00 | 18.43 | 157.91 | 190.00 |

#### MPSC

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `MpscRingBuffer` | `try` | 17.33 | 184776.82 | 329935.00 | 10.21 | 8126.98 | 27994.00 | 5.99 | 2552.98 | 15019.00 |
| `MpscRingBuffer` | `blocking` | 9.28 | 2408.07 | 1393.00 | 10.75 | 49494.57 | 118879.00 | 5.98 | 1028.41 | 1052.00 |
| `MpscRingBufferZC` | `try` | 8.50 | 2196.63 | 1202.00 | 6.66 | 2547.81 | 4839.00 | 5.81 | 1846.91 | 1082.00 |
| `MpscRingBufferZC` | `blocking` | 10.05 | 58688.96 | 142244.00 | 7.47 | 1771.40 | 2495.00 | 6.43 | 950.34 | 1132.00 |

#### SPMC

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `SpmcRingBuffer` | `try` | 15.59 | 220312.71 | 515051.00 | 24.29 | 41969.99 | 125412.00 | 25.44 | 55497.77 | 90494.00 |
| `SpmcRingBuffer` | `blocking` | 13.83 | 253738.64 | 457621.00 | 31.95 | 18794.84 | 55537.00 | 23.04 | 11845.07 | 38574.00 |
| `SpmcRingBufferZC` | `try` | 18.76 | 145747.41 | 206979.00 | 15.91 | 138117.75 | 320727.00 | 22.11 | 10403.07 | 41099.00 |
| `SpmcRingBufferZC` | `blocking` | 10.84 | 293474.39 | 586850.00 | 22.13 | 43296.24 | 119130.00 | 26.70 | 30655.76 | 103179.00 |

#### SPMC_BCAST

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `SpmcBroadcastRingBuffer` | `try` | 43.12 | 67419.59 | 391112.00 | 12.37 | 1663.11 | 3277.00 | 12.41 | 1766.97 | 4389.00 |
| `SpmcBroadcastRingBuffer` | `blocking` | 42.18 | 20933.11 | 138356.00 | 14.01 | 1504.87 | 2475.00 | 12.49 | 1387.95 | 4438.00 |
| `SpmcBroadcastRingBufferZC` | `try` | 53.56 | 95974.90 | 359862.00 | 16.47 | 1432.52 | 1633.00 | 16.53 | 966.24 | 1603.00 |
| `SpmcBroadcastRingBufferZC` | `blocking` | 52.01 | 95127.01 | 394660.00 | 18.50 | 1039.42 | 1713.00 | 16.41 | 870.60 | 1673.00 |

#### MPMC

| class | mode | 8B mops | 8B avg ns | 8B p95 ns | 72B mops | 72B avg ns | 72B p95 ns | 128B mops | 128B avg ns | 128B p95 ns |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `MpmcRingBuffer` | `try` | 18.76 | 13031.94 | 28144.00 | 6.82 | 1762.60 | 881.00 | 6.48 | 1683.65 | 761.00 |
| `MpmcRingBuffer` | `blocking` | 19.34 | 17612.70 | 46269.00 | 8.23 | 1009.47 | 772.00 | 7.87 | 1585.11 | 4499.00 |
| `MpmcRingBufferZC` | `try` | 29.31 | 35722.25 | 47762.00 | 8.50 | 1691.01 | 852.00 | 6.36 | 2090.83 | 1493.00 |
| `MpmcRingBufferZC` | `blocking` | 31.97 | 31681.24 | 47121.00 | 9.18 | 1652.98 | 761.00 | 6.89 | 4253.43 | 22333.00 |

### 6.3 结果总结

- 这次第 6 节里吞吐表和延迟表都已经改成“warmup 1 轮 + 实测 5 轮取中位数”，比之前的单轮采样稳定得多；而且覆盖范围也从 8 个类扩展到了 10 个类，新增了 `MPSC origin / MPSC ZC`。
- `SPSC` 仍然没有“ZC 全面替代 origin”的单边结论。
  `8B` 下 ZC 明显更快；`72B` 两者接近；`128B` 则是 `try` 模式下 ZC 更高、`blocking` 模式下 origin 略高。
- 新增的 `MPSC` 结果显示，它正好处在 `SPSC` 和 `MPMC` 之间。
  `8B blocking` 是 `MpscRingBufferZC` 最快，`72B` 则出现明显 payload 敏感性：`try` 模式偏向 ZC，`blocking` 模式偏向 origin；`128B` 两边吞吐都落在 `6~9 mops` 区间。
- `SPMC` 还是强依赖模式和 payload。
  `8B blocking` 明显是 origin 更强，但 `72B / 128B` 基本转向 ZC 占优，尤其 `128B blocking` 的 `SpmcRingBufferZC` 已经比 origin 高出一截。
- `SPMC_BCAST` 依旧是 ZC 收益最稳定的一类。
  这次 `8B / 72B / 128B` 三档、`try / blocking` 两种模式里，`SpmcBroadcastRingBufferZC` 的吞吐都高于 origin，其中 `8B` 仍然接近翻倍。
- `MPMC` 的吞吐结果这次反而整体偏向 ZC。
  `8B / 72B / 128B` 三档里，`MpmcRingBufferZC` 的 `try / blocking` 吞吐都高于 origin；但延迟表并没有同步给出同样单边的结论，尤其 `8B` 和 `128B blocking` 的 tail 仍然需要单独看。
- 延迟角度看，`72B / 128B` 的 `SPSC` 和大部分 `MPSC / MPMC / SPMC_BCAST` case 已经落回到微秒级甚至亚微秒级 p95，但 `8B` 小 payload 下更容易因为抢占和排队把 tail 放大到几十微秒到几百微秒。
  如果后续要做最终选型，建议继续把 `throughput-only` 作为吞吐排序依据，再把少数候选实现单独做更长时长的延迟重复测试。

## 7. 历史本地测试记录

下面这些结果保留为历史记录，时间是 `2026-04-05`。它们反映的是当时本地实验中对 origin / ZC / shm / blocking-nonblocking 的对比，不等同于“当前 CMake 可直接复现的 benchmark 目标”。

### 7.1 `ringbuffer_origin_vs_zc_bench`

| class | 8B origin/zc | 72B origin/zc | 128B origin/zc |
|---|---:|---:|---:|
| SPSC | 92.86 / 92.60 | 36.00 / 33.68 | 20.16 / 21.29 |
| SPMC | 47.85 / 37.20 | 44.82 / 38.01 | 33.62 / 31.94 |
| SPMC_BCAST | 67.37 / 171.93 | 20.75 / 29.22 | 14.49 / 16.27 |
| MPMC | 35.72 / 8.90 | 28.12 / 9.29 | 26.43 / 9.31 |

### 7.2 `ringbuffer_modes_bench`

| class | 8B try/block | 72B try/block | 128B try/block |
|---|---:|---:|---:|
| SPSC | 127.83 / 121.70 | 33.21 / 32.23 | 20.79 / 20.72 |
| SPMC | 43.43 / 28.85 | 38.13 / 29.68 | 29.15 / 29.14 |
| SPMC_BCAST | 111.97 / 127.77 | 26.11 / 31.79 | 16.42 / 16.71 |
| MPMC | 8.73 / 9.94 | 8.90 / 8.98 | 9.17 / 9.50 |

### 7.3 `spsc_origin_zc_shm_bench`

| payload | origin | zc | shm |
|---|---:|---:|---:|
| 8B | 226.41 | 84.71 | 33.17 |
| 72B | 26.61 | 32.68 | 15.06 |
| 128B | 33.29 | 25.06 | 11.74 |

### 7.4 `spsc_vs_shm_bench --case all --iterations 1000000 --buffer-bytes 536870912`

| case | payload | origin | zc | shm |
|---|---:|---:|---:|---:|
| uint64 | 8B | 92.41 | 256.79 | 32.45 |
| lev2_txn | 136B | 15.20 | 24.72 | 10.52 |
| order_intent | 56B | 19.40 | 45.16 | 16.86 |
| log_record | 512B | 13.70 | 17.88 | 2.55 |

## 8. 历史结果怎么读

- `SPMC_BCAST` 的 ZC 优化在历史测试里收益最明显，尤其小 payload 下更突出。
- `SPSC` 的 ZC 不是所有合成 payload 都稳赢，但在真实业务结构体上有机会更占优。
- `MPMC` 仍然主要受 CAS 和 `sequence` 协调成本约束，zero-copy 不能自动抹掉这部分同步开销。
- `shm` 方案在同进程线程 benchmark 里通常不如进程内 ringbuffer，但它解决的是跨进程共享内存，不是同一个问题域。
