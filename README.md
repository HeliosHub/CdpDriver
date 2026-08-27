# CdpDriver

CdpDriver 是一个同时工作在 Windows `Volume` 与 `DiskDrive` 类的 Upper Filter 驱动。当前开发版本采用 after-image：受保护分区的应用写在磁盘层持久化到独立 Journal 后直接完成，不再提交到源分区；卷层优先合成读取，磁盘层为旁路读取兜底。

当前版本：**1.6.8-test28**（Build `20260827.115-restore-point-prune`），Journal 磁盘格式：**v15**。

主要功能：

- 持续记录受保护卷的 after-image 写入
- 按时间点只读 Preview
- 基于分支切换的 Recovery
- 将 Recovery 意图写入 Superblock，重启后自动切换分支
- 将指定时间视图物化为持久还原点，重启时跳过旧历史扫描
- 查询 Journal 使用量、剩余负载空间和全部 Record 元数据
- 全局保护密码、句柄级认证和密码更换
- `VolHexdump` 按卷偏移导出原始数据

> ## 数据安全警告
>
> 本项目直接工作在卷和物理磁盘设备栈，并会修改源分区与专用 Journal 分区。请先在虚拟机或可重装测试机上验证，并完整备份重要数据。格式化 Journal 会清除其中全部历史。测试签名仅适用于测试环境，生产部署需要符合 Windows 要求的内核驱动签名。

## 支持环境

- Windows 10 / Windows 11 x64
- Visual Studio 2022
- Windows SDK / WDK 10.0.26100.0，或兼容版本

Windows 7/8.1 未验证，也不作为当前支持目标。

## 构建

打开 `CdpDriver.sln`，选择 `Release | x64` 后生成解决方案。主要产物位于：

- `x64\Release\CdpConsole.exe`
- `x64\Release\VolHexdump.exe`
- `x64\Release\CdpCore.Tests.exe`
- `x64\Release\driver\CdpDriver.sys`
- `x64\Release\driver\CdpDriver.inf`
- `x64\Release\driver\cdpdriver.cat`

也可以从开发者命令行构建：

```bat
msbuild CdpDriver.sln /m /p:Configuration=Release /p:Platform=x64
```

## 部署与基本使用

1. 测试机开启测试签名并重启：`bcdedit /set testsigning on`。
2. 以管理员身份运行 `CdpConsole.exe`。
3. 使用 `i` 安装 INF、注册驱动并配置 Volume/DiskDrive UpperFilters；按提示重启。
4. 准备一块不与源卷混用的专用 Journal 分区。
5. 使用 `1` 指定源卷 GUID 和 Journal 卷 GUID，选择格式化或挂载并开启保护。

格式化新 Journal 时会设置共享保护密码。密码不以明文写盘；之后的敏感操作需要先通过 `p` 完成认证。

## CdpConsole 命令

| 命令 | 功能 |
|---|---|
| `i` | 安装/注册驱动（INF + Volume/DiskDrive UpperFilters） |
| `1` | 配置并开启保护：源卷 GUID + 专用 Journal GUID |
| `2` | 停止指定源卷的保护，并使其 Journal 不再被自动发现 |
| `6` | 按源卷和时间点开始 Preview |
| `7` | 按卷字节偏移读取 Preview 数据，单次最多 2 MiB |
| `8` | 结束 Preview 会话 |
| `9` | 查询最早/最新 Record 时间 |
| `u` | 查询 Journal 容量、元数据空间、Record 负载已用/剩余空间 |
| `l` | 列出全部现存 Record 元数据和 Flags，不读取或输出 payload |
| `b` | 分页列出分支树、当前分支和继承关系 |
| `s` | 查询源卷保护状态、Phase 和 Journal GUID |
| `e` | 准备 Recovery；可选择只持久化重启恢复意图 |
| `r` | 同步提交已经准备好的 Recovery |
| `c` | 取消 Recovery；也可清除尚未执行的重启恢复标记 |
| `o` | 把指定时间视图物化到源分区并设置持久还原点 |
| `x` | 删除持久还原点；启动后首次受保护写入前不允许删除 |
| `m` | 手动合并一个最旧 RR，并回收该操作导致失效分支的连续 RR |
| `p` | 设置、验证或更换共享保护密码 |
| `v` | 列出卷 |
| `d` | 查询驱动版本、Build 和 Journal 版本 |
| `t` | 将本地时间 `年-月-日 时:分:秒` 转成 `WallClock100ns` |
| `h` | 帮助 |
| `q` | 退出控制台，不停止保护 |

