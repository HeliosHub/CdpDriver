/*
 * 授权闸门"受保护调用点"包装（混淆设计 §9 O-07～O-10 / O-19 / O-20）。
 *
 * 为什么需要这个文件：
 *   CdpIrpDispatchs.c 是所有 IOCTL 的分发入口，因为处在 I/O 热路径上，
 *   明确不进 .licprot、不做代码混淆（见 CdpLicenseSeg.h 的禁止清单）。
 *   但 BEGIN/COMMIT RECOVERY 的闸门调用恰好也在那个文件里：
 *
 *       status = CdpLicenseGateBeforeOp(DriverExt, &licenseLocal);
 *       if (!NT_SUCCESS(status)) { ... return status; }
 *
 *   于是攻击者只要把这一处返回码改成 STATUS_SUCCESS，CdpLicenseGate /
 *   CdpLocalSeal / CdpLicenseProtect 里全部机制（E0 解密、水位对照、
 *   Capability Token、.licprot CRC 完整性校验）一次都不会被执行——
 *   混淆得再强也没有意义。
 *
 * 解决办法：把调用点收进本单元。本文件链入 .licprot/.licpr（被 O-16 的
 * 完整性哈希覆盖），并带 -string-obfus -const-obfus -fla。CdpIrpDispatchs.c
 * 改为只调用这里的转发函数，因此"决策点"重新回到受保护 + 被控制流平坦化
 * 的范围内，而分发文件的代码与性能特征保持不变。
 *
 * 这里仅承接恢复闸门转发与许可证 IOCTL 的最小路由/长度校验；授权判定
 * 仍留在 CdpLicenseGate.c。这样既收紧静态边界，又不会把 I/O 热路径搬进
 * .licprot。
 */

#include "CdpLicenseGate.h"

#include "CdpLicenseSeg.h" /* 此后本文件代码进入 .licprot */

NTSTATUS CdpLicenseGateCallBeforeOp(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Out_ PCdp_LICENSE_LOCAL_STATE Local)
{
	return CdpLicenseGateBeforeOp(DriverExt, Local);
}

NTSTATUS CdpLicenseGateCallArmOp(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_LICENSE_LOCAL_STATE* Local)
{
	return CdpLicenseGateArmOp(DriverExt, Local);
}

NTSTATUS CdpLicenseGateCallAfterOpSuccess(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_LICENSE_LOCAL_STATE Local)
{
	return CdpLicenseGateAfterOpSuccess(DriverExt, Local);
}

VOID CdpLicenseGateCallAbortOp(VOID)
{
	CdpLicenseGateAbortOp();
}

BOOLEAN CdpLicenseGateCallTryDispatchIoctl(
	_In_opt_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG IoControlCode,
	_Inout_opt_ PVOID SystemBuffer,
	_In_ ULONG InputLength,
	_In_ ULONG OutputLength,
	_Out_ PNTSTATUS Status,
	_Out_ PULONG Information)
{
	if (!Status || !Information)
		return FALSE;

	*Information = 0;
	switch (IoControlCode)
	{
	case IOCTL_Cdp_SET_LICENSE:
	{
		PCdp_SET_LICENSE_REQUEST request;
		if (!DriverExt || !SystemBuffer ||
			InputLength < sizeof(*request))
		{
			*Status = STATUS_BUFFER_TOO_SMALL;
			return TRUE;
		}
		request = (PCdp_SET_LICENSE_REQUEST)SystemBuffer;
		if (request->LicenseLength == 0 ||
			request->LicenseLength > Cdp_LICENSE_BLOB_MAX)
		{
			*Status = STATUS_INVALID_PARAMETER;
			return TRUE;
		}
		*Status = CdpLicenseSetFromBlob(
			DriverExt, request->LicenseBlob, request->LicenseLength);
		return TRUE;
	}

	case IOCTL_Cdp_QUERY_LICENSE:
		if (!SystemBuffer)
		{
			*Status = STATUS_BUFFER_TOO_SMALL;
			return TRUE;
		}
		*Status = CdpLicenseQueryStatus(SystemBuffer, OutputLength, Information);
		return TRUE;

	case IOCTL_Cdp_EXPORT_RECEIPT:
		if (!SystemBuffer)
		{
			*Status = STATUS_BUFFER_TOO_SMALL;
			return TRUE;
		}
		*Status = CdpLicenseExportReceipt(SystemBuffer, OutputLength, Information);
		return TRUE;

	case IOCTL_Cdp_BUILD_APPLY_QR:
	{
		Cdp_BUILD_APPLY_QR_REQUEST request;
		if (!SystemBuffer || InputLength < sizeof(request) ||
			OutputLength < sizeof(Cdp_LICENSE_APPLY_QR_REPLY))
		{
			*Status = STATUS_BUFFER_TOO_SMALL;
			return TRUE;
		}
		request = *(PCdp_BUILD_APPLY_QR_REQUEST)SystemBuffer;
		*Status = CdpLicenseBuildApplyQrPayload(request.DesiredDurationSec,
			request.DesiredCredits, request.Mode, request.Kind, request.QrPrefix,
			SystemBuffer, OutputLength, Information);
		return TRUE;
	}

	default:
		return FALSE;
	}
}

#include "CdpLicenseSegEnd.h" /* 恢复默认 code/const 节 */
