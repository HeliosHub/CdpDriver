# 架构设计

## 概述

本项目是一个 Windows **卷过滤驱动**，对受保护卷的写入做 **Copy-on-Write（写前镜像）**：每次写操作先把被覆盖的旧数据追加到独立的 **CDP/Journal 分区**，再把原始写透传到下层设备。磁盘上始终是最新数据；历史 before-image 留在 journal 中，用于：

- **Preview**：按时间点只读重建历史视图；包含时间戳等于目标时间的 COW 记录
- **Recovery**：先准备目标时间视图，再显式提交同步回填；包含时间戳等于目标时间的 COW 记录

配套工具：

| 组件 | 作用 |
|------|------|
| `CdpConsole` | 安装/注册驱动、配置捕获（CMD1/2）、卷句柄读写（CMD3–5）、Preview、查询 journal 时间范围 |
| `CdpCore` | 用户态库：内存模拟源卷+journal，单测 COW/Preview/Recovery（见 `CdpCore/README.md`） |

当前 journal 磁盘格式版本为 **v7**（单 Superblock + 2MB 头区/负载区交替）。Superblock 持久化源卷 GUID；旧版 journal 分区需重新 Format。

## 驱动在设备栈中的位置

```
文件系统驱动 (NTFS 等)
|
本驱动（Volume Upper Filter）
|
卷设备
|
磁盘驱动
```

写路径：`IRP_MJ_WRITE` →（若 `CaptureEnabled`）排队到捕获工作线程 → 读 before-image → `CdpJournalAppend` → 同步转发原写。  
读路径：默认透传；仅在 **Recovery** 阶段对非分页读做历史合成。

## 核心设计思想

### 为什么用 COW Journal，而不是写重定向

早期方案曾把写入重定向到空闲扇区，靠“重启丢弃映射”做还原。当前实现改为：

1. **受保护卷上的数据始终是最新的**——文件系统与应用看到真实落盘内容，无需维护 `$Bitmap` / 扇区映射表。
2. **历史保存在独立分区**——容量、寿命与受保护卷解耦；journal 满时可丢弃最旧记录（环式推进 header region）。
3. **可按时间点回溯**——Preview / Recovery 基于 wall-clock + sequence，而不是“仅重启还原”。

## 卷工作阶段（Phase）

每个源卷 `Cdp_DEVICE_EXTENSION.Phase`：

| 值 | 宏 | 行为 |
|----|-----|------|
| 0 | `Cdp_PHASE_NORMAL` | COW 捕获（若已 CMD1）；读透传 |
| 1 | `Cdp_PHASE_PREVIEW` | 同 Normal 的写捕获；允许 Preview IOCTL |
| 2 | `Cdp_PHASE_RECOVERY` | 一次性同步回填过程中的临时状态 |

约束：

- **全局同时只能有一个 Preview 会话**；`BEGIN_PREVIEW` 要求当前为 Normal，成功后进入 Preview。
- **Recovery 只能从 Normal 进入**；存在 Preview 会话时拒绝 `BEGIN_RECOVERY`。
- `BEGIN_RECOVERY` 只准备历史视图并保持 Recovery；`COMMIT_RECOVERY`
  回填成功后进入 Normal，`CANCEL_RECOVERY` 不回填并进入 Normal。

## Journal 布局（v7）

独立 CDP 分区采用 **2MB 记录头区 + 对应负载区** 交替排列：

```
+------------------+  扇区 0
| Superblock 主    |
+------------------+
| HeaderRegion 0   |  2MB（密排 32B 记录头 + 尾部 RegionLink）
| Payload 0        |  本区内记录的 before-image（紧随其后追加）
+------------------+
| HeaderRegion 1   |  2MB
| Payload 1        |
+------------------+
| …                |
| HeaderRegion n   |  2MB
| Payload n        |
+------------------+
```

- **只有当前 2MB Header 槽用尽**时，才在 `PayloadRegionOff`（或回绕到可用区起点）新开一对 `Header[2MB]+Payload`。
- **Payload 写到分区尾不够**：写游标绕回可用区起点，**不**新开 Header；空间仍不够则丢弃最旧记录腾地方。
- 记录头里的 `FileOffset` 指向该条 payload 的绝对偏移。
- Header 区间用 `RegionLink`（Prev/Next）串成链，Mount / Preview 扫描 / 丢弃最旧记录时使用。

Version = **7**。v6 及更早版本 journal 需重新 Format。

### Superblock

| 字段 | 含义 |
|------|------|
| `LastHeaderRegionOff` | 最新 2MB Header 区起点 |
| `SourceVolumeGuid` | 与该 journal 配对的源卷 Volume GUID |

当前 `PayloadRegionOff` 不再写入 Superblock。Mount 扫描最新 Header 区，以最后一条有效记录的 `FileOffset + DataLength` 向扇区对齐后重建下一 Payload 写入位置；空日志从最新 Header 区末尾开始。

### 记录头（32 字节）

| 字段 | 含义 |
|------|------|
| `WallClock100ns` | 写入时刻 |
| `VolumeOffset` | 源卷字节偏移 |
| `FileOffset` | journal 内 payload 绝对偏移 |
| `DataLength` | before-image 长度 |
| `Sequence` | 单调递增序号 |

## Preview / Recovery 区间树

按 `VolumeOffset`（`Start`）排序的 **AVL 区间树**，节点维护 `MaxEnd` 做重叠剪枝；插入/查找 O(log n)。构建时按 Sequence 升序去重插入，并合并卷/payload 均连续的相邻区间。

