#pragma once

/*
 * 受保护闸门调用点（混淆设计 §9 O-07～O-10 / O-19 / O-20）。
 *
 * CdpIrpDispatchs.c 处于 I/O 热路径，不能进 .licprot、也不能做代码混淆
 * （CdpLicenseSeg.h:16-17 有明确禁止）。但 BEGIN/COMMIT RECOVERY 的闸门
 * 调用点就在那里——如果直接调用 CdpLicenseGateBeforeOp，攻击者只需改一处
 * 返回码判断，就能让整套授权机制（含 .licprot 完整性校验）完全不被执行。
 *
 * 因此分发文件改为调用下面的转发函数，它们定义在 CdpLicenseGateCall.c，
 * 该单元链入 .licprot/.licpr 并做代码混淆。
 *
 * 这些函数只做转发，不含任何判定逻辑。
 */

#ifdef CDP_LICENSE

#include "CdpEngineDefs.h"
#include "CdpLicenseDefs.h"

NTSTATUS CdpLicenseGateCallBeforeOp(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Out_ PCdp_LICENSE_LOCAL_STATE Local);

NTSTATUS CdpLicenseGateCallArmOp(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_LICENSE_LOCAL_STATE* Local);

NTSTATUS CdpLicenseGateCallAfterOpSuccess(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_LICENSE_LOCAL_STATE Local);

VOID CdpLicenseGateCallAbortOp(VOID);

#endif /* CDP_LICENSE */
