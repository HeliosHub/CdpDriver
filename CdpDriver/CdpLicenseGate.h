#pragma once

/*
 * 授权闸门与状态机（设计 §5～§6 / §11）。
 *
 * 混淆清单：O-07～O-10 / O-14 / O-15 / O-19 / O-20
 *
 * 典型调用：
 *   DriverEntry           → CdpLicenseInitialize
 *   Journal Mount         → CdpLicenseOnJournalMounted（从盘恢复 g_*）
 *   SET_LICENSE IOCTL     → CdpLicenseSetFromBlob
 *   BEGIN_RECOVERY（真开）→ CdpLicenseGateBeforeOp → CdpLicenseGateArmOp
 *   COMMIT_RECOVERY 成功  → CdpLicenseGateAfterOpSuccess
 *   BEGIN 失败 / Cancel    → CdpLicenseGateAbortOp
 *   BUILD_APPLY_QR        → CdpLicenseBuildApplyQrPayload
 */

#ifdef CDP_LICENSE

#include "CdpEngineDefs.h"
#include "CdpLicenseDefs.h"

extern Cdp_LICENSE_STATE g_CdpLicenseState; /* 单机全局授权镜像 */
/*
 * KMUTEX（非 FastMutex）：闸门会在锁内解密 E0 / SHA256（BCrypt），
 * 必须保持 PASSIVE_LEVEL。FastMutex 升到 APC_LEVEL 会得到 STATUS_NOT_SUPPORTED。
 */
extern KMUTEX g_CdpLicenseMutex;

#define CdpLicenseLock() \
	KeWaitForSingleObject(&g_CdpLicenseMutex, Executive, KernelMode, FALSE, NULL)
#define CdpLicenseUnlock() \
	KeReleaseMutex(&g_CdpLicenseMutex, FALSE)

/* 清全局状态、捕获 .licprot CRC 快照（O-16）、启动每小时推进 T0 的定时器 */
VOID CdpLicenseInitialize(
	_In_ PDRIVER_OBJECT DriverObject,
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt);
VOID CdpLicenseShutdown(_Inout_ PCdp_DRIVER_EXTENSION DriverExt);

/* 验签 + 绑机 + 初始化五元组 + Seal 写全部已挂载 Journal */
NTSTATUS CdpLicenseSetFromBlob(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_reads_bytes_(BlobLength) const UCHAR* Blob,
	_In_ ULONG BlobLength);

NTSTATUS CdpLicenseQueryStatus(
	_Out_writes_bytes_to_(OutLength, *Written) PVOID OutBuffer,
	_In_ ULONG OutLength,
	_Out_ PULONG Written);

/* 导出申请回执：OPS_T / OPS_S / A_MOD + 硬件信息 */
NTSTATUS CdpLicenseExportReceipt(
	_Out_writes_bytes_to_(OutLength, *Written) PVOID OutBuffer,
	_In_ ULONG OutLength,
	_Out_ PULONG Written);

/*
 * 公钥混合加密申请明文，返回 Ciphertext + QrPayload（URI 字符串）。
 * 内核不生成位图；Agent/GUI 将 QrPayload 画成二维码即可。
 * 正式申请：时长模式 duration>0，次数模式 credits>0，混合两者都要；
 * 试用申请：duration/credits 可为 0（由服务端配置覆盖）。
 * QrPrefix：应用层传入的扫码 URI 前缀（须自带 #c= / ?c=），驱动只拼接 Base64。
 */
NTSTATUS CdpLicenseBuildApplyQrPayload(
	_In_ ULONG DesiredDurationSec,
	_In_ ULONG DesiredCredits,
	_In_ ULONG Mode,
	_In_ ULONG Kind,
	_In_reads_(Cdp_APPLY_QR_PREFIX_MAX) const CHAR* QrPrefix,
	_Out_writes_bytes_to_(OutLength, *Written) PVOID OutBuffer,
	_In_ ULONG OutLength,
	_Out_ PULONG Written);

/* Format 新 Journal 后：若内存已有证则写入全部已挂载超级块；无证则成功返回 */
NTSTATUS CdpLicensePersistIfLoaded(
	_In_ PCdp_DRIVER_EXTENSION DriverExt);

/*
 * 回滚/重做前闸门（设计 §6.3 / §6.5）：
 *   完整性 → 解密 E0 → 与 g_* 对照 → 篡改则 A_MOD++ 写回并失败
 *   → 用 l_T0/l_C0 判期/次数 → 通过后 OPS_T++ 写 E0 → 签发 Capability Token
 */
NTSTATUS CdpLicenseGateBeforeOp(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Out_ PCdp_LICENSE_LOCAL_STATE Local);

/* 真正进入 CdpCoreRecoveryBegin 前：完整性 + 用 SealKey 重算 token，成功则 Armed=TRUE */
NTSTATUS CdpLicenseGateArmOp(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_LICENSE_LOCAL_STATE* Local);

/* Begin 失败或 Cancel 时作废 pending token，避免残留 Armed 被 Commit 误用 */
VOID CdpLicenseGateAbortOp(VOID);

/* Commit 成功后：必须 Armed 且 token 可重算，然后 OPS_S++ / C0--，再 Seal 并作废 token */
NTSTATUS CdpLicenseGateAfterOpSuccess(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_LICENSE_LOCAL_STATE Local);

/* 挂载带 License 的 Journal 时，若全局尚未加载则验签并恢复 g_* / 推进 T0 */
NTSTATUS CdpLicenseOnJournalMounted(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_JOURNAL Journal);

/* 仅当启动自动发现到带持久化许可证的 Journal 时，排队延迟恢复。 */
VOID CdpLicenseScheduleDeferredRestore(_In_ PCdp_DRIVER_EXTENSION DriverExt);

#endif /* CDP_LICENSE */
