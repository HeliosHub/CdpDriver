# CdpCore

CdpCore 是驱动和用户态单元测试共用的 COW / Preview / Recovery 引擎。它不直接依赖文件系统，通过 `Cdp_STORE` 抽象访问源卷和 Journal。

## 与驱动的关系

| 组件 | 职责 |
|---|---|
| `CdpCore` | Phase、COW Append、Preview/History/Staging 区间树、Recovery CommitStep |
| `CdpJournal` | Journal v11 的 Format、Mount、Append、扫描、查询和区域淘汰 |
| `CdpDriver` | IRP/IOCTL、设备 Store、工作线程、Paging I/O、启动 Gate、认证 |
| `CdpCore.Tests` | 使用内存 Store 执行确定性和故障注入测试 |

内核路径通过 `CdpCoreBind(SourceStore, MountedJournal, SourceVolumeGuid, ...)` 绑定真实源卷 Store 和已经挂载的 Journal；用户态测试通过 `CdpCoreCreate(SourceStore, JournalStore, ...)` 创建两块内存 Store。驱动已经实际使用 CdpCore，不是预留的未来后端。

## 主要 API

声明位于 `include/cdp_core.h`：

- 生命周期：`CdpCoreCreate`、`CdpCoreBind`、`CdpCoreDestroy`
- Journal：`CdpCoreFormatJournal`、`CdpCoreMountJournal`
- COW：`CdpCoreWrite`、`CdpCoreCaptureAppend`
- 查询：`CdpCoreQueryTimeRange`、`CdpCoreQueryJournalUsage`、`CdpCoreQueryRecordHeaders`
- Preview：`CdpCorePreviewBegin`、`CdpCoreRead`、`CdpCorePreviewEnd`
- Recovery：`CdpCoreRecoveryBegin`、`CdpCoreRecoveryCommitStep`、`CdpCoreRecoveryCommit`、`CdpCoreRecoveryCancel`

`CdpCoreCaptureAppend` 只完成 before-image 和 Journal Append，原始应用写由驱动转发。`CdpCoreWrite` 是用户态测试使用的完整 COW 写路径。

`CdpCoreRecoveryCommitStep` 每次至多处理一个有效 History 节点。驱动在相邻 Step 之间释放 `HistoryMutex`，允许新写进入并 Punch 尚未回填的重叠范围；用户态 `CdpCoreRecoveryCommit` 循环复用同一 Step 实现。

## 构建和运行测试

```bat
msbuild CdpCore\CdpCore.Tests.vcxproj /p:Configuration=Release /p:Platform=x64
x64\Release\CdpCore.Tests.exe
```

测试覆盖的重点包括：

- COW before-image、写失败和分配失败传播
- Preview/Recovery 在构建前、构建中、Prepared 和 Commit 阶段的新写
- HistoryTree 的包含、相交、左右裁剪、节点拆分及 AVL 摘要更新
- PreviewTree Merge 与 Recovery Staging Punch 的不同语义
- 非扇区对齐历史读取和源数据补洞
- 逐节点 Commit、新写交替、回填 COW、`BACKFILL`、`WritebackActive` 和失败恢复
- Journal v11 Format/Mount、低16位索引/高16位 Flags、未写满 HeaderRegion、1 MiB 区域、payload `1/10` 切区
- 区域淘汰、环形回绕、最后 Record 定位和 64 位全局 Sequence
- oldest 时间钳制、Record 分页 Generation、使用量统计
- Store 最大传输限制和分块回退

内核专属行为（PnP 时序、真实 Paging MDL 映射、启动全卷 Gate、驱动签名）不能只靠 CdpCore 用户态测试证明，仍需在虚拟机和目标系统上做集成测试。
