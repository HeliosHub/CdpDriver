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
 * 只允许存放"转发"逻辑：任何判定、常量或数据结构一旦写在这里，都会同时
 * 进入 CRC 覆盖范围与 .licprot 尺寸统计，反而增加维护成本。真正的判定
 * 仍留在 CdpLicenseGate.c。
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

#include "CdpLicenseSegEnd.h" /* 恢复默认 code/const 节 */
