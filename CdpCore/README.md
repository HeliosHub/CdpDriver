# CdpCore

用户态可测的 COW / Preview / Recovery 引擎。通过 `Cdp_STORE` 抽象读写；测试用两块内存分别模拟**源分区**与 **journal 分区**。

## 与驱动的关系

| 组件 | 职责 |
|------|------|
| `CdpCore` | Journal 布局、区间树、COW/Preview/Recovery 状态机（`Cdp_USERMODE` 编译同一份 `CdpJournal.c`） |
| `CdpDriver` | IRP/IOCTL 适配、分页 IO 透传、真设备 `Cdp_STORE` 后端（后续可接到 Core） |

当前 Journal 已支持 `Store` 后端与 `InitializeWithStore`；驱动仍走 `TargetDevice` IRP 路径。

## 构建与测试

```bat
msbuild CdpDriver.sln /t:CdpCore.Tests /p:Configuration=Release /p:Platform=x64
x64\Release\CdpCore.Tests.exe
```

## API 入口

见 `include/cdp_core.h`：`CdpCoreCreate` / `CdpCoreWrite` / `CdpCoreRead` / `CdpCorePreviewBegin` / `CdpCoreRecoveryBegin` 等。
