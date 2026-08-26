# 架构设计

## 1. 总体结构

CdpDriver 同时注册为 `Volume` 与 `DiskDrive` 类 Upper Filter。卷层负责高效识别已绑定保护分区并优先合成读取；磁盘层负责截获最终物理写入，并为绕过卷层的读取提供兜底。两层共享同一个保护上下文和 after-image Journal 视图。

- `CdpDriver`：PnP、卷/磁盘 IRP、自动发现、保护路由、工作线程、drain 与 IOCTL。
- `CdpCore`：当前视图、Preview、Recovery、还原点物化和空间回收协调。
- `CdpJournal`：v15 磁盘格式、Record、分支树、区间树及持久化状态。
- `CdpConsole`：安装、保护配置、查询、Preview、Recovery 和还原点管理。
- `CdpDiskFilter` / `CdpDiskCtl`：独立的最小磁盘层验证工程，主驱动不依赖它们。

磁盘自动发现可创建不附着设备栈的 `SOURCE` 上下文。已启动卷按磁盘号和分区起始位置绑定到对应保护上下文，卷 I/O 不需要遍历全部分区。

## 2. 保护路由

每个物理磁盘维护按绝对范围排序的保护路由：

1. 先检查最近命中的分区，连续 I/O 通常一次比较完成定位。
2. 未命中时对有序路由做二分查找，复杂度为 `O(log N)`。
3. 只有路由容量异常不足时才启用全局扫描兜底；正常未命中直接下发。

关闭保护时先停止新路由引用，再等待已经取得的磁盘 I/O 引用归零，最后销毁 Core 和 Journal 绑定。

## 3. 写入路径

卷层 WRITE 不接管，直接发往下一层。请求到达磁盘 Upper Filter 后按绝对物理范围匹配保护分区：

1. Dispatch 保留原始 IRP 和绝对磁盘偏移，排入该磁盘的 FIFO 工作队列。
2. Worker 在 `PASSIVE_LEVEL` 获取源保护上下文，并用 `HistoryMutex` 串行化写入顺序。
3. `CdpCoreAppendAfterImage` 依次持久化 payload 与 Record Header。
4. Journal 成功后更新当前分支 `MetaTree`，并直接完成应用 IRP；写入不会到达源分区。
5. Journal 失败时应用写失败，不允许绕过保护写入源分区。

保护关闭过程进入 `DRAINING` 后，已经到达的普通应用写仍由磁盘 FIFO 串行处理，不使用卷层 gate，也不依靠线程或范围识别内部写回。

## 4. 读取路径

- 卷层 READ：已绑定保护卷优先进入合成读取，卷相对偏移只在入口转换一次为绝对磁盘偏移。
- 磁盘层 READ：处理直接到达磁盘栈、没有经过卷层的读取，作为完整性兜底。
- 合成规则：命中 `MetaTree`/`PreviewTree` 的区间读取 Journal after-image；未覆盖区间读取源分区基础数据。

保护开启期间抑制 `DeviceDsmAction_Trim`，防止源分区基础数据被底层回收。保护关闭后 READ、WRITE 和 TRIM 均正常下发。

## 5. Drain 与磁盘直写

停止保护时，drain 将 `MetaTree` 中当前视图逐段写回源分区：

1. Phase 从 `GENERAL` 原子切换到 `DRAINING`，停止合并并等待正在提交的重定向写完成。
2. `CdpCoreDrainOneMetaRangeWithWriter` 每次返回一个仍需写回的绝对磁盘范围。
3. 驱动直接针对物理磁盘过滤设备的 `LowerDeviceObject` 构造同步 WRITE IRP。
4. IRP 使用绝对磁盘偏移，并在下一层 `IO_STACK_LOCATION::Flags` 设置 `SL_FORCE_DIRECT_WRITE`。
5. 写入成功后从当前覆盖树移除对应范围；全部范围完成后才清除保护状态。

该 IRP 从磁盘过滤层下方发起，不经过卷栈，也不会重新进入本驱动的卷层或磁盘层，因此不需要 IRP 私有标记、线程识别或 volume gate。还原点物化复用同一绝对磁盘写回器。

## 6. Journal v15

Journal 由一个 Superblock 和循环排列的 `1 MiB HeaderRegion + PayloadRegion` 组成。HeaderRegion 最后 32 字节为 `Cdp_HEADER_REGION_LINK`，其余包含 32767 个 32 字节 Record 槽。

普通 Record 保存时间、源偏移、payload 偏移和长度。`Sequence` 低 16 位为区域内索引，最高位 `0x80000000` 表示分支 Record。运行时全局序号为：

```text
RegionLink.StartSequence + (Header.Sequence & 0xFFFF)
```

PayloadRegion 达到 Journal 容量的 `1/10` 或 HeaderRegion 写满时切换区域。普通 append 不逐次更新 Superblock；区域切换、Recovery/还原点标记、凭据和格式化等状态变化才更新。

## 7. 分支、Preview 与 Recovery

格式化时创建根分支 1。新分支固定从新 HeaderRegion 的索引 0 开始，全局 Sequence 跨分支持续递增。挂载扫描保留 Record，重建分支树及当前分支 `MetaTree`。

- Preview 按目标时间构建只读 `PreviewTree`，不改变当前分支。
- Recovery 是分支切换：确定父分支与继承点、追加新分支 Record、构建并原子发布新 `MetaTree`，不写回源分区。
- 重启 Recovery 先持久化意图；下次启动构建目标视图，恢复后的第一笔受保护写在追加 payload 前持久化延迟创建的分支。

Preview 与 Recovery 互斥；Preview 构建期间不允许启动合并。

## 8. 持久还原点

设置还原点时，驱动把目标时间视图通过磁盘直写器物化到源分区，然后在 Superblock 保存持久标记。重启自动发现看到该标记后：

1. 不扫描已经无关的旧 Record 历史。
2. 直接把已物化源分区作为当前基础视图。
3. 第一笔受保护写入前重置旧历史并创建新的根分支，然后正常追加 after-image。

还原点不会自动过期，只能显式删除；启动后尚未完成首次受保护写时不支持删除。

## 9. 空间合并

Journal 使用率达到 90% 时启动唯一合并线程。合并把最旧区域中当前分支仍有效的最新值写回源分区，再清理对应覆盖与不可达分支数据。跨区域删除使用 tombstone，已分配 Sequence 不复用。任一步失败都会停止本轮合并并保留可重试状态。

持久还原点存在时禁用合并，避免改变已经物化的源分区基线。

## 10. 主要同步对象

| 对象 | 保护范围 |
|---|---|
| `CaptureConfigMutex` | 保护配置、自动发现和 Source/Journal 对象图变更 |
| `HistoryMutex` | 单源 Journal append、drain、还原点物化及树发布顺序 |
| `Journal.Lock` | Journal 游标、区域链和持久元数据 |
| `TreeLock` | `MetaTree`、`PreviewTree`、Phase 和延迟分支/历史重置状态 |
| 磁盘 FIFO Worker | 最终物理 I/O 的到达顺序与应用 IRP 完成顺序 |
| `DiskIoOutstanding` | 路由引用与关闭保护之间的生命周期屏障 |

## 11. 验证边界

用户态单元测试覆盖 after-image、挂载重建、区间覆盖、分支继承、Preview、Recovery、持久还原点、drain/物化回调及合并失败重试。PnP 时序、真实 Paging MDL、磁盘过滤栈、`SL_FORCE_DIRECT_WRITE` 和启动盘关闭保护仍需在虚拟机及目标物理机做集成验证。
