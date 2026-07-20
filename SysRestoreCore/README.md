# SysRestoreCore

用户态可测的 COW / Preview / Recovery 引擎。通过 `QH_STORE` 抽象读写；测试用两块内存分别模拟**源分区**与 **journal 分区**。

## 与驱动的关系

| 组件 | 职责 |
|------|------|
| `SysRestoreCore` | Journal 布局、区间树、COW/Preview/Recovery 状态机（`QH_USERMODE` 编译同一份 `QHJournal.c`） |
| `SysRestoreDriver` | IRP/IOCTL 适配、分页 IO 透传、真设备 `QH_STORE` 后端（后续可接到 Core） |

当前 Journal 已支持 `Store` 后端与 `InitializeWithStore`；驱动仍走 `TargetDevice` IRP 路径。

## 构建与测试

```bat
msbuild SysRestoreDriver.sln /t:SysRestoreCore.Tests /p:Configuration=Release /p:Platform=x64
x64\Release\SysRestoreCore.Tests.exe
```

## API 入口

见 `include/qh_core.h`：`QhCoreCreate` / `QhCoreWrite` / `QhCoreRead` / `QhCorePreviewBegin` / `QhCoreRecoveryBegin` 等。
