# CdpCore

CdpCore 是驱动与用户态单元测试共用的 after-image Journal 引擎。它通过 `Cdp_STORE` 抽象访问源卷和日志卷，不依赖文件系统。

## 当前数据语义

- 应用写只追加到日志卷，成功后直接完成上层写请求，不再写源卷；普通追加不依赖驱动层 `HistoryMutex`，由 Journal 预留锁、publish ticket 与 `TreeLock` 分别负责位置分配、发布顺序和树更新。
- 当前分支的 `MetaTree` 保存每个卷区间的最新日志位置；读取命中树时取日志 payload，空缺区间取源卷。
- Journal 格式版本为 v15。Record Header 的 `Sequence` 低16位为区域内索引，最高位 `BRANCH` 表示分支记录；新分支固定从新 HeaderRegion 的索引0开始，全局 Sequence 跨分支持续递增。
- 普通挂载扫描保留记录，重建分支信息树和当前分支 `MetaTree`；启动自动发现会在文件系统挂载源卷前完成保护对象图发布。
- 日志使用率达到90%后可启动唯一合并线程。合并仅回填待删除区域中当前分支仍有效的最新值；区域包含继承点时会 tombstone 无效父分支后缀，并只递归删除继承点 Record 已被丢弃的分支。继承自有效 Record 的兄弟分支保留到合并到达其自身区域，且所有被删除 Sequence 均不复用。

## Preview 与 Recovery

- Preview 根据目标时间定位分支，递归包含父分支继承路径并构建 `PreviewTree`；读取命中日志，空缺取源卷。
- Preview 启动时若合并正在运行则返回忙；合并删除 Preview 目标 Record 所在区域时会停止 Preview。
- Recovery 根据目标时间定位父分支和继承点，创建新分支并构建新的完整 `MetaTree`，成功后原子替换旧树。
- 重启 Recovery 在启动挂载阶段只构建并发布目标 `MetaTree`，不执行日志写入；新分支由恢复后的第一笔应用写在追加 Payload 前持久化。待分支存在期间禁止合并。
- Recovery 不回填源卷。`Commit` 仅保留为无写回的兼容确认接口。
- Recovery 构建失败会删除刚创建的空分支记录，保留原 `MetaTree` 并返回 General。

## Drain 与持久还原点

- `CdpCoreDrainOneMetaRangeWithWriter` 每次选择一个当前覆盖范围，通过调用方提供的绝对偏移 writer 写回，成功后从 `MetaTree` 删除该覆盖。
- `CdpCoreMaterializeTimeWithWriter` 构建指定时间视图，并通过同一类 writer 将完整目标视图物化到源存储。
- 实际内核 writer 由驱动实现：直接向物理磁盘过滤层下方发送带 `SL_FORCE_DIRECT_WRITE` 的绝对偏移 WRITE；CdpCore 不依赖设备栈细节。
- 持久还原点启动时可跳过旧 Record 扫描，直接使用已物化源数据；第一笔 after-image append 前重置旧历史并建立新根分支。

## 主要 API

- 生命周期：`CdpCoreCreate`、`CdpCoreBind`、`CdpCoreDestroy`
- Journal：`CdpCoreFormatJournal`、`CdpCoreMountJournal`
- 写入：`CdpCoreAppendAfterImage`
- 读取：`CdpCoreRead`
- 查询：`CdpCoreQueryTimeRange`、`CdpCoreQueryJournalUsage`、`CdpCoreQueryRecordHeaders`
- Preview：`CdpCorePreviewBegin`、`CdpCorePreviewRead`、`CdpCorePreviewEnd`
- Recovery：`CdpCoreRecoveryBegin`、`CdpCoreRecoveryCommitStep`、`CdpCoreRecoveryCommit`
- Drain/物化：`CdpCoreDrainOneMetaRangeWithWriter`、`CdpCoreMaterializeTimeWithWriter`
- 持久还原点：`CdpCorePreparePersistentRestoreBoot`、`CdpCoreCancelPersistentRestoreBoot`
- 合并：`CdpCoreSetMergeActive`、`CdpCoreCompactOldestRegion`（自动模式按 90% 阈值循环；手动模式跳过阈值回收一个最旧 RR，并在该 Core 回收事务内清理由失效分支产生的连续 tombstone RR）

## 构建与测试

```bat
msbuild CdpCore\CdpCore.Tests.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
x64\Release\CdpCore.Tests.exe
```

测试覆盖初始分支、after-image 写失败、最新区间覆盖、分支继承读取、合并与失败重试、分支树重建、Preview 分支路径、Preview/合并协调、Recovery 分支切换/回滚、重启恢复的延迟分支持久化，以及 drain/目标时间物化和持久还原点的延迟历史重置。
