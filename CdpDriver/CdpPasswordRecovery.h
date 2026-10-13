#pragma once

/*
 * 忘记口令后的找回入口。
 * 二维码不要求旧口令；导入只武装当前句柄；改密成功后共享凭据的 AuthEpoch 加一。
 * CdpProtect 不调用这里，也不读取找回凭据或 jti。
 */

#include "CdpIoctl.h"

struct _Cdp_DRIVER_EXTENSION;
struct _Cdp_CONTROL_FILE_CONTEXT;

/* 采集指纹和当前信封，生成一次性找回二维码，并把 nonce 记在本句柄上。 */
NTSTATUS CdpBuildRecoveryQr(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const Cdp_BUILD_RECOVERY_QR_REQUEST* Request,
	Cdp_RECOVERY_QR_REPLY* Reply);

/* 验签并核对凭据。同一句柄在武装窗口内重复导入同一 jti 视为成功。 */
NTSTATUS CdpImportRecoveryCredential(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const UCHAR* Credential,
	ULONG CredentialLength);

/* 用已武装的句柄提交新口令。失败不撤武装，成功后旧口令失效。 */
NTSTATUS CdpChangePasswordWithRecovery(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const UCHAR* Password,
	ULONG PasswordLength);

/* CLEANUP / CLOSE 时清掉 nonce、jti 摘要和武装标记。 */
VOID CdpRecoveryClearFileContext(
	struct _Cdp_CONTROL_FILE_CONTEXT* Context);
