#pragma once

/*
 * 硬件身份采集与规范化（设计 §3 Device Identity / 服务端 fingerprint.py）。
 *
 * 混淆清单：O-04
 * 指纹算法写死，改动会导致已签发 License 全部失效。
 */

#ifdef CDP_LICENSE
#ifdef CDP_LICENSE_OBFUSCATE

#include "CdpLicenseDefs.h"

/*
 * 采集主板 UUID、系统盘序列号，并计算：
 *   SHA256( norm(mb) || 0x00 || norm(disk) || 0x00 || tpm||0x00 || salt )
 * 当前未采 TPM / product_salt（空段仍保留分隔 0x00，与服务端一致）。
 */
NTSTATUS CdpLicenseCollectHardwareId(
	_Out_writes_(Cdp_LICENSE_MB_UUID_CHARS) CHAR* MbUuid,
	_Out_writes_(Cdp_LICENSE_DISK_SERIAL_CHARS) CHAR* DiskSerial,
	_Out_writes_bytes_(Cdp_LICENSE_FP_BYTES) UCHAR* Fingerprint);

/* 小写、去花括号与空白 */
VOID CdpLicenseNormalizeMbUuid(
	_In_z_ const CHAR* In,
	_Out_writes_(Cdp_LICENSE_MB_UUID_CHARS) CHAR* Out);

/* Trim 后转大写 */
VOID CdpLicenseNormalizeDiskSerial(
	_In_z_ const CHAR* In,
	_Out_writes_(Cdp_LICENSE_DISK_SERIAL_CHARS) CHAR* Out);

#endif /* CDP_LICENSE_OBFUSCATE */
#endif /* CDP_LICENSE */
