#pragma once

/*
 * CdpLocalSeal-v1（设计 §7）—— 仅用于生成本地状态密文 E0，不用于 License 防伪。
 *
 * 混淆清单：O-05 / O-06 / O-11 / O-12 / O-13
 */

#ifdef CDP_LICENSE
#ifdef CDP_LICENSE_OBFUSCATE

#include "CdpLicenseDefs.h"

/* K = SHA256( fingerprint || BE64(T_EXP) || PRODUCT_MAGIC[16] ) */
NTSTATUS CdpLocalSealDeriveKey(
	_In_reads_bytes_(Cdp_LICENSE_FP_BYTES) const UCHAR* HardwareId,
	_In_ UINT64 TExp100ns,
	_Out_writes_bytes_(32) UCHAR* KeyOut);

/*
 * 确定性密封：IV = SHA256(K || "e0-iv")[0..15]，便于复现对照；
 * 布局：magic|version|reserved|iv|ciphertext|tag(16)。
 */
NTSTATUS CdpLocalSealEncrypt(
	_In_reads_bytes_(32) const UCHAR* Key,
	_In_ const Cdp_E0_PLAINTEXT* Plain,
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength);

/* 验 tag → Feistel 解密 → 校验明文 CRC；推荐用于 E0↔内存对照 */
NTSTATUS CdpLocalSealDecrypt(
	_In_reads_bytes_(32) const UCHAR* Key,
	_In_reads_bytes_(InLength) const UCHAR* In,
	_In_ ULONG InLength,
	_Out_ PCdp_E0_PLAINTEXT Plain);

VOID CdpLocalSealSecureZero(
	_Out_writes_bytes_(Length) PVOID Buffer,
	_In_ SIZE_T Length);

/*
 * Capability Token（O-18）：
 *   SHA256(SealKey || "cdp-cap-v1" || T0,C0,OPS_T,OPS_S,A_MOD || nonce || issued)
 * 字段以小端写入材料缓冲。仅复用 E0 的 SealKey，不另派生密钥。
 * CdpLocalSealCapTokenEqual 为恒定时间比较。
 */
NTSTATUS CdpLocalSealMakeCapToken(
	_In_reads_bytes_(32) const UCHAR* Key,
	_In_ UINT64 T0,
	_In_ ULONG C0,
	_In_ ULONG OpsT,
	_In_ ULONG OpsS,
	_In_ ULONG AMod,
	_In_ ULONG Nonce,
	_In_ UINT64 Issued100ns,
	_Out_writes_bytes_(Cdp_CAP_TOKEN_BYTES) UCHAR* TokenOut);

BOOLEAN CdpLocalSealCapTokenEqual(
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* A,
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* B);

ULONG CdpLicenseCrc32c(
	_In_ ULONG Crc,
	_In_reads_bytes_(Length) const VOID* Buffer,
	_In_ ULONG Length);

#endif /* CDP_LICENSE_OBFUSCATE */
#endif /* CDP_LICENSE */
