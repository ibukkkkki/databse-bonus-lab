
# Bonus实验：事务系统性能优化挑战


> **本实验在原[RucBase](https://github.com/ruc-deke/rucbase-lab)框架上扩展完成**，提供：
> 1. 一版**设计粗糙、可正确运行**的事务系统
> 2. 一套完整的 **TPC-C benchmark 工具链**
> 3. **故意保留**的性能瓶颈，作为本实验的优化空间
>
> 本实验的目标**不是从零造数据库**，而是**像真实数据库工程师一样定位瓶颈、提出优化、量化收益**。

---

## 目录

1. [项目背景与目标](#1-项目背景与目标)
2. [代码结构与改动概览](#2-代码结构与改动概览)
3. [部署与运行指导](#3-部署与运行指导)
4. [基线性能数据](#4-基线性能数据)
5. [参考优化方向](#5-参考优化方向)

---

## 1. 项目背景与目标

### 1.1 RucBase 简介

RucBase 是中国人民大学数据库实验课的教学数据库框架，覆盖了一个最小化关系数据库的核心模块：存储引擎（buffer pool / record manager）、索引（B+ 树）、查询执行（火山模型）、SQL 解析、事务与并发控制、日志恢复。学生在 4 个 Lab 中分别完成存储管理、索引管理、查询执行、并发控制四大模块。

### 1.2 本实验的定位



本实验在保留原框架的前提下，**最少侵入地补齐了一版可运行的事务系统**，并且**故意保留了大量性能问题**：

| 设计选择 | 优化方向 |
|---|---|
| 表级 S/X 锁 + wait-die 死锁预防（粒度粗，写事务之间互斥） | 进一步可下沉到行级锁 |
| 写操作通过 `write_set` 物理回滚 | 实现更高级的undo操作 |
| 强制走 `SeqScan`，不依赖 B+ 树 | 实现索引 |


### 1.3 当前锁机制：表级 S/X 锁 + wait-die

为了让事务在多线程下既正确又不至于死锁卡死，本实验实现了一版**最小可行的两阶段锁**：

- **粒度**：表级。每张表维护一个共享/排他锁请求队列（`LockManager::lock_table_`，键为 `(txn_id, tab_fd)`）。
- **模式**：S（共享，读）/ X（排他，写）。`SeqScanExecutor` 在 `beginTuple` 时申请 S 锁，`Insert/Update/Delete` 在执行前申请 X 锁；同一事务对同一张表升级 S→X 也走同一个队列。
- **释放时机**：严格 2PL —— 锁全部累计到事务结束（`commit` 或 `abort`）时由 `TransactionManager` 一次性释放，事务执行期间只加锁、不释放。
- **死锁预防**：**wait-die**（见下文）。**没有**死锁检测线程，也没有超时回滚。

> 关键代码：`src/transaction/concurrency/lock_manager.{h,cpp}`、`src/transaction/transaction_manager.cpp`。

#### 1.3.1 wait-die 是什么

wait-die 是经典的**死锁预防**算法（Rosenkrantz, 1978），核心是用事务的开始时间戳 `start_ts` 给所有事务时间排序，让新事务让步于老事务：

因为"被迫等待"这条边只会从老指向年轻，wait-for 图里**永远不会形成环**，从根本上杜绝死锁。代价是**年轻事务会被反复 abort**，这正是基线测试中 abort 率高达 85–89% 的根因（见 4.1）。

---

## 2. benchmark概览

### 2.0 TPC-C 简介

[TPC-C](https://www.tpc.org/tpcc/) 是事务处理性能委员会（Transaction Processing Performance Council）于 1992 年发布、至今仍是衡量 OLTP 数据库性能的**事实标准** benchmark。它模拟一个批发供应商的订单处理业务，包含 9 张表（warehouse / district / customer / history / orders / new_orders / order_line / item / stock）和 5 类事务，按规范权重混合执行：

| 事务 | 权重 | 业务语义 | 读写特征 |
|---|---|---|---|
| **NewOrder** | 45% | 接收一笔新订单（含 5–15 个 item） | 读重 + 写中等，跨多表 |
| **Payment** | 43% | 客户付款，更新仓库 / 区 / 客户余额 | 写重，热点在 warehouse / district |
| **OrderStatus** | 4% | 查询客户最近一笔订单状态 | 只读 |
| **Delivery** | 4% | 后台批量发货（一次处理 10 笔订单） | 写重 |
| **StockLevel** | 4% | 查询近期低库存商品数 | 只读，扫描较多 |

**核心特征**：

- **写密集**：约 92% 的事务包含写操作，是典型的 OLTP 工作负载，能放大并发控制、日志、锁等模块的瓶颈。
- **数据规模可调**：以 `warehouse` 数 W 为基准，每个仓库约 100 MB 数据；其他表行数与 W 成正比，因此 benchmark 既可在单机小规模验证，也可在分布式集群上压测。
- **核心指标 tpmC**：每分钟成功提交的 NewOrder 事务数，**只统计 NewOrder**（其他事务作为背景负载）。tpmC 越高越好。
- **强一致性约束**：规范要求在跑分结束后做一致性检查（如 `D_NEXT_O_ID = max(O_ID) + 1`），任何并发控制 bug 都会被暴露。


### 2.1 TPC-C 工具链文件

```
src/test/tpcc/
├── CMakeLists.txt          # 编译两个可执行文件：tpcc_loader / tpcc_driver
├── tpcc_common.h           # 9 张表的 schema、TPC-C 常量、随机数据生成
├── tpcc_client.h           # 简单的 socket SQL 客户端封装
├── tpcc_loader.cpp         # 灌数据（支持 --scale 缩放）
└── tpcc_driver.cpp         # 5 类事务的并发驱动 + tpmC/latency 统计
```

---

## 3. 部署与运行指导

### 3.1 环境要求

- Linux（已在 CentOS 7 / Ubuntu 20.04 上验证）
- CMake ≥ 3.13
- GCC ≥ 7.3 / Clang ≥ 6.0（C++17）
- flex / bison（构建解析器）
- 磁盘空闲 ≥ 5 GB（scale=1 全量灌库 + golden 备份各占 2.3 GB）

**可以直接跳到[3.6节](#36-三脚本一键复现)，使用脚本完成编译/数据准备/baseline跑测**

### 3.2 编译

```bash
cd /path/to/rucbase-lab
cmake -B build
cmake --build build --target rmdb tpcc_loader tpcc_driver -j$(nproc)
```

编译产物：

```
build/bin/rmdb           # 数据库 server，端口硬编码 8765
build/bin/tpcc_loader    # TPC-C 数据灌入工具
build/bin/tpcc_driver    # TPC-C 性能驱动 + tpmC/latency 统计
```

### 3.3 命令参数说明

#### 3.3.1 `rmdb`（server）

```bash
rmdb <database_name>
```

#### 3.3.2 `tpcc_loader`

```
tpcc_loader [-h <host>] [-p <port>] [-w <W>] [-S <scale>|--scale <scale>] [-s|--skip-data] [-d|--drop]
```

| 短选项 | 长选项 | 含义 | 默认 |
|---|---|---|---|
| `-h` | `--host` | server 地址 | `127.0.0.1` |
| `-p` | `--port` | server 端口 | `8765` |
| `-w` | `--warehouses` | 仓库数 W | `1` |
| `-S` | `--scale` | 行数缩放因子，**反向**：`1`=全量、`10`=1/10、`100`=1/100 | `1` |
| `-s` | `--skip-data` | 只建表+建索引，不灌数据（开发期调试用） | off |
| `-d` | `--drop` | 灌之前先 `DROP TABLE` 旧表（**不是** duration ⚠️） | off |

#### 3.3.3 `tpcc_driver`

```
tpcc_driver [-h <host>] [-p <port>] [-w <W>] [-t <threads>] [-d <seconds>] [-S <scale>]
```

| 短选项 | 含义 | 默认 |
|---|---|---|
| `-h / -p` | server 地址 / 端口 | `127.0.0.1` / `8765` |
| `-w` | 仓库数（**必须**与 loader 一致） | `1` |
| `-t` | 并发客户端线程数 | `1` |
| `-d` | 测试时长（秒） | `30` |
| `-S` | 缩放因子（**必须**与 loader `-S` 一致） | `1` |


输出示例：

```
=========== TPC-C Result ===========
duration:  61.6 s
committed: 340
aborted:   2720
TPS (commit/s):       5.52
tpmC (NewOrder/min):  149.04
latency p50/p95/p99 (us): 514 / 366624 / 4628129
```

### 3.4 数据规模选项

| `-S` | 数据量（实测） | 灌库时间 | 
|---|---|---|
| **100** | ~22 MB | ~1 秒 |
| **10** | ~236 MB | ~30 秒 |
| **1** | ~2.3 GB（含定长 slot 膨胀）| 2–3 分钟 | 

### 3.5 其他说明

**关 server 一律用 `kill -INT`，严禁 `kill -9`**。
   `kill -9` 跳过 `sm_manager_->close_db()`，BufferPool 中的脏页与刚更新的元数据全部不落盘，重启后看起来"整个库被清空了"。

### 3.6 三脚本一键复现

仓库 `scripts/` 下提供了三个脚本，只需要按顺序跑这三条命令：

```bash
cd /path/to/rucbase-lab
bash scripts/01_setup.sh                # 一次性准备（编译 + 灌库 + 制作 golden）
bash scripts/02_run_s10_t4_d60.sh       # 中规模 60 秒测试
bash scripts/03_run_s1_t4_d300.sh       # 全量 5 分钟长压
```
---

## 4. 基线性能数据

### 4.1 主基线

| 配置 | 脚本 | duration | committed | aborted | abort 率 | TPS | **tpmC** | p50 | p95 | p99 |
|---|---|---|---|---|---|---|---|---|---|---|
| **scale=10 / -t 4 / -d 60** | `02_run_s10_t4_d60.sh` | 61.6 s | **340** | **2720** | 88.9% | 5.52 | **149.04** | 514 µs | 367 ms | 4628 ms |
| **scale=1  / -t 4 / -d 300** | `03_run_s1_t4_d300.sh` | 348.9 s | **79** | **455** | 85.2% | 0.23 | **6.11** | 2951 ms | 65870 ms | 65870 ms |

受服务器配置影响，不同同学测出的数值会有些差异。

### 4.2 重要观察

- ⚠️ **abort 率高（85–89%）是 wait-die 在 4 线程冲突下的正常表现**：年轻事务遇到老事务持锁会主动 abort。
- 🔬 **scale=1 长尾极端严重**：p95 = p99 ≈ 66 秒，意味着每隔几十秒就有事务被锁死或扫描死等到 driver 超时；这是后续优化时可以关注的尾延迟指标

---
## 5. 参考优化方向

本章列出当前实现中主要的几个瓶颈点，以及可走的优化路径。**不要求全部完成**，挑 1～2 个方向做到可复现、有理论依据、有性能数据对比即可。

### 5.1 主要瓶颈清单

| 编号 | 瓶颈 | 表现 |
|---|---|---|
| **B1** | 全表扫描 | 任何点查都走 SeqScan，stock/customer 表每查一条记录要扫几万页 | 
| **B2** | 表级锁  | New-Order / Payment 与任意 SeqScan 互斥；wait-die 下年轻事务大量 die |
| **B3** | 锁升级冲突即 die | 读后写场景（如 Payment 先 SELECT 后 UPDATE）会被强制 abort | 
| **B4** | 无 WAL | abort后不能恢复 |

### 5.3 延伸阅读：业界与学术界的优化思路

本节涉及一些前沿优化知识，供感兴趣的同学阅读。本节仅作参考，不做要求。

#### 5.3.1 锁与并发控制协议本身

近 10 年的趋势是**从纯 2PL 走向 MVCC + OCC + 自适应混合**，本实验的"表锁 + wait-die"对应的是 1970 年代 System R 的设计点。可以把下面这张表当作一份索引：

| 协议方向 | 代表工作 | 核心思想 | 与本实验的关系 |
|---|---|---|---|
| **MVCC + Serializable** | Hekaton (SIGMOD'13)、HyPer/Umbra (TUM)、Cicada (SIGMOD'17) | 写不阻塞读，靠 timestamp + validation 决定可见性 | 直接消解 D2 中"读写互斥"导致的 abort 风暴 |
| **OCC + 高效 validation** | Silo (SOSP'13)、TicToc (SIGMOD'16)、MOCC (VLDB'16) | 无中心化锁表，commit 时再检查冲突；TicToc 用动态时间戳避免假冲突 | 提供"abort 率高 ≠ 一定要悲观锁"的另一条思路 |
| **混合 / 自适应协议** | IC3 (SIGMOD'16)、Bamboo (SIGMOD'21)、CormCC (SIGMOD'18) | 不同事务用不同协议；Bamboo 在 2PL 上做 early lock release | 让冲突链上的等待者尽早往前推，对 TPC-C 热点行场景非常对症 |
| **确定性执行** | Calvin (SIGMOD'12)、Aria (VLDB'20)、Caracal (SOSP'21) | 提前定序消除死锁，副作用是丢了交互式事务 | 思路上的"另一极"：根本不让冲突发生 |
| **Range / phantom 处理** | MaaT、ERMIA、Umbra Precision Locks (VLDB'20) | 干净处理 SI/SSI 下的 phantom，而不是用表锁糊过去 | 解释\"为什么纯行锁还不够\"，引出谓词锁/范围锁 |

**死锁预防的演进**：经典的 wait-die / wound-wait（即本实验所用）已知**饥饿、年轻事务被反复重启**。新一些的工作如 **Bamboo (SIGMOD'21)** 通过 early lock release 减少链式等待；**Polyjuice (OSDI'21)** 干脆用 RL 学每次访问的"等/退/跳"策略，在 TPC-C 上同时压过 Silo / Cicada / 2PL。


#### 5.3.2 事务路径上的工程优化

工业数据库这几年真正落到生产的优化，几乎都不在"协议"层，而在**提交路径、索引并发、缓冲池、分布式架构**这些工程位置。

##### (a) 提交路径

| 优化 | 代表落地 | 对本实验的启发 |
|---|---|---|
| **Group commit / pipeline log** | MySQL InnoDB、PostgreSQL、TiDB、OceanBase | 已是商用 DB 标配；本实验目前**根本没写 WAL**，是 D 系列后续的天然扩展 |
| **远端 redo（log-as-a-service）** | Aurora (SIGMOD'17/'18)、Socrates (SIGMOD'19)、PolarDB Serverless (SIGMOD'21) | "log = the database"，把 redo 下推到存储 |
| **Persistent Memory WAL** | SOFORT (HyPer)、Pronto | Optane EOL 后热度下降，但 CXL 内存把这个题目又带回来了 |
| **NIC offload / SmartNIC commit** | XStore (OSDI'20)、LineFS (SOSP'21) | 用 DPU 把 commit fsync 路径搬出 CPU |

##### (b) 索引与缓冲池

- **Learned Index**（Kraska et al., SIGMOD'18）→ ALEX、PGM、LIPP、Updatable LIPP（VLDB'22~24）
- **B+树 → Bw-tree (Hekaton) / OLC-Btree** —— lock-free 或 optimistic latch coupling
- **LeanStore (ICDE'18) / Umbra (CIDR'20)** —— 摆脱 buffer pool latch 瓶颈，page eviction 用 epoch + pointer swizzling
- **缓存替换学习化**：LRB (OSDI'20)、CACHEUS、HALP (NSDI'23)

##### (c) 分布式事务

- **Spanner / TrueTime**（OSDI'12）启发了 CockroachDB、YugabyteDB
- **FaRM / FaRMv2**（SOSP'15、SOSP'19）—— RDMA + 乐观分布式事务
- **DrTM 系列**（SOSP'15、ATC'18、DrTM+H OSDI'22）—— RDMA + HTM
- 国内云厂商（OceanBase / TiDB / PolarDB-X）在 TPC-C 上反复刷榜，主线就是 **MVCC + Paxos/Raft + 分区 2PC**


#### 5.3.3 Learned Concurrency Control



| 工作 | 出处 | 核心思想 |
|---|---|---|
| **Polyjuice** | OSDI'21（MIT） | 把每笔事务的每次访问看作一次"等 / 不等 / abort"决策，用进化策略 + 离线训练学最优策略；TPC-C 上明显优于 Silo / Cicada / 2PL |
| **CormCC / Tebaldi** | SIGMOD'18 / VLDB'19 | 协议自动选择，可看作浅层版 meta-controller：热点表用 OCC、冷表用 2PL |
| **Bamboo** | SIGMOD'21 | 不算严格意义的 learned，但属于"自适应放松 2PL"这一脉 |

**这条线开放的问题**（也是本实验的天然延伸）：

1. **基于归因的事务回滚**：现在 abort 是粗粒度回滚整个事务，能否只回滚冲突子集（partial rollback）？基础工作是 ARIES nested savepoint，但学习型方案还没成形
2. **RL 学习的事务调度**：Polyjuice 之后还没有真正的后续 SOTA，**离线 RL + LLM 提供的领域先验**是一个干净的切口
3. **LLM 驱动的死锁/锁等待诊断**：把 `pg_locks` / `INNODB_LOCK_WAITS` 的时间序列喂给 Agent，让它给出"长事务/索引缺失/隔离级别过高"等可解释定位——工业界（Aurora Performance Insights、PolarDB AutoPilot）已上线，但归因深度仍然不够

> 如果你的 D2 优化最终选择"用某种启发式动态决定加锁/放锁时机"，就可以把它包装成"轻量级 learned CC 雏形"——这比单纯说"我把 wait-die 改成了 wait-die+"在报告里好讲得多。
