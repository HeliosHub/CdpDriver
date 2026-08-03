# 架构设计

## 1. 总体结构

CdpDriver 是 Volume Upper Filter。写入先在 PASSIVE_LEVEL 的捕获工作线程中读取源卷 before-image，追加到独立 Journal，再同步提交原始写 IRP。源卷保存实时数据，Journal 保存历史数据。

```text
文件系统（NTFS 等）
        │
CdpDriver（Volume Upper Filter）
        │
卷设备 / 磁盘驱动

          COW payload + metadata
        └────────────────────────> 专用 Journal 卷
```

| 组件 | 职责 |
|---|---|
| `CdpDriver` | PnP、IRP/IOCTL、启动 Gate、工作线程、真实设备 Store、认证 |
| `CdpCore` | COW、Preview/Recovery Phase、区间树、逐节点 Commit |
| `CdpJournal` | v11 磁盘布局、Append/Mount/扫描/淘汰、Superblock |
| `CdpConsole` | 安装、配置、查询、Preview/Recovery、密码控制 |
| `CdpCore.Tests` | 使用内存 Store 对核心算法做用户态单元测试 |

## 2. Phase 状态机

每个受保护源卷有独立 Phase：

| 值 | 名称 | 行为 |
|---|---|---|
| 0 | `Cdp_PHASE_GENERAL` | 正常 COW，普通读取透传 |
| 1 | `Cdp_PHASE_PREVIEW` | 正常 COW；允许 Preview IOCTL 读取历史视图 |
| 2 | `Cdp_PHASE_RECOVERY` | 读取历史视图；允许新写并 Punch 尚未回填的重叠历史 |

全局只允许一个 Preview 会话。Preview 与 Recovery 互斥。Recovery Begin 只准备视图；Commit 或 Cancel 成功后回到 General。

## 3. 写入、TRIM 与读取

### 3.1 普通写入

```text
IRP_MJ_WRITE
  → 入捕获队列
  → 获取 HistoryMutex
  → 读取源卷 before-image
  → CdpJournalAppend
  → 根据 Phase 更新 Staging/HistoryTree
  → 同步提交原始写 IRP
  → 释放 HistoryMutex
```

捕获或 HistoryTree Punch/拆分失败会让该写 IRP 失败，并记录 RecoveryFailureStatus；不会让写入继续后再用“整节点失效”掩盖错误。

### 3.2 TRIM

当保护开启且收到 `IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES / DeviceDsmAction_Trim` 时，驱动返回成功但不把 TRIM 发给下层。这样删除文件不会使尚未被覆盖的历史扇区被设备清除，也避免删除大量文件时立即产生同等规模的 Journal payload。簇被复用后的真实写仍由 `IRP_MJ_WRITE` 捕获。停止保护后 DSM/TRIM 正常透传。

### 3.3 读取

- General：直接下发源卷。
- Preview：源卷正常读不改变；`READ_PREVIEW` 通过 PreviewTree + live source 合成。
- Recovery：普通和 Paging Read 都通过 HistoryTree + live source 合成。
- 非扇区对齐的 Preview/Recovery 读取先扩大到扇区对齐范围，完成历史覆盖后再返回原请求子区间。
- Recovery Paging I/O 在专用工作线程中映射 MDL 并合成；映射或合成失败时返回错误，不回退为 live source 透传。

## 4. Journal v11 磁盘布局

```text
+-------------------------+  offset 0
| Superblock（占一扇区）  |
+-------------------------+
| HeaderRegion 0（1 MiB） |
| PayloadRegion 0         |
+-------------------------+
| HeaderRegion 1（1 MiB） |
| PayloadRegion 1         |
+-------------------------+
| ...                     |
+-------------------------+
```

### 4.1 Record Header

磁盘 Record Header 固定 32 字节：

| 字段 | 大小 | 含义 |
|---|---:|---|
| `WallClock100ns` | 8 | 本地 wall-clock，FILETIME epoch |
| `VolumeOffset` | 8 | 源卷字节偏移 |
| `FileOffset` | 8 | payload 在 Journal 卷上的绝对字节偏移 |
| `DataLength` | 4 | before-image 有效长度 |
| `Sequence` | 4 | 低16位为 HeaderRegion 内索引；高16位为 Record Flags |

每个 1 MiB HeaderRegion 最后 32 字节是 `Cdp_HEADER_REGION_LINK`：

| 字段 | 含义 |
|---|---|
| `PrevRegionOff` | 上一个 HeaderRegion 偏移；单区时可指向自身 |
| `NextRegionOff` | 下一个 HeaderRegion 偏移；最新区可指向自身 |
| `StartSequence` | 本区第一条 Record 的 64 位全局序号 |
| `Reserved` | 固定写 0 |

