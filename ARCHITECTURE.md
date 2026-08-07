# 架构设计

## 1. 总体结构

CdpDriver 是 Volume Upper Filter。受保护卷的写 IRP 被送入 PASSIVE_LEVEL Worker，应用数据作为 after-image 持久化到专用 Journal；成功后直接完成上层 IRP，不再写入源卷。源卷作为基础镜像，Journal 保存各分支后续产生的数据。

- `CdpDriver`：IRP/IOCTL、启动卷发现、I/O Gate、Worker、合并线程。
- `CdpCore`：当前视图、Preview、Recovery、合并协调。
- `CdpJournal`：v12 磁盘格式、Record 扫描、分支树和区间树构建。
- `CdpConsole`：配置、查询、Preview/Recovery 和安装管理。

## 2. 写入与读取

### 写入

1. Dispatch 将写 IRP 标记 Pending 并排入卷级队列。
2. Worker 等待自动发现 Gate，并持有 `HistoryMutex` 串行化日志顺序。
3. `CdpCoreAppendAfterImage` 写 payload 和普通 Record Header。
4. 在 `TreeLock` 下用新 Record 覆盖更新当前分支 `MetaTree`。
5. Journal 成功后完成应用写；失败则使应用写失败。停止保护后才把写 IRP下发源卷。

当前不使用旧的 before-image 捕获、批量 COW、HistoryTree、StagingTree 或回填式 Commit。

### 读取

所有模式均通过 CdpCore 合成读取：当前模式查 `MetaTree`，Preview 查 `PreviewTree`；命中区间读取 Journal payload，空缺区间读取源卷。树访问由 `TreeLock` 保护。

保护开启时抑制 `DeviceDsmAction_Trim`，避免底层回收源卷基础数据；停止保护后直接转发。

## 3. Journal v12

磁盘布局为一个 Superblock，随后循环排列 `1 MiB HeaderRegion + PayloadRegion`。HeaderRegion 最后32字节为 `Cdp_HEADER_REGION_LINK`，其余共有32767个32字节 Record 槽。

普通 Record 保存时间、源卷偏移、payload 日志偏移、长度和区域内索引。`Sequence` 低16位为区域内索引，最高位 `0x80000000` 为 `BRANCH`；分支 Record 用相同32字节保存分支号、父分支号和继承的全局 Record 序号，且没有 payload。

运行时全局序号为：

```text
RegionLink.StartSequence + (Header.Sequence & 0xFFFF)
```

PayloadRegion 跨度达到日志卷容量的 `1/10` 或 HeaderRegion 写满时切换新区域。普通 Append 不逐次更新 Superblock；区域切换、Recovery 意图、凭据及格式化等元数据变化才更新。

## 4. 分支与 MetaTree

格式化 Journal 时创建分支1。后续每次创建分支都强制新建 HeaderRegion，分支 Record 位于新区域索引0，但 `StartSequence` 延续全局 `NextSequence`。挂载从保留 Record 重建分支信息树，节点包含父分支、继承点、起止 Record 和 latest 标记。当前视图从 newest 向 oldest 单遍扫描，只接纳当前分支及其递归祖先在继承点以内的 Record；相同卷区间只保留最新值。

写入成功后直接用新 Record 更新 `MetaTree`。挂载期间驱动排队读写，树完整发布后才放行。

## 5. 空间合并

日志使用率达到90%时启动唯一合并线程；已有合并线程时不得重复启动。

1. 定位最旧且不是 active/newest 的 HeaderRegion。
2. 只在该区域内构建当前分支路径的最新值树，兄弟分支 Record 丢弃。
3. 把这些最新值从 Journal 写入源卷，使源卷成为新的基础镜像。
4. 从 `MetaTree` 删除仍指向该区域的覆盖；更新活动 Preview 的相应覆盖。
5. 若待删区域不包含任何分支继承点，不处理其他区域的不可达数据。
6. 若待删区域包含继承点，删除祖先分支在有效继承点后的后缀。分支删除采用依赖级联：仅当其继承点 Record 被丢弃，或合并已经到达该非当前分支自己的首个 HeaderRegion时，才删除该分支及其递归后代。
7. 继承自仍有效或已回填 Record 的兄弟分支不因“不是最新分支”而提前删除，继续保留到合并实际到达其自身区域。
8. 跨区域删除写成内部 tombstone。槽位的全局 Sequence 永久保留，但不进入 Record 查询、时间范围、分支树或视图树；对应区域成为 oldest 后再整区物理回收。
9. 全部回填和清理成功后删除目标 HeaderRegion；任一步失败均停止本次合并。

Preview 正在启动时拒绝合并；合并已运行时拒绝新 Preview。若合并到达 Preview 目标 Record 所在区域，则停止 Preview。

## 6. Preview

Preview 根据目标时间在分支信息树中定位目标分支和目标 Record，再递归扫描该分支及父分支继承路径，直接构建 `PreviewTree`。读取命中树时取 Journal，空缺取源卷。目标时间早于 oldest 时按 oldest 处理。

## 7. Recovery

Recovery 是分支切换，不修改源卷：

1. 驱动停止并等待合并线程结束，然后关闭源卷 Recovery I/O Gate。
2. 根据目标时间确定父分支和继承 Record。
3. 追加新分支 Record。
4. 扫描新分支完整继承路径，构建替换 `MetaTree`。
5. 成功后原子交换新旧树、返回 General 并放行 I/O。

失败时删除刚追加的空分支 Record，保留原 `MetaTree`。`Commit` 是幂等的兼容确认，不执行源卷写回；运行中的同步 Begin 没有可取消的 prepared 状态。

重启 Recovery 意图保存在 Superblock。启动时全部卷完成发现和 Source/Journal 配对前保持 Gate；发现意图后完成上述 Begin，成功发布新树后放行 I/O并清除标记。

## 8. 主要同步对象

| 对象 | 保护范围 |
|---|---|
| `HistoryMutex` | 单卷 Journal 追加、树构建和 Recovery 切换顺序 |
| `Journal.Lock` | Journal 运行时游标、区域链和磁盘元数据 |
| `TreeLock` | `MetaTree`、`PreviewTree` 及 Phase/构建协调字段 |
| `AutoDiscoveryGateEvent` | 启动卷分类和自动 Recovery 完成前的 I/O |
| 合并线程 Gate | 每个 Core 最多一个合并所有者，并与 Preview/Recovery 协调 |

## 9. 验证边界

用户态单测覆盖 after-image 写入与失败、分支继承读取、挂载重建、合并与失败重试、Preview 分支路径、Preview/合并协调以及 Recovery 分支切换与回滚。PnP 时序、真实 Paging MDL、启动 Gate 和签名加载仍需在虚拟机及目标系统做集成验证。
