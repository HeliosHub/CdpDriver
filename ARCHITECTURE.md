# 架构设计

## 1. 总体结构

CdpDriver 只注册为 Windows `Volume` 类 Upper Filter，并排在 `volsnap` 下层。驱动不附着 `DiskDrive` 栈，也不依靠磁盘层读写兜底。

- `CdpDriver`：卷 PnP、自动发现、卷读写 FIFO、drain 与 IOCTL。
- `CdpCore`：当前视图、Preview、Recovery、还原点和空间回收。
- `CdpJournal`：v19 Journal、Record、分支树、区间树及持久化状态。
- `CdpConsole`：安装、保护配置、查询、Preview、Recovery 和还原点管理。

每个受保护卷的真实 Volume 过滤设备扩展就是唯一保护上下文。Core、MetaTree 和 Journal Record 的源偏移全部以源卷起点为 0，不再保存物理磁盘绝对偏移。

## 2. 自动发现

Volume `START_DEVICE` 下发成功后，驱动从卷栈查询磁盘号、当前分区及物理相邻的下一分区。驱动按磁盘号临时引用物理磁盘设备，仅作为 Journal 后端客户端，在下一分区的绝对起点读取 Superblock；它不会创建或附着磁盘过滤设备。

- 下一分区没有 CDP Magic：返回 `STATUS_NOT_FOUND`，该卷按未保护卷启动。
- 已识别 CDP Magic，但版本、校验、布局、源卷身份或恢复过程失败：卷 START fail-closed。
- Superblock 有效：在 START 返回前挂载 Journal、创建卷相对源 Store、启动卷 FIFO Worker 并发布保护状态。

因此自动发现不依赖注册表中的保护清单。注册表只用于 Windows 类过滤器安装顺序。

## 3. 普通读写与 Flush

受保护卷的 READ、WRITE 和 FLUSH 排入同一个卷 FIFO：

1. WRITE 把卷相对偏移的 after-image payload 和 Record Header 持久化到 Journal，发布 MetaTree 后完成原 IRP；不会写入源卷。
2. READ 先查询 MetaTree。未覆盖部分从源卷下层按卷相对偏移读取，覆盖部分由 Journal after-image 合成。
3. FLUSH 等待此前 FIFO 写完成，先刷新 Journal payload/metadata 后再刷新源卷下层。
4. Journal 或工作线程不可用但保护仍已发布时，读写和 Flush 一律失败，不允许绕过保护透传。

保护期间抑制 `DeviceDsmAction_Trim`，避免源卷基线被回收。未保护卷的请求直接发往下一层。

## 4. Drain、合并和还原点回填

停止保护进入 `DRAINING` 后，Core 逐段返回仍需物化的卷相对范围。驱动向源 Volume 的 `LowerDeviceObject` 发送带 `SL_FORCE_DIRECT_WRITE` 的同步 WRITE，成功后从 MetaTree 移除对应覆盖；全部范围成功后才撤销保护。

普通空间合并、设置/删除还原点时的物化和恢复相关回填复用同一个卷相对写回器。它们不经过本过滤设备，因而不会再次进入保护路径。任何写回失败都会保留可重试状态。

## 5. Preview、Recovery 与分支

- Preview 按目标时间构建独立只读 `PreviewTree`，不会替换当前 MetaTree。
- Recovery 确定父分支和继承点，构建并原子发布新的当前视图，不回填源卷。
- 重启 Recovery 先在 Superblock 持久化意图；自动发现挂载时恢复目标视图，第一笔新写之前持久化延迟创建的分支。
- 持久还原点把目标视图物化到源卷并保留启动锚点。还原点模式下的空间合并使用 Journal 内运行期 checkpoint，不回填源卷。

## 6. Journal v19

Journal 由 Superblock 与循环排列的 `1 MiB HeaderRegion + PayloadRegion` 组成。HeaderRegion 末尾保存 RegionLink，其余为 Record Header。普通 Record 中的源偏移是卷相对偏移；Journal 自身的 payload 偏移仍相对于 Journal 分区起点。

自动发现通过物理磁盘设备访问相邻日志分区时，`TargetBaseOffset` 只负责把 Journal 相对地址换算为物理磁盘地址，不参与源卷 MetaTree 的键值计算。

开发阶段不提供旧 Journal 格式兼容；格式版本不匹配会被视为已识别但不可挂载的保护状态，并阻止卷启动。

## 7. 主要同步对象

| 对象 | 保护范围 |
|---|---|
| `CaptureConfigMutex` | 保护配置与自动发现对象图变更 |
| 卷 FIFO Worker | 受保护卷 READ/WRITE/FLUSH 到达和完成顺序 |
| `HistoryMutex` | Journal append、drain、还原点物化和视图发布顺序 |
| `Journal.Lock` | Journal 游标、区域链、payload/metadata I/O 与 Flush |
| `TreeLock` | MetaTree、PreviewTree、Phase 和延迟分支状态 |
| `VolumeIoOutstanding` | 卷 FIFO 请求与关闭保护的生命周期屏障 |

## 8. 验证边界

用户态测试覆盖 after-image、挂载重建、卷相对区间覆盖、分支继承、Preview、Recovery、持久还原点、checkpoint、drain/物化、合并失败重试，以及自动发现对“无签名”和“已签名但损坏”的区分。

Volume PnP 启动时序、`volsnap` 下层排序、真实 Paging MDL、相邻分区原始 I/O、休眠/关机和启动盘关闭保护仍需在虚拟机及目标物理机做集成验证。