当前定义 `Cdp_JOURNAL_RECORD_FLAG_BACKFILL = 0x80000000`，其余高位 Flag 保留。运行时全局 Sequence 为 `StartSequence + (Header.Sequence & 0xFFFF)`；当前 HeaderRegion 只有32767个可用 Record 槽，低16位足够表示全部区域内索引。

### 4.2 Superblock

Superblock 关键字段：

| 字段 | 含义 |
|---|---|
| `Magic` / `Version` / `SectorSize` | 格式识别与校验；当前 Version=11 |
| `PartitionSize` | 格式化时的 Journal 容量 |
| `LastHeaderRegionOff` | 最新 HeaderRegion 偏移 |
| `SourceVolumeGuid` | 与该 Journal 配对的源卷 |
| `Flags` | Recovery Pending、Credential Configured 等标记 |
| `RecoveryTargetTime100ns` | 重启恢复目标时间 |
| `Credential` | KDF、Salt、Verifier、CredentialId、AuthEpoch |
| `Crc32c` / `RecoveryCrc32c` / `MetadataCrc32c` | 基础字段、恢复意图和扩展元数据校验 |

普通 Append 不更新 Superblock，只持久化并 Flush payload 与 Record Header。新建 HeaderRegion、设置/清除 Recovery 意图、修改凭据、Format/Close 等才更新 Superblock；失败时保留 `SuperblockDirty` 供后续 Append 重试。

### 4.3 区域切换、Mount 与淘汰

- Header 槽用尽，或当前 PayloadRegion 跨度将超过 Journal 容量 `1/10` 时，新建 HeaderRegion。
- 因 Payload 阈值可提前切区，HeaderRegion 可以未写满。
- 后继区域存在时，当前区 Record 数可由相邻 `StartSequence` 差得到。
- 最新区域未写满时，Mount 只扫描最新 HeaderRegion 找到最后一条有效 Record，并以 `FileOffset + DataLength` 对齐后重建写游标；不需要扫描其他区域。
- 空间不足时整区淘汰 oldest HeaderRegion。淘汰边界由 RegionLink 和最后一条 Record 计算，只读取必要扇区，不读取整个 1 MiB HeaderRegion。

## 5. Preview 构建与读取

Preview/Recovery 共用按 `VolumeOffset` 排序的 AVL 区间树。节点维护 `MaxEnd` 以加速重叠查询，并维护 `MinValidSequence` 以选择下一回填节点。

Preview Begin 冻结 `SnapshotMaxSequence`，从最新 HeaderRegion 向旧区域单遍扫描；每读到一个符合 `WallClock100ns >= TargetTime100ns` 的 Record 就直接覆盖插入 PreviewTree，不保存 Header 数组，也不二次读取区域。若当前 Record 带 `BACKFILL`，则不应用时间停止条件：该 Record 仍入树并继续向前扫描，直到普通 Record 满足停止条件或到达 oldest。构建期间的新 COW 进入 StagingTree；构建结束后合并到 PreviewTree，补上扫描快照之后的 before-image。

Preview Read 先查询 PreviewTree，再用当前源卷数据填充空缺。相同字节范围选更早的 Sequence，得到目标时间视图。

## 6. Recovery 全阶段

### 6.1 Begin

1. 将源卷应用层读写排入队列，避免历史树尚未完成时观察到半构建视图。
2. 冻结 `SnapshotMaxSequence`，从新到旧单遍扫描 Journal，读取 Record 时直接构建 HistoryTree。
3. 构建期间的新写在完成 COW 后记入 StagingTree。
4. 扫描结束后用 StagingTree Punch HistoryTree 的重叠字节；这表示这些范围在 Begin 期间已经产生了更新，不能再被旧历史覆盖。
5. 保持 Recovery Phase，Begin 返回并放行排队 I/O。

请求时间早于 oldest 时，入口把有效目标时间提升为 oldest。Preview、普通 Recovery 和重启 Recovery 规则一致。

### 6.2 Prepared 与并发新写

Recovery 读取始终先查询 HistoryTree，再从 live source 填充空缺。新写仍执行 COW，然后在 `HistoryMutex` 下 Punch 尚未回填 HistoryTree 的重叠部分。部分重叠会把节点裁剪或拆成左右片段，并重新平衡 AVL 树、刷新 `MaxEnd/MinValidSequence`。内存分配或调整失败会使 Recovery 失败。

Recovery Begin 开始后，所有应用新写 COW 都设置 `BACKFILL`。构建期正常进入 StagingTree，Prepared/Commit 期间正常 Punch 尚未回填的重叠 History；Flag 只改变后续目标时间扫描语义，不改变当前 Recovery 的并发写处理。

### 6.3 Commit

驱动反复调用 `CdpCoreRecoveryCommitStep`：

1. 按 `MinValidSequence` 找到最早的有效节点。
2. 读取该节点 payload。
3. 回填前先捕获目标范围当前源数据，追加带 `BACKFILL` 的 COW Record，再通过 Source Store 写入历史 payload。
4. 成功后把该节点标为 Invalid，并刷新树摘要。
5. 每个节点处理完成就释放外部 `HistoryMutex`，让新写有机会执行并 Punch 后续节点。