如果 Preview/Recovery 请求时间早于当前最早 Record，驱动会自动按 oldest 时间点处理；普通恢复、重启恢复和返回的有效目标时间使用同一规则。

`VolHexdump` 可直接把卷内指定区间导出到文件；offset/size 相对于分区起点，支持十进制和 `0x` 十六进制：

```bat
VolHexdump <volume-guid> <offset> <size> <output-file>
```

只传入 `<volume-guid>` 时进入交互模式。输出文件已存在时会被覆盖。

## Preview 与 Recovery

Preview 按目标时间定位分支，递归包含父分支继承路径并构建 `PreviewTree`。读取命中的部分取 Journal after-image，空缺部分取源卷基础数据。

Recovery Begin 根据目标时间确定父分支和继承点，追加一个新分支记录，再构建完整的新 `MetaTree`。构建成功后原子替换旧树；整个过程不向源分区回填数据。`r` / Commit 仅作为兼容确认命令，不执行磁盘写回。

应用写逐条追加 payload 和 Record Header。日志写失败时对应应用写失败，不会绕过 Journal 写入源卷。

选择“重启恢复”时，`e` 只把恢复标记和目标时间写入 Journal Superblock，不立即构建历史树。下次启动时：

1. 磁盘和卷的 `START_DEVICE` 阶段完成物理布局识别及 Source/Journal 配对。
2. 发现 Recovery 标记后读取日志并构建目标 `MetaTree`，启动阶段不创建新分支 Record。
3. 目标视图发布后保护路径生效；恢复后的第一笔写入先持久化延迟创建的新分支，再追加该笔 payload。
4. 自动恢复失败时保留标记及原历史用于诊断和重试，不让启动 I/O 永久阻塞。

## 持久还原点

`o` 会把指定时间的完整视图物化到源分区，并将还原点标记写入 Journal Superblock。物化写入直接从磁盘过滤层下方向对应物理磁盘发送绝对偏移 WRITE，不经过卷栈。目标 Record 及其之前的历史随即回收：完整过期 HeaderRegion（RR）直接删除；与目标边界同处一个 RR 的旧数据 Record 写为 tombstone。该边界 RR 为保留后续数据所需的分支结构 Header 不删除。

重启后检测到还原点时，驱动直接使用已经物化的源分区作为基础视图，不扫描旧 Record。第一笔受保护写入前会清空旧历史并创建新的根分支。还原点不会自动过期；启动后首次受保护写完成前不支持删除。

## 停止保护与 Drain

`2` 进入 `DRAINING` 后，把当前 `MetaTree` 覆盖逐段写回源分区。写回 IRP 直接发送给物理磁盘过滤设备的下层对象，使用绝对磁盘偏移，并设置 `SL_FORCE_DIRECT_WRITE`。该路径不使用 volume gate、线程识别或 IRP 私有标记，也不会重新进入本驱动的卷层或磁盘层。只有全部范围成功写回后才关闭保护；失败时保留保护状态。

## 手动合并

`m` 会异步合并一个最旧的可合并 HeaderRegion，忽略自动合并要求的 90% Journal 使用率。若该次回收使某些分支失效，Core 会在同一回收事务中标记并继续删除连续的 tombstone RR；但不会因此再进入下一个普通 RR，后者仍由自动合并在空间使用率达到阈值时处理。命令需要认证，并且仅在保护处于 General 阶段、没有 Preview、没有持久还原点、也没有正在运行的合并线程时可接受。合并期间，开始 Preview 的返回数据直接带 `MERGING` 状态和空句柄；GUI 据此提示等待合并完成，无需额外查询。完成结果记录在驱动 `[MERGE]` 日志中。

自动合并在 General 阶段的阈值为 90%。若存在 Preview，则仅在使用率达到 95% 时紧急中止该 Preview 并开始合并；预览读取会返回“预览已因 Journal 使用率达到 95% 而被自动合并中止”的明确错误，CdpGui 检测到该读取错误后会立即结束对应 Preview 会话。

## Journal 空间与磁盘格式

当前开发格式为 v15：一个 Superblock，随后是若干 `1 MiB HeaderRegion + PayloadRegion`。单条磁盘 Record Header 为 32 字节，HeaderRegion 末尾 32 字节是 RegionLink。

