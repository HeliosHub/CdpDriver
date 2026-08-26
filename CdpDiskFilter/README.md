# CdpDiskFilter 验证工程

这是一个独立的最小化 `DiskDrive` Upper Filter，只用于验证磁盘类驱动层的
after-image 重定向是否能解决卷过滤层可能存在的旁路读取问题。

主 `CdpDriver` 已包含正式的卷层读取与磁盘层读写路径，不链接也不加载本验证驱动；除非专门做 A/B 验证，不需要安装 `CdpDiskFilter`。

## 行为

- 不自动发现源卷或日志卷，必须由 `CdpDiskCtl` 明确指定同一磁盘上的
  源分区号和日志分区号。
- 源分区写请求不再写源分区；数据按到达顺序追加到日志分区，并把最新映射
  保存在非分页内存链表中。
- 源分区读请求先从源分区读取完整范围，再按最新写优先，用日志数据覆盖命中
  扇区，最后完成原读请求。
- 其他范围及其他 IRP 直接下发。

## 安装与使用

请只在启用测试签名的虚拟机中使用。以管理员身份执行：

```powershell
pnputil /add-driver C:\Users\Administrator\Desktop\CdpDriver\x64\Release\CdpDiskFilter\CdpDiskFilter.inf /install
```

该驱动是启动型 DiskDrive Lower Filter，安装后必须重启。用 PowerShell 的
`Get-Partition -DiskNumber <n>` 确认分区号，然后执行：

```powershell
C:\Users\Administrator\Desktop\CdpDriver\x64\Release\CdpDiskCtl.exe configure <磁盘号> <源分区号> <日志分区号>
C:\Users\Administrator\Desktop\CdpDriver\x64\Release\CdpDiskCtl.exe query <磁盘号>
C:\Users\Administrator\Desktop\CdpDriver\x64\Release\CdpDiskCtl.exe disable <磁盘号>
```

`configure` 会读取并提交分区样式、磁盘标识、扇区大小、磁盘长度以及两个分区的
绝对物理偏移。过滤器注册在 DiskDrive Upper 列表中 PartMgr 之后，接收 PartMgr
换算后的绝对磁盘偏移；下层仍为 disk.sys，因此能够直接处理和提交
`IRP_MJ_READ/WRITE`。驱动优先按磁盘号匹配设备栈，并用 GPT Disk GUID 或 MBR
Disk Signature 后备校验。

## 测试版限制

- 日志映射仅存在内存中；重启或 `disable` 后全部丢失。
- 日志分区从起始位置直接覆盖，原有文件系统和数据会损坏；必须使用专用测试分区。
- 不实现环形、回收、Record Header、持久化、预览、恢复、分支或合并。
- 日志写满后返回 `STATUS_DISK_FULL`。
- 单笔拦截 I/O 最大 4 MiB，且必须按物理扇区对齐并完整位于源分区内。
- Flush、TRIM、SCSI/设备控制等不参与重定向，仅原样下发。
- 队列为单工作线程 FIFO，优先验证语义和层级，不代表最终性能设计。