回填时设置 `WritebackActive`，因此回填自身的 backfill COW 只写 Journal，不参与、不 Punch 当前 HistoryTree。树为空后释放 HistoryTree/StagingTree，清除目标时间和失败状态，回到 General。Cancel 不写源卷，只销毁临时树并回到 General。

## 7. 启动自动发现与重启恢复

驱动使用 Boot Driver Reinitialization 回调，在启动卷枚举完成后开始完整分类，而不是固定延时 30 秒。每个新启动卷先关闭 `AutoDiscoveryGate`；读写工作线程在 Gate 上等待，直到该卷被确认并处理。

完整扫描会识别 Journal magic、挂载 v11 Journal、根据 `SourceVolumeGuid` 配对源卷并恢复保护。普通源卷或没有对应 Journal 的卷在分类完成后放行。

若 Superblock 存在 Recovery Pending：

1. 自动激活 Source/Journal 配对并执行 Recovery Begin。
2. Begin 成功、历史视图一致后打开源卷 Gate。
3. 自动逐节点 Commit；启动后的新写可与 Commit 交替并 Punch 重叠历史。
4. Commit 成功后清除 Pending 标记并回到 General。
5. Begin/Commit 失败时记录错误并保留 Pending，但打开 Gate，避免本次启动永久阻塞。

## 8. 同步对象

| 对象 | 保护范围 |
|---|---|
| `HistoryMutex` | 单个源卷的 COW、树构建、历史读取和单节点回填 |
| `Journal.Lock` | Journal 运行时状态和磁盘元数据 |
| `TreeLock` | PreviewTree、HistoryTree、StagingTree 及摘要字段 |
| 捕获工作线程 | 在 PASSIVE_LEVEL 完成 before-image + Append + 原写转发 |
| Recovery Read 线程 | 安全处理 Recovery 普通/Paging Read 和启动 Gate 等待 |
| `AutoDiscoveryGateEvent` | 卷识别及自动 Recovery Begin 完成前阻塞启动 I/O |

## 9. 控制接口

| IOCTL | 用途 |
|---|---|
| `IOCTL_Cdp_SEND_COMMAND` | CMD1 配置/开启保护，CMD2 停止保护 |
| `IOCTL_Cdp_QUERY_PROTECT_STATUS` | 查询是否存在受保护卷 |
| `IOCTL_Cdp_BEGIN/READ/END_PREVIEW` | Preview 会话 |
| `IOCTL_Cdp_QUERY_PHASE` | 查询源卷 Phase、Journal GUID、Recovery 目标 |
| `IOCTL_Cdp_QUERY_TIME_RANGE` | 查询 oldest/newest Record 时间 |
| `IOCTL_Cdp_QUERY_JOURNAL_USAGE` | 查询容量、元数据、payload 已用/剩余、Record 数 |
| `IOCTL_Cdp_QUERY_JOURNAL_RECORDS` | 分页读取 Record 元数据，不返回 payload |
| `IOCTL_Cdp_BEGIN_RECOVERY` | 普通 Begin，或仅持久化 ON_REBOOT 意图 |
| `IOCTL_Cdp_COMMIT_RECOVERY` | 同步逐节点回填 |
| `IOCTL_Cdp_CANCEL_RECOVERY` | Cancel 或清除尚未执行的重启意图 |
| `IOCTL_Cdp_AUTHENTICATE` | 在当前控制句柄上认证 |
| `IOCTL_Cdp_QUERY_CREDENTIAL` | 查询共享凭据状态、Journal 数和 AuthEpoch |
| `IOCTL_Cdp_CHANGE_PASSWORD` | 更新匹配当前共享凭据的已挂载 Journal；失败时尝试回滚 |
| `IOCTL_Cdp_QUERY_VERSION` | 查询驱动、Build 和 Journal 版本 |

Record 列表采用 `StartIndex + ExpectedGeneration` 分页。保留集合变化时 Generation 改变，调用方应从第一页重新枚举，避免拼接不同快照。

## 10. 日志与限制

`Cdp_LOG` 在 Release/Debug 都保留；`Cdp_DBG` 只在 Debug 生效。因此高频 `[COW-TRACE]` 不会进入 Release，关键 `[RECOVERY]`、`[AUTO-CDP]`、错误日志仍可用于现场诊断。

主要限制：

- 只支持 Journal v11，不迁移旧开发格式。
- 全局只允许一个 Preview；Preview 与 Recovery 互斥。
- 空间以 HeaderRegion 为粒度淘汰，可能同时失去同区多条历史。
- Begin 会阻塞源卷 I/O；Commit 会持续占用源卷和 Journal 带宽。
- Paging Read 无法安全映射 MDL 时失败，不允许绕过时间点视图。
