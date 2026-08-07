# CdpCore

CdpCore 是驱动与用户态单元测试共用的 after-image Journal 引擎。它通过 `Cdp_STORE` 抽象访问源卷和日志卷，不依赖文件系统。

## 当前数据语义

- 应用写只追加到日志卷，成功后直接完成上层写请求，不再写源卷。
- 当前分支的 `MetaTree` 保存每个卷区间的最新日志位置；读取命中树时取日志 payload，空缺区间取源卷。
- Journal 格式版本为 v12。Record Header 的 `Sequence` 低16位为区域内索引，最高位 `BRANCH` 表示分支记录；新分支固定从新 HeaderRegion 的索引0开始，全局 Sequence 跨分支持续递增。
- 挂载时扫描保留记录，重建分支信息树和当前分支 `MetaTree`；挂载完成前由驱动排队读写。
- 日志使用率达到90%后可启动唯一合并线程。合并仅回填待删除区域中当前分支仍有效的最新值；区域包含继承点时会 tombstone 无效父分支后缀，并只递归删除继承点 Record 已被丢弃的分支。继承自有效 Record 的兄弟分支保留到合并到达其自身区域，且所有被删除 Sequence 均不复用。

## Preview 与 Recovery

- Preview 根据目标时间定位分支，递归包含父分支继承路径并构建 `PreviewTree`；读取命中日志，空缺取源卷。
- Preview 启动时若合并正在运行则返回忙；合并删除 Preview 目标 Record 所在区域时会停止 Preview。
- Recovery 根据目标时间定位父分支和继承点，创建新分支并构建新的完整 `MetaTree`，成功后原子替换旧树。
- Recovery 不回填源卷。`Commit` 仅保留为无写回的兼容确认接口。
- Recovery 构建失败会删除刚创建的空分支记录，保留原 `MetaTree` 并返回 General。

## 主要 API

- 生命周期：`CdpCoreCreate`、`CdpCoreBind`、`CdpCoreDestroy`
- Journal：`CdpCoreFormatJournal`、`CdpCoreMountJournal`
- 写入：`CdpCoreAppendAfterImage`
- 读取：`CdpCoreRead`
- 查询：`CdpCoreQueryTimeRange`、`CdpCoreQueryJournalUsage`、`CdpCoreQueryRecordHeaders`
- Preview：`CdpCorePreviewBegin`、`CdpCorePreviewRead`、`CdpCorePreviewEnd`
- Recovery：`CdpCoreRecoveryBegin`、`CdpCoreRecoveryCommitStep`、`CdpCoreRecoveryCommit`
- 合并：`CdpCoreSetMergeActive`、`CdpCoreCompactOldestRegion`

## 构建与测试

```bat
msbuild CdpCore\CdpCore.Tests.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
x64\Release\CdpCore.Tests.exe
```

测试覆盖初始分支、after-image 写失败、最新区间覆盖、分支继承读取、合并与失败重试、分支树重建、Preview 分支路径、Preview/合并协调及 Recovery 分支切换/回滚。
