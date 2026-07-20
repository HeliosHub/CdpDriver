# SysRestoreDriver

Windows 卷过滤驱动：对受保护卷做 **写前镜像（COW）**，把被覆盖的旧数据写入独立的 **Journal/CDP 分区**，源卷始终保持最新数据。基于 journal 可做：

- **时间点 Preview**（只读重建历史视图）
- **Recovery**（读路径呈现指定时刻的数据）

配套：

- `QHConsole` — 控制台工具（安装/注册驱动、配置捕获、扇区读写、Preview、查询 journal 时间范围）

> ## ⚠️ 数据安全警告
>
> 本项目是**挂在卷设备栈上的内核态过滤驱动**，开启捕获后会拦截写路径并写 journal 分区。请先阅读：
>
> - **务必先在虚拟机或可随时重装的测试机上试用**，不要直接在装有重要数据的机器上首次部署
> - **试用前请完整备份**；未签名内核驱动在未经测试的环境可能蓝屏
> - **需要一块专用 journal 分区**（建议独立、足够大）；Format journal 会清空全部历史
> - 本项目按 Apache License 2.0 以 **"按现状（AS-IS）"** 提供，不承担数据丢失或故障责任

## 适用场景

- 需要保留可回溯历史的系统盘 / 数据盘保护
- 按时间点查看或恢复卷内容（Preview / Recovery）
- 开发与联调：用 `QHConsole` 验证 COW 与 Preview

仅在 Win10/11 上测试。本项目基于 WDK 从头构建，Apache License 2.0 开源。

## 项目状态

当前实现：**COW Journal v6**（2MB 头区 + 负载区交替）+ Preview + Recovery 阶段机。驱动安装与捕获/Preview 均通过 `QHConsole` 完成。

## 依赖

**零第三方依赖，纯 WDK。** 驱动只链接 WDK 内核库（`ntoskrnl`、`hal` 等）。

## 支持的平台

- Windows 10/11：已测试
- Windows 8.1/7：理论上兼容，未经测试

## 编译环境

- Microsoft Visual Studio 2022
- Windows SDK / WDK 10.0.26100.0（或兼容版本）

## 编译步骤

1. 打开 `SysRestoreDriver.sln`
2. 选择 Release / x64
3. 生成解决方案

产物：

- `SysRestoreDriver\x64\Release\SysRestoreDriver.sys`
- `QHConsole\x64\Release\QHConsole.exe`
- `QHConsole\x64\Release\driver\SysRestoreDriver.sys` + `.inf`（驱动构建后自动复制，供 `i` 命令安装）

## 部署与使用

1. 管理员终端：`bcdedit /set testsigning on`，重启  
   > 当前未做生产签名，需测试模式。生产环境需 EV 内核驱动签名证书。
2. 以管理员运行 `QHConsole.exe`，执行 **`i`** 安装/注册驱动（INF + Volume UpperFilters）；必要时重启
3. 准备**空闲专用分区**作为 journal（勿与源卷混用）
4. 在 `QHConsole` 中：
    - `1` — 配置捕获（源卷 GUID + journal GUID；可选 Format）
    - `2` — 停止捕获
    - `4` / `3` / `5` — 打开卷句柄 / 按句柄读扇区 / 关闭句柄（单次读最大 2MB）
    - `6` / `7` / `8` — Preview 开始 / 读（单次最大 2MB）/ 结束
    - `9` — 查询 journal 最早/最新 COW 记录时间（需已 CMD1 配置捕获）
5. Recovery：通过 `IOCTL_QH_BEGIN_RECOVERY` / `IOCTL_QH_END_RECOVERY`（结束必回 Normal）；控制台命令可后续补充

**开启/停止 COW 捕获**：`QHConsole` 命令 `1`（CMD1）开启、`2`（CMD2）停止。重启后需重新 CMD1（当前无持久化保护开关文件）。

详见 [架构设计](ARCHITECTURE.md)。

## FAQ

### Q1：和旧版“写重定向到空闲扇区”有何不同？

旧方案改写写入目标扇区，靠重启丢弃映射还原。当前方案 **源卷写透传**，历史 before-image 进独立 journal，支持按时间 Preview/Recovery，且不依赖 `$Bitmap` / 扇区映射表。

### Q2：Journal 分区要多大？

取决于写入量与希望保留的时间窗口。每次写都会追加 before-image；分区填满后驱动会推进最旧 header region 丢弃旧记录。生产前请按负载评估容量，并预留余量。

### Q3：能否同时开多个 Preview？

**不能。** 全局同时只允许一个 Preview；结束或 Recovery 结束后回到 Normal。

### Q4：Recovery 结束后状态是什么？

`END_RECOVERY` 清理恢复上下文后，**Phase 进入 Normal**。

## 已知限制

- Journal 格式变更（当前 v6，2MB 头区+负载区交替）后需重新 Format
- 全局仅一个 Preview 会话
- Recovery 下分页读仍透传
- `QHConsole` 尚未内置 Recovery 菜单项

## 版权与许可

Apache License 2.0，版权归 Xuhui Jiang。见 [LICENSE](LICENSE)、[NOTICE](NOTICE)。

## 相关文档

- [架构设计](ARCHITECTURE.md)
