#pragma once

/*
 * v19 密码信封的公开接口。
 * 明文用厂商公钥做 RSA-OAEP-SHA256，驱动侧不能解密；
 * 挂载时只检查算法、密文长度和信封 CRC。
 */

#ifdef CDP_LICENSE

#include "CdpIoctl.h"
#include "CdpJournal.h"

#define Cdp_PASSWORD_ENVELOPE_MAGIC 0x31455750UL /* PWE1 */
#define Cdp_PASSWORD_ENVELOPE_PLAIN_VERSION 1UL

#pragma pack(push, 1)
typedef struct _Cdp_PASSWORD_ENVELOPE_PLAIN
{
	ULONG Magic;
	ULONG Version;
	GUID CredentialId;
	UINT64 AuthEpoch;
	ULONG PasswordBytes;
	UCHAR Password[Cdp_PASSWORD_MAX_UTF8_BYTES];
} Cdp_PASSWORD_ENVELOPE_PLAIN, *PCdp_PASSWORD_ENVELOPE_PLAIN;
#pragma pack(pop)

/* RSA-2048 / OAEP-SHA256 最大明文 190。只加密到口令实际长度。 */
C_ASSERT(
	FIELD_OFFSET(Cdp_PASSWORD_ENVELOPE_PLAIN, Password) +
	Cdp_PASSWORD_MAX_UTF8_BYTES <= 190);

struct _Cdp_DRIVER_EXTENSION;

/* 加密口令并填好信封 CRC。返回前清零明文。 */
NTSTATUS CdpPasswordEnvelopeCreate(
	_In_reads_bytes_(PasswordBytes) const UCHAR* Password,
	_In_ ULONG PasswordBytes,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_ Cdp_PASSWORD_ENVELOPE* Envelope);

/* 算法、长度必须是 RSA-2048 密文 256，且 CRC 覆盖这段已用密文。 */
BOOLEAN CdpPasswordEnvelopeValid(
	_In_ const Cdp_PASSWORD_ENVELOPE* Envelope);

/*
 * 重验口令。已是同一份 v19 信封则直接成功；
 * 存在 v18 时生成或复用一份信封并做 Bundle 事务。
 */
NTSTATUS CdpEnsurePasswordEnvelopes(
	_In_ struct _Cdp_DRIVER_EXTENSION* DriverExt,
	_In_reads_bytes_(PasswordBytes) const UCHAR* Password,
	_In_ ULONG PasswordBytes);

/* 改密：钉住旧凭据匹配的 Journal，逐个写入新 Bundle，失败则回滚。 */
NTSTATUS CdpCommitSharedCredentialBundle(
	_In_ struct _Cdp_DRIVER_EXTENSION* DriverExt,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* OldCredential,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* NewCredential,
	_In_ const Cdp_PASSWORD_ENVELOPE* NewEnvelope);

/* 读取当前共享 Bundle。任一已挂载 Journal 还没有信封时返回 STATUS_REVISION_MISMATCH。 */
NTSTATUS CdpFindSharedPasswordBundle(
	_In_ struct _Cdp_DRIVER_EXTENSION* DriverExt,
	_Out_ Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_ Cdp_PASSWORD_ENVELOPE* Envelope);

#endif
