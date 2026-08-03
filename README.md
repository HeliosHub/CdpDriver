# CdpDriver

CdpDriver 是一个 Windows 卷 Upper Filter 驱动。它对受保护卷执行写前镜像（Copy-on-Write，COW）：先把即将被覆盖的旧数据写入独立 Journal 卷，再把原写请求提交给源卷。因此源卷始终保存最新数据，Journal 保存可用于时间点预览和恢复的 before-image。

当前版本：**1.4.1**（Build `20260803.1`），Journal 磁盘格式：**v11**。

主要功能：

- 持续捕获受保护卷的覆盖写入
- 按时间点只读 Preview
- 普通 Recovery（Begin + Commit）
- 将 Recovery 意图写入 Superblock，重启后自动 Begin + Commit
- 查询 Journal 使用量、剩余负载空间和全部 Record 元数据
- 全局保护密码、句柄级认证和密码更换
- `VolHexdump` 按卷偏移导出原始数据

> ## 数据安全警告
>
> 本项目直接工作在卷设备栈并修改专用 Journal 分区。请先在虚拟机或可重装测试机上验证，并完整备份重要数据。格式化 Journal 会清除其中全部历史。测试签名仅适用于测试环境，生产部署需要符合 Windows 要求的内核驱动签名。

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
3. 使用 `i` 安装 INF、注册驱动并配置 Volume UpperFilters；按提示重启。
4. 准备一块不与源卷混用的专用 Journal 分区。
5. 使用 `1` 指定源卷 GUID 和 Journal 卷 GUID，选择格式化或挂载并开启保护。

格式化新 Journal 时会设置共享保护密码。密码不以明文写盘；之后的敏感操作需要先通过 `p` 完成认证。

## CdpConsole 命令

| 命令 | 功能 |
|---|---|
| `i` | 安装/注册驱动（INF + Volume UpperFilters） |
| `1` | 配置并开启保护：源卷 GUID + 专用 Journal GUID |
| `2` | 停止指定源卷的保护，并使其 Journal 不再被自动发现 |
| `6` | 按源卷和时间点开始 Preview |
| `7` | 按卷字节偏移读取 Preview 数据，单次最多 2 MiB |
| `8` | 结束 Preview 会话 |
| `9` | 查询最早/最新 Record 时间 |
| `u` | 查询 Journal 容量、元数据空间、Record 负载已用/剩余空间 |
| `l` | 列出全部现存 Record 元数据和 Flags，不读取或输出 payload |
| `s` | 查询源卷保护状态、Phase 和 Journal GUID |
| `e` | 准备 Recovery；可选择只持久化重启恢复意图 |
| `r` | 同步提交已经准备好的 Recovery |
| `c` | 取消 Recovery；也可清除尚未执行的重启恢复标记 |
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

Preview 只构建历史视图，不修改源卷。读取时，历史树覆盖的部分取 Journal before-image，空缺部分取源卷当前数据；非扇区对齐请求会先扩展到对齐区间完成合成，再只返回调用方要求的字节。

普通 Recovery 分为两个明确阶段：

1. `e` / Begin：排队该源卷的应用层读写，扫描 Journal 并构建 HistoryTree；Begin 完成后恢复 I/O。
2. `r` / Commit：按全局 Sequence 逐节点把 before-image 回填源卷；每个节点后释放同步锁，使新写能够在节点之间执行。

Recovery Phase 中的读取由 HistoryTree 和源卷实时数据合成，包括 Paging I/O。新写仍先正常 COW；若与尚未回填的历史区间重叠，会从 HistoryTree 中 Punch 掉重叠部分，从而保留新写数据。Punch/拆分失败会使 Recovery 失败，不退回“整节点失效”的策略。

从 Recovery Begin 开始到 Commit 完成，应用新写和恢复回填自身产生的 COW 都带 `BACKFILL (0x80000000)` 标志。恢复回填先把当前源数据作为 backfill before-image 追加到 Journal，再写源卷；由于 `WritebackActive`，该回填 COW 不进入或 Punch 当前 Recovery HistoryTree。后续 Preview/Recovery 扫描遇到 backfill Record 时，无视该 Record 已满足时间停止条件，仍将它插入树并继续向旧记录查找，最远只到当前 oldest。

选择“重启恢复”时，`e` 只把恢复标记和目标时间写入 Journal Superblock，不立即构建历史树。下次启动时：

