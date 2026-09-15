#pragma once

/*
 * Vendor 公钥信任根 + CNG 非对称/对称封装（设计 §3.1 / §8）。
 *
 * 混淆清单：O-01 / O-02 / O-03
 *  - 申请方向：RSA-OAEP(AES-key) + AES-GCM（与 server hybrid.py 一致）
 *  - License 方向：RSA-PSS 验签 canonical JSON（禁止 blob 自带公钥自验）
 */

#ifdef CDP_LICENSE
#ifdef CDP_LICENSE_OBFUSCATE

#include "CdpLicenseDefs.h"

/* 双分片 XOR 组装 BCRYPT_RSAPUBLIC_BLOB；两副本交叉不一致则拒签 */
NTSTATUS CdpLicenseTrustAssemblePublicKey(
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength);

/* RSA-OAEP-SHA256 加密（申请混合加密中的 AES-256 密钥） */
NTSTATUS CdpLicenseRsaOaepEncrypt(
	_In_reads_bytes_(PlainLength) const UCHAR* Plain,
	_In_ ULONG PlainLength,
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength);

/* AES-256-GCM；输出 = ciphertext || tag(16)，AAD 由调用方提供 */
NTSTATUS CdpLicenseAesGcmEncrypt(
	_In_reads_bytes_(32) const UCHAR* AesKey,
	_In_reads_bytes_(12) const UCHAR* Nonce,
	_In_reads_bytes_(AadLength) const UCHAR* Aad,
	_In_ ULONG AadLength,
	_In_reads_bytes_(PlainLength) const UCHAR* Plain,
	_In_ ULONG PlainLength,
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength);

/*
 * 验签对象：长度前缀 canonical JSON（不含 signature 字段）。
 * 内部先 SHA-256(canonical)，再 BCryptVerifySignature（CNG 要求传摘要而非原文）。
 * salt_length = 222，对齐 cryptography PSS.MAX_LENGTH（RSA-2048/SHA-256）。
 */
NTSTATUS CdpLicenseVerifySignedCanonical(
	_In_reads_bytes_(CanonicalLength) const UCHAR* CanonicalWithoutSig,
	_In_ ULONG CanonicalLength,
	_In_reads_bytes_(SignatureLength) const UCHAR* Signature,
	_In_ ULONG SignatureLength);

NTSTATUS CdpLicenseSha256(
	_In_reads_bytes_(Length) const UCHAR* Data,
	_In_ ULONG Length,
	_Out_writes_bytes_(32) UCHAR* Digest);

NTSTATUS CdpLicenseRandomBytes(
	_Out_writes_bytes_(Length) UCHAR* Buffer,
	_In_ ULONG Length);

#endif /* CDP_LICENSE_OBFUSCATE */
#endif /* CDP_LICENSE */