## Preview：时间点只读视图

1. **BEGIN**：校验源卷 Phase=Normal、全局无其他 Preview；CAS 进入 Preview；冻结 `SnapshotMaxSequence`；扫 journal 收集匹配 header，按 Sequence 升序插入并去重；再合并卷/payload 均连续的相邻区间；构建期间并发 COW 写入 StagingTree，结束后 Merge（Insert 去重）+ Dedup/Coalesce。
2. **READ**：区间树重叠查询；同字节取最早 `Sequence`；未覆盖空缺用当前卷 live 数据填充。全程持有源卷 `HistoryMutex`，与 COW 捕获互斥。
3. **END**：销毁会话，Phase → Normal。

## Recovery：HistoryTree 回填 + 无效标记

1. **BEGIN / Prepare**：冻结 `SnapshotMaxSequence`；扫 journal 收集 `WallClock100ns >= TargetTime100ns` 的 header；按 Sequence 升序插入并去重；**合并连续区间**（卷偏移与 journal payload 均相邻）。构建期间并发新写只记入 StagingTree。结束后 PunchByStaging，再 DedupEarliest（含 Coalesce）丢掉 Invalid，并保持 Recovery Phase。
2. **Prepared**：非分页读由 HistoryTree + live source 合成；允许新写入，新写覆盖的 HistoryTree 范围失效，因此 Commit 后仍保留新数据。
3. **COMMIT**：遍历 HistoryTree 回填源分区（COW 后写下层；`Sequence` 升序；跳过 `Invalid`），成功后清理并回到 Normal。
4. **CANCEL**：不回填，清理临时树并回到 Normal。
3. **完成**：清理 HistoryTree / StagingTree，自动回到 Normal；BEGIN IOCTL 此时才同步返回。

回填期间的 COW 设 `WritebackActive`，避免把自己刚回填的写再标成 Invalid。

> Preview 与 Recovery 都收集 `WallClock100ns >= TargetTime100ns` 的记录。Preview 的 Staging→PreviewTree 仍是 **Merge 插入**（补扫盘遗漏的 before-image）。Recovery 的 Staging 语义不同：表示“这些区间已被新写覆盖，History 应跳过”，故用 **Punch** 而非 Merge。

## 控制接口（IOCTL）

| IOCTL | 用途 |
|-------|------|
| `IOCTL_Cdp_QUERY_PROTECT_STATUS` | 查询是否有卷已 CMD1 开启捕获（`CaptureEnabled`） |
| `IOCTL_Cdp_SEND_COMMAND` | CMD1 配置捕获 / CMD2 停止 / CMD4 开卷 / CMD5 关卷 |
| `IOCTL_Cdp_READ_SECTORS` | CMD3 按句柄读扇区 |
| `IOCTL_Cdp_BEGIN/READ/END_PREVIEW` | Preview 会话 |
| `IOCTL_Cdp_QUERY_PHASE` | 查询 Phase |
| `IOCTL_Cdp_QUERY_TIME_RANGE` | 查询 journal 最早/最新 COW 记录 WallClock |
| `IOCTL_Cdp_BEGIN_RECOVERY` | 准备 Recovery 历史视图并保持 Recovery Phase |
| `IOCTL_Cdp_COMMIT_RECOVERY` | 同步回填已准备的历史视图，成功后回到 Normal |
| `IOCTL_Cdp_CANCEL_RECOVERY` | 取消已准备的 Recovery，不回填 |

CMD1 参数：`PartitionGuid1`（源卷）、`PartitionGuid2`（journal 分区）、`FormatJournal`（非 0 则 Format，否则 Mount）。

## 关键同步

- **`HistoryMutex`（每源卷）**：COW（读 before-image + Append + 转发原写）与 Preview/Recovery 构建、回填串行化，避免时间线撕裂。
- **`Journal.Lock`**：journal 结构与磁盘元数据。
- **Preview/Recovery `TreeLock`**：AVL 区间树与 Staging 合并。
- **捕获工作线程**：写 IRP 入队，PASSIVE_LEVEL 下执行 COW + 同步转发。

## 捕获启用

首次由控制设备 **CMD1** 格式化/开启 COW（源卷 GUID + journal 分区 GUID），**CMD2** 停止。重启后，驱动在 AddDevice 完成并延迟 30 秒后扫描首扇区 magic，避开 PnP START/Mount Manager 启动关键路径；随后自动挂载 v7 journal，并根据 Superblock 的 `SourceVolumeGuid` 打开源卷、绑定 Core 和重新使能捕获。

## 已知限制

1. Preview / Recovery 依赖 journal 中有足够历史；journal Format 后历史清空。
2. 全局仅允许一个 Preview 会话。
3. Recovery Commit 是同步物理回填，执行期间 `HistoryMutex` 会阻塞该源卷的捕获工作线程。
4. Recovery 阶段的 Paging I/O 当前仍透传 live source；驱动会打印
   `[RECOVERY] paging read bypass` 诊断日志，后续需决定安全的历史合成策略。

## 未来规划

- 捕获配置持久化
- journal 容量告警与策略（保留窗口、优先级）
- 性能剖析与写路径批处理优化
- 评估非 NTFS 卷上的捕获可行性（当前捕获不依赖 `$Bitmap`，但 CdpConsole 卷枚举仍以固定盘为主）