1. 启动卷枚举完成前，各卷读写由自动发现 Gate 暂存。
2. 驱动扫描并分类全部已启动卷，配对 Source 与 Journal。
3. 发现 Recovery 标记后自动执行 Begin。
4. Begin 成功后立即放行该源卷启动 I/O，再自动 Commit；新写按上述 Recovery 规则与回填交替执行。
5. Commit 成功后清除标记并回到 General Phase；失败时保留标记用于诊断/重试，但会放行 I/O，避免系统永久阻塞。

## Journal 空间与磁盘格式

当前开发格式为 v11：一个 Superblock，随后是若干 `1 MiB HeaderRegion + PayloadRegion`。单条磁盘 Record Header 为 32 字节，HeaderRegion 末尾 32 字节是 RegionLink。

- 单个 PayloadRegion 的跨度达到 Journal 容量的 `1/10` 时，即使 HeaderRegion 未写满也会切换新区域。
- 空间不足时整区淘汰最旧 HeaderRegion 及其全部 payload，不逐条删除 Record。
- 淘汰空间通过相邻 RegionLink 和最后一条 Record 的 `FileOffset + DataLength` 计算，不扫描整个 1 MiB HeaderRegion。
- 普通 Append 持久化 payload 和 Record Header；只有切换 HeaderRegion、Recovery 标记、凭据、Format/Close 等元数据变化时才更新 Superblock，避免每次 Append 的写放大。
- Record Header 的 `Sequence` 低16位是区域内索引，高16位是标志；当前定义最高位 `0x80000000` 为 `BACKFILL`。运行时全局序号为 `RegionLink.StartSequence + (Header.Sequence & 0xFFFF)`，使用64位表示。

`CdpConsole u` 中的 metadata 包含 Superblock 占用扇区和所有活动 HeaderRegion。例如一个 1 MiB HeaderRegion 加一个 512 字节 Superblock 扇区为 `1,049,088` 字节；这不是把 1 MiB 换算错误。

## TRIM 处理

保护开启期间，驱动成功拦截并抑制 `DeviceDsmAction_Trim`，以免删除文件后物理扇区被清零/回收而绕过普通 COW。删除大量文件不会立即把所有被删数据复制到 Journal；对应簇以后被重新写入时，真实 `IRP_MJ_WRITE` 才按需捕获 before-image。停止保护后 TRIM 和普通写直接下发，不再进入 COW。

## 保护密码与授权

- Superblock 只保存 PBKDF2-SHA256 派生校验值、随机 Salt、迭代次数、CredentialId 和 AuthEpoch，不保存明文密码。
- 默认 PBKDF2 迭代次数为 200,000。
- 认证状态绑定到打开控制设备的文件句柄，不能被其他句柄复用。
- 成功认证后的空闲有效期为 1 小时；通过敏感操作授权检查会续期。
- 更换密码会更新所有使用当前共享凭据的已挂载 Journal，并递增认证代次；中途失败时驱动会尝试回滚已经更新的 Journal。
- 连续 5 次认证失败会锁定认证入口 1 小时。
- 遇到 `ERROR_ACCESS_DENIED (err=5)` 时应重新认证，并确认目标源卷绑定的 Journal 使用同一凭据。

## 日志行为

Release 构建仍保留关键生命周期、失败和恢复诊断日志。高频 `[COW-TRACE]`（包括 `write seen`、`write queued`、`write bypass`）使用 `Cdp_DBG`，只在 Debug 构建输出；Release 不输出这些跟踪。保护期间被抑制的 TRIM 使用 `[COW-TRIM]` 记录。

## 已知限制

- 当前只兼容 Journal v11；升级后需要重新格式化旧 Journal。
- 全局同时只允许一个 Preview 会话，Preview 与 Recovery 互斥。
- 可恢复时间窗口取决于 Journal 容量；淘汰以 HeaderRegion 为粒度。
- Recovery Begin 扫描期间会排队源卷应用层读写，历史较多时可能产生可感知延迟。
- Recovery Commit 对调用者同步，并会占用源卷与 Journal I/O 带宽。
- Recovery Paging I/O 依赖安全映射 MDL；映射失败时对应读取失败，不会绕过历史视图读取 live source。

## 相关文档

- [架构设计](ARCHITECTURE.md)
- [CdpCore 与单元测试](CdpCore/README.md)
- [Apache License 2.0](LICENSE)
