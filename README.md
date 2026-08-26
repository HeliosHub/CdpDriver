# CdpDriver

CdpDriver 是一个 Windows 卷 Upper Filter 驱动。当前开发版本采用 after-image：受保护卷的应用写持久化到独立 Journal 后直接完成，不再提交到源卷；读取由当前分支的日志数据和源卷基础数据合成。

当前版本：**1.6.8-test15**（Build `20260826.102-disk-force-direct-drain`），Journal 磁盘格式：**v15**。

主要功能：

- 持续记录受保护卷的 after-image 写入
- 按时间点只读 Preview
- 基于分支切换的 Recovery
- 将 Recovery 意图写入 Superblock，重启后自动切换分支
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

Preview 按目标时间定位分支，递归包含父分支继承路径并构建 `PreviewTree`。读取命中的部分取 Journal after-image，空缺部分取源卷基础数据。

Recovery Begin 会排队该源卷读写，根据目标时间确定父分支和继承点，追加一个新分支记录，再构建完整的新 `MetaTree`。构建成功后原子替换旧树并立即恢复 I/O；整个过程不向源卷回填数据。`r` / Commit 仅作为兼容确认命令，不执行磁盘写回。

应用写逐条追加 payload 和 Record Header。日志写失败时对应应用写失败，不会绕过 Journal 写入源卷。

选择“重启恢复”时，`e` 只把恢复标记和目标时间写入 Journal Superblock，不立即构建历史树。下次启动时：

1. 启动卷枚举完成前，各卷读写由自动发现 Gate 暂存。
2. 驱动扫描并分类全部已启动卷，配对 Source 与 Journal。
3. 发现 Recovery 标记后只读取日志并构建目标 `MetaTree`，启动阶段不写新 RR。
4. 恢复后的第一笔源卷写入先持久化新分支并清除 Recovery 标记，再追加该笔 Payload；在此之前合并保持禁用。
4. Begin 成功并发布新 `MetaTree` 后放行源卷 I/O并清除标记。
5. Begin 失败时保留原分支视图和标记用于诊断/重试，但会放行 I/O，避免系统永久阻塞。

## Journal 空间与磁盘格式

当前开发格式为 v15：一个 Superblock，随后是若干 `1 MiB HeaderRegion + PayloadRegion`。单条磁盘 Record Header 为 32 字节，HeaderRegion 末尾 32 字节是 RegionLink。

- 单个 PayloadRegion 的跨度达到 Journal 容量的 `1/10` 时，即使 HeaderRegion 未写满也会切换新区域。
- 日志使用率达到90%时启动唯一合并线程。删除最旧 HeaderRegion 前，先把其中当前分支仍有效的最新值写入源卷；兄弟分支记录直接丢弃。
- 淘汰空间通过相邻 RegionLink 和最后一条 Record 的 `FileOffset + DataLength` 计算，不扫描整个 1 MiB HeaderRegion。
- 普通 Append 持久化 payload 和 Record Header；只有切换 HeaderRegion、Recovery 标记、凭据、Format/Close 等元数据变化时才更新 Superblock，避免每次 Append 的写放大。
- Record Header 的 `Sequence` 低16位是区域内索引，最高位 `0x80000000` 为 `BRANCH`。分支记录保存分支号、父分支号和继承 Record 序号；普通 Record 的全局序号为 `RegionLink.StartSequence + (Header.Sequence & 0xFFFF)`。
- 创建新分支时无条件新建 HeaderRegion，分支 Record 固定写在新区域索引0；新区域继续使用全局 `NextSequence`，不会按分支重新编号。
- 合并区域包含分支继承点时，会清理父分支无效后缀；只有继承点 Record 被丢弃的分支及其递归后代才立即删除。继承自仍有效/已回填 Record 的兄弟分支继续保留，直到合并到该分支自己的首个 HeaderRegion。跨区域删除使用内部 tombstone 保留已分配的 Sequence；查询和重挂载不会返回这些记录，后续新记录也不会复用其序号。

`CdpConsole u` 中的 metadata 包含 Superblock 占用扇区和所有活动 HeaderRegion。例如一个 1 MiB HeaderRegion 加一个 512 字节 Superblock 扇区为 `1,049,088` 字节；这不是把 1 MiB 换算错误。

## TRIM 处理

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

Release 构建仍保留关键生命周期、失败和恢复诊断日志。高频 `[COW-TRACE]`（包括 `write seen`、`write queued`、`write bypass`）使用 `Cdp_DBG`，只在 Debug 构建输出；Release 不输出这些跟踪。保护期间被抑制的 TRIM 使用 `[COW-TRIM]` 记录。

## 已知限制

- 当前可挂载 Journal v15 和上一版 v14；更早格式需要重新格式化。
- 全局同时只允许一个 Preview 会话，Preview 与 Recovery 互斥。
- 可恢复时间窗口取决于 Journal 容量；淘汰以 HeaderRegion 为粒度。
- Recovery Begin 扫描期间会排队源卷应用层读写，历史较多时可能产生可感知延迟。
- 当前写入不做批量合并，每个应用写独立追加 Journal。
- Paging I/O 依赖安全映射 MDL；映射失败时对应请求失败，不会绕过当前分支视图。

## 相关文档

- [架构设计](ARCHITECTURE.md)
- [CdpCore 与单元测试](CdpCore/README.md)
- [Apache License 2.0](LICENSE)