- 单个 PayloadRegion 的跨度达到 Journal 容量的 `1/10` 时，即使 HeaderRegion 未写满也会切换新区域。
- 日志使用率达到90%时启动唯一合并线程。删除最旧 HeaderRegion 前，先把其中当前分支仍有效的最新值通过磁盘下层的 `SL_FORCE_DIRECT_WRITE` 写入源卷；兄弟分支记录直接丢弃。合并日志会输出模式、RR 偏移和序号范围。
- 淘汰空间通过相邻 RegionLink 和最后一条 Record 的 `FileOffset + DataLength` 计算，不扫描整个 1 MiB HeaderRegion。
- 普通 Append 持久化 payload 和 Record Header；只有切换 HeaderRegion、Recovery 标记、凭据、Format/Close 等元数据变化时才更新 Superblock，避免每次 Append 的写放大。
- Record Header 的 `Sequence` 低16位是区域内索引，最高位 `0x80000000` 为 `BRANCH`。分支记录保存分支号、父分支号和继承 Record 序号；普通 Record 的全局序号为 `RegionLink.StartSequence + (Header.Sequence & 0xFFFF)`。
- 创建新分支时无条件新建 HeaderRegion，分支 Record 固定写在新区域索引0；新区域继续使用全局 `NextSequence`，不会按分支重新编号。
- 合并区域包含分支继承点时，会清理父分支无效后缀；只有继承点 Record 被丢弃的分支及其递归后代才立即删除。继承自仍有效/已回填 Record 的兄弟分支继续保留，直到合并到该分支自己的首个 HeaderRegion。跨区域删除使用内部 tombstone 保留已分配的 Sequence；查询和重挂载不会返回这些记录，后续新记录也不会复用其序号。

`CdpConsole u` 中的 metadata 包含 Superblock 占用扇区和所有活动 HeaderRegion。例如一个 1 MiB HeaderRegion 加一个 512 字节 Superblock 扇区为 `1,049,088` 字节；这不是把 1 MiB 换算错误。

## 读写分层与 TRIM

卷层 WRITE 始终透传，由磁盘层完成最终保护截获。卷层 READ 对已绑定保护分区优先合成；直接到达磁盘层的读取由磁盘层兜底。每个磁盘使用“最近命中分区 + 有序范围二分查找”定位保护分区，多分区查找复杂度为 `O(log N)`。

保护开启期间，驱动拦截并抑制 `DeviceDsmAction_Trim`，避免底层回收破坏源卷基础数据。停止保护后 TRIM 和普通写直接下发。

## 保护密码与授权

- Superblock 只保存 PBKDF2-SHA256 派生校验值、随机 Salt、迭代次数、CredentialId 和 AuthEpoch，不保存明文密码。
- 默认 PBKDF2 迭代次数为 200,000。
- 认证状态绑定到打开控制设备的文件句柄，不能被其他句柄复用。
- 成功认证后的空闲有效期为 1 小时；通过敏感操作授权检查会续期。
- 更换密码会更新所有使用当前共享凭据的已挂载 Journal，并递增认证代次；中途失败时驱动会尝试回滚已经更新的 Journal。
- 连续 5 次认证失败会锁定认证入口 1 小时。
- 遇到 `ERROR_ACCESS_DENIED (err=5)` 时应重新认证，并确认目标源卷绑定的 Journal 使用同一凭据。

## 日志行为

Release 构建保留关键生命周期、失败、读取路径和 drain 诊断日志。高频 I/O 跟踪使用 `Cdp_DBG`，只在 Debug 构建输出。保护期间被抑制的 TRIM 使用 `[COW-TRIM]` 记录；磁盘直写失败使用 `[DRAIN-DISK-WRITE-FAIL]`，同步 I/O 长时间未完成使用 `[DRAIN-DIAG]`。

## 已知限制

- 当前可挂载 Journal v15 和上一版 v14；更早格式需要重新格式化。
- 全局同时只允许一个 Preview 会话，Preview 与 Recovery 互斥。
- 可恢复时间窗口取决于 Journal 容量；淘汰以 HeaderRegion 为粒度。
- Recovery Begin 扫描期间会排队源卷应用层读写，历史较多时可能产生可感知延迟。
- 当前写入不做批量合并，每个应用写独立追加 Journal。
- Paging I/O 依赖安全映射 MDL；映射失败时对应请求失败，不会绕过当前分支视图。
- 持久还原点启动后、首次受保护写入完成前不支持删除。

## 相关文档

- [架构设计](ARCHITECTURE.md)
- [CdpCore 与单元测试](CdpCore/README.md)
- [Apache License 2.0](LICENSE)
