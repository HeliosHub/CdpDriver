/*
 * 离线找回口令。
 * 驱动用厂商公钥加密找回请求、验厂商对重设凭据的 RSA-PSS 签名，
 * 然后在 15 分钟的句柄窗口里把共享凭据换成新口令。
 * 不记录明文口令、找回 JSON、jti 原文或信封明文。
 * 非授权构建三个入口都返回 STATUS_NOT_SUPPORTED。
 */

#include "CdpEngineDefs.h"
#include "CdpPasswordRecovery.h"
#include "CdpPasswordRecoveryPolicy.h"
#include "..\CdpCore\include\cdp_alloc.h"
#include <ntstrsafe.h>

#if defined(CDP_LICENSE) && defined(CDP_LICENSE_OBFUSCATE)
#include "CdpPasswordEnvelope.h"
#include "CdpCredential.h"
#include "CdpLicenseTrust.h"
#include "CdpLicenseHw.h"
#include "CdpLicenseGate.h"
#endif

/* 句柄武装窗口走单调时钟，15 分钟，不受墙钟回拨影响。 */
#define CDP_RECOVERY_ARM_100NS (15ULL * 60ULL * 10000000ULL)
/* 本次启动内记住的 jti 摘要。重启后旧凭据仍会因 AuthEpoch 加一而失效。 */
#define CDP_RECOVERY_SEEN_JTI 32u
#define CDP_RECOVERY_UNIX_EPOCH_100NS 116444736000000000ULL

/* 清掉本句柄上的找回材料。已经完成的认证字段保持不动。 */
VOID CdpRecoveryClearFileContext(
	struct _Cdp_CONTROL_FILE_CONTEXT* Context)
{
	PCdp_CONTROL_FILE_CONTEXT context = (PCdp_CONTROL_FILE_CONTEXT)Context;

	if (!context)
		return;
	context->RecoveryResetArmed = FALSE;
	context->RecoveryNonceValid = FALSE;
	RtlSecureZeroMemory(context->RecoveryRequestNonce,
		sizeof(context->RecoveryRequestNonce));
	RtlSecureZeroMemory(context->RecoveryJtiHash, sizeof(context->RecoveryJtiHash));
	context->RecoveryExpiresAt100ns = 0;
	RtlSecureZeroMemory(&context->RecoveryCredentialId,
		sizeof(context->RecoveryCredentialId));
	context->RecoveryAuthEpoch = 0;
}

#if defined(CDP_LICENSE) && defined(CDP_LICENSE_OBFUSCATE)

static KSPIN_LOCK g_CdpRecoverySeenLock;
static volatile LONG g_CdpRecoverySeenReady;
static UCHAR g_CdpRecoverySeen[CDP_RECOVERY_SEEN_JTI][CDP_RECOVERY_JTI_HASH_LEN];
static ULONG g_CdpRecoverySeenCount;
static ULONG g_CdpRecoverySeenNext;

/* 二维码组装用的临时缓冲，放在池里，避免撑满内核栈。 */
typedef struct _CDP_RECOVERY_QR_WORK
{
	CHAR Plain[768];
	UCHAR Ct[784];
	UCHAR Packet[1280];
	CHAR B64[2048];
} CDP_RECOVERY_QR_WORK;

/* 第一次使用时初始化已见 jti 表的自旋锁。 */
static VOID CdpRecoveryEnsureSeenLock(VOID)
{
	if (InterlockedCompareExchange(&g_CdpRecoverySeenReady, 1, 0) == 0)
	{
		KeInitializeSpinLock(&g_CdpRecoverySeenLock);
		InterlockedExchange(&g_CdpRecoverySeenReady, 2);
		return;
	}
	while (InterlockedCompareExchange(&g_CdpRecoverySeenReady, 2, 2) != 2)
		KeStallExecutionProcessor(1);
}

/* 固定 32 字节比较，不在第一个差异处返回。 */
static BOOLEAN CdpRecoveryHashEqual(
	const UCHAR* Left,
	const UCHAR* Right)
{
	ULONG i;
	UCHAR diff = 0;

	for (i = 0; i < CDP_RECOVERY_JTI_HASH_LEN; ++i)
		diff |= (UCHAR)(Left[i] ^ Right[i]);
	return diff == 0;
}

/* 本次启动是否已经接受过这份 jti。扫完全表，不提前退出。 */
static BOOLEAN CdpRecoveryJtiSeenLocked(const UCHAR* Hash)
{
	ULONG i;
	BOOLEAN found = FALSE;

	for (i = 0; i < g_CdpRecoverySeenCount && i < CDP_RECOVERY_SEEN_JTI; ++i)
	{
		if (CdpRecoveryHashEqual(g_CdpRecoverySeen[i], Hash))
			found = TRUE;
	}
	return found;
}

/* 查重并记下新 jti。已经见过则返回 FALSE，调用方不得武装句柄。 */
static BOOLEAN CdpRecoveryTryRememberJti(const UCHAR* Hash)
{
	KIRQL irql;
	BOOLEAN fresh;

	CdpRecoveryEnsureSeenLock();
	KeAcquireSpinLock(&g_CdpRecoverySeenLock, &irql);
	fresh = !CdpRecoveryJtiSeenLocked(Hash);
	if (fresh)
	{
		RtlCopyMemory(g_CdpRecoverySeen[g_CdpRecoverySeenNext], Hash,
			CDP_RECOVERY_JTI_HASH_LEN);
		g_CdpRecoverySeenNext = (g_CdpRecoverySeenNext + 1) % CDP_RECOVERY_SEEN_JTI;
		if (g_CdpRecoverySeenCount < CDP_RECOVERY_SEEN_JTI)
			++g_CdpRecoverySeenCount;
	}
	KeReleaseSpinLock(&g_CdpRecoverySeenLock, irql);
	return fresh;
}

/* 把策略码换成驱动要返回的 NTSTATUS。 */
static NTSTATUS CdpRecoveryMapPolicy(int Code)
{
	switch (Code)
	{
	case CDP_RECOVERY_OK:
		return STATUS_SUCCESS;
	case CDP_RECOVERY_BAD_FORMAT:
		return STATUS_INVALID_PARAMETER;
	case CDP_RECOVERY_BAD_TYP:
	case CDP_RECOVERY_BAD_VER:
	case CDP_RECOVERY_EPOCH:
		return STATUS_REVISION_MISMATCH;
	case CDP_RECOVERY_EXPIRED:
	case CDP_RECOVERY_BAD_TIME:
		return STATUS_ACCOUNT_EXPIRED;
	default:
		return STATUS_ACCESS_DENIED;
	}
}

/* 小写十六进制，供 JSON 使用。 */
static VOID CdpRecoveryHexEncode(
	const UCHAR* In,
	ULONG Count,
	CHAR* Out)
{
	static const CHAR kHex[] = "0123456789abcdef";
	ULONG i;

	for (i = 0; i < Count; ++i)
	{
		Out[i * 2] = kHex[In[i] >> 4];
		Out[i * 2 + 1] = kHex[In[i] & 0x0F];
	}
	Out[Count * 2] = 0;
}

/* 与申请二维码相同的 Base64，不含换行。 */
static NTSTATUS CdpRecoveryBase64(
	const UCHAR* In,
	ULONG InLength,
	CHAR* Out,
	ULONG OutCap,
	PULONG OutLength)
{
	static const CHAR kTable[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	ULONG i;
	ULONG o = 0;

	*OutLength = 0;
	for (i = 0; i < InLength; i += 3)
	{
		ULONG remain = InLength - i;
		ULONG n = ((ULONG)In[i]) << 16;

		if (remain > 1)
			n |= ((ULONG)In[i + 1]) << 8;
		if (remain > 2)
			n |= (ULONG)In[i + 2];
		if (o + 4 >= OutCap)
			return STATUS_BUFFER_TOO_SMALL;
		Out[o++] = kTable[(n >> 18) & 63];
		Out[o++] = kTable[(n >> 12) & 63];
		Out[o++] = remain > 1 ? kTable[(n >> 6) & 63] : '=';
		Out[o++] = remain > 2 ? kTable[n & 63] : '=';
	}
	if (o >= OutCap)
		return STATUS_BUFFER_TOO_SMALL;
	Out[o] = 0;
	*OutLength = o;
	return STATUS_SUCCESS;
}

/* 前缀必须是可打印 ASCII，并在缓冲内以 0 结尾。 */
static NTSTATUS CdpRecoverySanitizePrefix(
	const CHAR* In,
	CHAR* Out)
{
	ULONG i;

	if (!In || !Out)
		return STATUS_INVALID_PARAMETER;
	RtlZeroMemory(Out, Cdp_RECOVERY_QR_PREFIX_MAX);
	for (i = 0; i + 1 < Cdp_RECOVERY_QR_PREFIX_MAX; ++i)
	{
		UCHAR c = (UCHAR)In[i];

		if (c == 0)
			break;
		if (c < 0x20 || c > 0x7E)
			return STATUS_INVALID_PARAMETER;
		Out[i] = (CHAR)c;
	}
	if (Out[0] == 0)
		return STATUS_INVALID_PARAMETER;
	return STATUS_SUCCESS;
}

/* GUID 写成不带花括号的小写文本，与服务端 uuid 字符串一致。 */
static VOID CdpRecoveryFormatGuid(const GUID* Id, CHAR* Out, ULONG OutCap)
{
	RtlStringCbPrintfA(Out, OutCap,
		"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		Id->Data1, Id->Data2, Id->Data3,
		Id->Data4[0], Id->Data4[1], Id->Data4[2], Id->Data4[3],
		Id->Data4[4], Id->Data4[5], Id->Data4[6], Id->Data4[7]);
}

/* 墙钟转 Unix 秒，只用于凭据的 iat/exp，不用于 15 分钟窗口。 */
static NTSTATUS CdpRecoveryUnixNow(long long* UnixOut)
{
	LARGE_INTEGER now;

	KeQuerySystemTimePrecise(&now);
	if ((UINT64)now.QuadPart < CDP_RECOVERY_UNIX_EPOCH_100NS)
		return STATUS_INVALID_PARAMETER;
	*UnixOut = (long long)(
		((UINT64)now.QuadPart - CDP_RECOVERY_UNIX_EPOCH_100NS) / 10000000ULL);
	return STATUS_SUCCESS;
}

/* 生成找回请求密文，成功后才把 16 字节 nonce 记到句柄上。 */
NTSTATUS CdpBuildRecoveryQr(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const Cdp_BUILD_RECOVERY_QR_REQUEST* Request,
	Cdp_RECOVERY_QR_REPLY* Reply)
{
	PCdp_CONTROL_FILE_CONTEXT context = (PCdp_CONTROL_FILE_CONTEXT)Context;
	Cdp_CREDENTIAL_DESCRIPTOR credential;
	Cdp_PASSWORD_ENVELOPE envelope;
	CDP_RECOVERY_QR_WORK* work = NULL;
	CHAR prefix[Cdp_RECOVERY_QR_PREFIX_MAX];
	CHAR email[Cdp_EMAIL_MAX];
	CHAR guid[40];
	CHAR tClient[40];
	CHAR fpHex[65];
	CHAR envHex[65];
	CHAR nonceHex[33];
	CHAR mb[Cdp_LICENSE_MB_UUID_CHARS];
	CHAR disk[Cdp_LICENSE_DISK_SERIAL_CHARS];
	UCHAR fingerprint[Cdp_RECOVERY_FP_BYTES];
	UCHAR requestNonce[16];
	UCHAR aesKey[32];
	UCHAR gcmNonce[12];
	UCHAR ek[256];
	UCHAR digest[32];
	ULONG ekLen = 0;
	ULONG ctLen = 0;
	ULONG packetLen = 0;
	ULONG b64Len = 0;
	ULONG plainLen;
	UCHAR aad = 0x01;
	TIME_FIELDS tf;
	LARGE_INTEGER wall;
	NTSTATUS status = STATUS_UNSUCCESSFUL;

	if (!DriverExt || !context || !Request || !Reply)
		return STATUS_INVALID_PARAMETER;
	RtlZeroMemory(Reply, sizeof(*Reply));
	RtlZeroMemory(&credential, sizeof(credential));
	RtlZeroMemory(&envelope, sizeof(envelope));
	RtlZeroMemory(prefix, sizeof(prefix));
	RtlZeroMemory(email, sizeof(email));
	RtlZeroMemory(requestNonce, sizeof(requestNonce));
	RtlZeroMemory(aesKey, sizeof(aesKey));

	/* 没有共享 v19 信封就不能证明这台机器持有可找回的凭据。 */
	status = CdpFindSharedPasswordBundle(
		(PCdp_DRIVER_EXTENSION)DriverExt, &credential, &envelope);
	if (!NT_SUCCESS(status))
		goto done;
	if (envelope.CiphertextLength != Cdp_PASSWORD_ENVELOPE_MAX)
	{
		status = STATUS_REVISION_MISMATCH;
		goto done;
	}
	status = CdpRecoverySanitizePrefix(Request->QrPrefix, prefix);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpLicenseSanitizeEmail(Request->Email, email);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpLicenseCollectHardwareId(mb, disk, fingerprint);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpLicenseSha256(
		envelope.Ciphertext, envelope.CiphertextLength, digest);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpLicenseRandomBytes(requestNonce, sizeof(requestNonce));
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpLicenseRandomBytes(aesKey, sizeof(aesKey));
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpLicenseRandomBytes(gcmNonce, sizeof(gcmNonce));
	if (!NT_SUCCESS(status))
		goto done;

	CdpRecoveryFormatGuid(&credential.CredentialId, guid, sizeof(guid));
	CdpRecoveryHexEncode(fingerprint, sizeof(fingerprint), fpHex);
	CdpRecoveryHexEncode(digest, sizeof(digest), envHex);
	CdpRecoveryHexEncode(requestNonce, sizeof(requestNonce), nonceHex);
	KeQuerySystemTimePrecise(&wall);
	RtlTimeToTimeFields(&wall, &tf);
	RtlStringCbPrintfA(tClient, sizeof(tClient),
		"%04d-%02d-%02dT%02d:%02d:%02dZ",
		tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second);

	work = (CDP_RECOVERY_QR_WORK*)Cdp_ALLOC(sizeof(*work));
	if (!work)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}
	RtlZeroMemory(work, sizeof(*work));
	/* 键按字典序拼好，服务端按同一 canonical 规则验字段。 */
	status = RtlStringCbPrintfA(work->Plain, sizeof(work->Plain),
		"{\"auth_epoch\":%I64u,\"credential_id\":\"%s\",\"email\":\"%s\","
		"\"envelope_sha256\":\"%s\",\"fp\":\"%s\",\"nonce\":\"%s\",\"t_client\":\"%s\","
		"\"typ\":\"cdp-protect-recovery-v1\",\"ver\":1}",
		credential.AuthEpoch, guid, email, envHex, fpHex, nonceHex, tClient);
	if (!NT_SUCCESS(status))
		goto done;
	plainLen = 0;
	while (plainLen < sizeof(work->Plain) && work->Plain[plainLen])
		++plainLen;

	status = CdpLicenseRsaOaepEncrypt(aesKey, sizeof(aesKey), ek, sizeof(ek), &ekLen);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpLicenseAesGcmEncrypt(
		aesKey, gcmNonce, &aad, 1,
		(const UCHAR*)work->Plain, plainLen,
		work->Ct, sizeof(work->Ct), &ctLen);
	if (!NT_SUCCESS(status))
		goto done;
	if (1u + 2u + ekLen + 12u + ctLen > sizeof(work->Packet) ||
		1u + 2u + ekLen + 12u + ctLen > sizeof(Reply->Ciphertext))
	{
		status = STATUS_BUFFER_TOO_SMALL;
		goto done;
	}
	work->Packet[0] = 0x01;
	work->Packet[1] = (UCHAR)((ekLen >> 8) & 0xFF);
	work->Packet[2] = (UCHAR)(ekLen & 0xFF);
	RtlCopyMemory(work->Packet + 3, ek, ekLen);
	RtlCopyMemory(work->Packet + 3 + ekLen, gcmNonce, 12);
	RtlCopyMemory(work->Packet + 3 + ekLen + 12, work->Ct, ctLen);
	packetLen = 3 + ekLen + 12 + ctLen;
	status = CdpRecoveryBase64(
		work->Packet, packetLen, work->B64, sizeof(work->B64), &b64Len);
	if (!NT_SUCCESS(status))
		goto done;
	/* 前缀里可能有 %，所以用拷贝而不是 printf 拼接。 */
	status = RtlStringCbCopyA(Reply->QrPayload, sizeof(Reply->QrPayload), prefix);
	if (!NT_SUCCESS(status))
		goto done;
	status = RtlStringCbCatA(Reply->QrPayload, sizeof(Reply->QrPayload), work->B64);
	if (!NT_SUCCESS(status))
		goto done;
	Reply->CiphertextLength = packetLen;
	RtlCopyMemory(Reply->Ciphertext, work->Packet, packetLen);
	RtlCopyMemory(Reply->DeviceFingerprint, fingerprint, sizeof(fingerprint));

	/* 新的二维码替换本句柄上一次的 nonce，并取消尚未使用的武装。 */
	CdpRecoveryClearFileContext(Context);
	RtlCopyMemory(context->RecoveryRequestNonce, requestNonce, sizeof(requestNonce));
	context->RecoveryNonceValid = TRUE;
	status = STATUS_SUCCESS;

done:
	if (!NT_SUCCESS(status) && Reply)
		RtlSecureZeroMemory(Reply, sizeof(*Reply));
	if (work)
	{
		RtlSecureZeroMemory(work, sizeof(*work));
		Cdp_FREE(work);
	}
	RtlSecureZeroMemory(&credential, sizeof(credential));
	RtlSecureZeroMemory(&envelope, sizeof(envelope));
	RtlSecureZeroMemory(aesKey, sizeof(aesKey));
	RtlSecureZeroMemory(requestNonce, sizeof(requestNonce));
	RtlSecureZeroMemory(mb, sizeof(mb));
	RtlSecureZeroMemory(disk, sizeof(disk));
	RtlSecureZeroMemory(fingerprint, sizeof(fingerprint));
	RtlSecureZeroMemory(digest, sizeof(digest));
	RtlSecureZeroMemory(ek, sizeof(ek));
	return status;
}

/* 验签、核对声明，成功后只武装当前句柄。 */
NTSTATUS CdpImportRecoveryCredential(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const UCHAR* Credential,
	ULONG CredentialLength)
{
	PCdp_CONTROL_FILE_CONTEXT context = (PCdp_CONTROL_FILE_CONTEXT)Context;
	Cdp_CREDENTIAL_DESCRIPTOR current;
	Cdp_PASSWORD_ENVELOPE envelope;
	CDP_RECOVERY_CLAIM claim;
	CDP_RECOVERY_EXPECT expect;
	const UCHAR* json = NULL;
	const UCHAR* signature = NULL;
	UCHAR* canonical = NULL;
	CHAR guid[40];
	CHAR mb[Cdp_LICENSE_MB_UUID_CHARS];
	CHAR disk[Cdp_LICENSE_DISK_SERIAL_CHARS];
	UCHAR fingerprint[Cdp_RECOVERY_FP_BYTES];
	unsigned int jsonLen = 0;
	unsigned int sigLen = 0;
	long long nowUnix = 0;
	int policy;
	NTSTATUS status;

	if (!DriverExt || !context || !Credential || CredentialLength == 0)
		return STATUS_INVALID_PARAMETER;
	RtlZeroMemory(&claim, sizeof(claim));
	RtlZeroMemory(&current, sizeof(current));
	RtlZeroMemory(&envelope, sizeof(envelope));
	RtlZeroMemory(&expect, sizeof(expect));

	policy = CdpRecoverySplitCredential(
		Credential, CredentialLength, &json, &jsonLen, &signature, &sigLen);
	if (policy != CDP_RECOVERY_OK)
	{
		status = CdpRecoveryMapPolicy(policy);
		goto done;
	}
	canonical = (UCHAR*)Cdp_ALLOC(4u + jsonLen);
	if (!canonical)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}
	/* 服务端签名的是 u32BE 长度 + JSON，不是文件里再包一层之后的整段。 */
	canonical[0] = (UCHAR)((jsonLen >> 24) & 0xFF);
	canonical[1] = (UCHAR)((jsonLen >> 16) & 0xFF);
	canonical[2] = (UCHAR)((jsonLen >> 8) & 0xFF);
	canonical[3] = (UCHAR)(jsonLen & 0xFF);
	RtlCopyMemory(canonical + 4, json, jsonLen);
	status = CdpLicenseVerifySignedCanonical(
		canonical, 4u + jsonLen, signature, sigLen);
	RtlSecureZeroMemory(canonical, 4u + jsonLen);
	Cdp_FREE(canonical);
	canonical = NULL;
	if (!NT_SUCCESS(status))
		goto done;

	policy = CdpRecoveryParseClaim(json, jsonLen, &claim);
	if (policy != CDP_RECOVERY_OK)
	{
		status = CdpRecoveryMapPolicy(policy);
		goto done;
	}
	/* 同一句柄、同一 jti、窗口还没过：改密失败后的重复导入直接成功。 */
	if (context->RecoveryResetArmed &&
		KeQueryInterruptTime() < context->RecoveryExpiresAt100ns &&
		CdpRecoveryHashEqual(context->RecoveryJtiHash, claim.JtiHash))
	{
		status = STATUS_SUCCESS;
		goto done;
	}
	if (!context->RecoveryNonceValid)
	{
		status = STATUS_ACCESS_DENIED;
		goto done;
	}

	status = CdpFindSharedPasswordBundle(
		(PCdp_DRIVER_EXTENSION)DriverExt, &current, &envelope);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpLicenseCollectHardwareId(mb, disk, fingerprint);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpRecoveryUnixNow(&nowUnix);
	if (!NT_SUCCESS(status))
		goto done;
	CdpRecoveryFormatGuid(&current.CredentialId, guid, sizeof(guid));
	expect.Fingerprint = fingerprint;
	expect.CredentialId = guid;
	expect.AuthEpoch = current.AuthEpoch;
	expect.Nonce = context->RecoveryRequestNonce;
	expect.NowUnix = nowUnix;
	expect.SeenJtiHashes = NULL;
	expect.SeenCount = 0;
	policy = CdpRecoveryCheckClaim(&claim, &expect);
	if (policy != CDP_RECOVERY_OK)
	{
		status = CdpRecoveryMapPolicy(policy);
		goto done;
	}

	/* 策略通过后才占住 jti。并发的第二份相同凭据在这里被拒绝。 */
	if (!CdpRecoveryTryRememberJti(claim.JtiHash))
	{
		status = STATUS_ACCESS_DENIED;
		goto done;
	}
	context->RecoveryResetArmed = TRUE;
	RtlCopyMemory(context->RecoveryJtiHash, claim.JtiHash, sizeof(claim.JtiHash));
	context->RecoveryExpiresAt100ns =
		KeQueryInterruptTime() + CDP_RECOVERY_ARM_100NS;
	context->RecoveryCredentialId = current.CredentialId;
	context->RecoveryAuthEpoch = current.AuthEpoch;
	/* nonce 只用一次。同一 jti 的重复导入走上面的武装短路，不再看 nonce。 */
	context->RecoveryNonceValid = FALSE;
	RtlSecureZeroMemory(context->RecoveryRequestNonce,
		sizeof(context->RecoveryRequestNonce));
	status = STATUS_SUCCESS;

done:
	if (!NT_SUCCESS(status))
		Cdp_LOG("[RECOVERY] import failed status=0x%08X\n", status);
	if (canonical)
		Cdp_FREE(canonical);
	RtlSecureZeroMemory(&claim, sizeof(claim));
	RtlSecureZeroMemory(&current, sizeof(current));
	RtlSecureZeroMemory(&envelope, sizeof(envelope));
	RtlSecureZeroMemory(mb, sizeof(mb));
	RtlSecureZeroMemory(disk, sizeof(disk));
	RtlSecureZeroMemory(fingerprint, sizeof(fingerprint));
	return status;
}

/* 武装窗口内写入新口令。提交失败时保留武装，便于安装器重试。 */
NTSTATUS CdpChangePasswordWithRecovery(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const UCHAR* Password,
	ULONG PasswordLength)
{
	PCdp_CONTROL_FILE_CONTEXT context = (PCdp_CONTROL_FILE_CONTEXT)Context;
	Cdp_CREDENTIAL_DESCRIPTOR oldCredential;
	Cdp_CREDENTIAL_DESCRIPTOR newCredential;
	Cdp_PASSWORD_ENVELOPE envelope;
	NTSTATUS status;

	if (!DriverExt || !context || !Password ||
		PasswordLength == 0 || PasswordLength > Cdp_PASSWORD_MAX_UTF8_BYTES)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (!context->RecoveryResetArmed ||
		KeQueryInterruptTime() >= context->RecoveryExpiresAt100ns)
	{
		return STATUS_ACCESS_DENIED;
	}

	RtlZeroMemory(&oldCredential, sizeof(oldCredential));
	RtlZeroMemory(&newCredential, sizeof(newCredential));
	RtlZeroMemory(&envelope, sizeof(envelope));
	status = CdpFindSharedPasswordBundle(
		(PCdp_DRIVER_EXTENSION)DriverExt, &oldCredential, &envelope);
	if (!NT_SUCCESS(status))
		goto done;
	if (RtlCompareMemory(&context->RecoveryCredentialId,
			&oldCredential.CredentialId, sizeof(GUID)) != sizeof(GUID) ||
		context->RecoveryAuthEpoch != oldCredential.AuthEpoch)
	{
		/* 代次已经变了，这份武装不再对应磁盘上的凭据。 */
		CdpRecoveryClearFileContext(Context);
		status = STATUS_REVISION_MISMATCH;
		goto done;
	}

	status = CdpCredentialCreate(Password, PasswordLength, &newCredential);
	if (!NT_SUCCESS(status))
		goto done;
	newCredential.CredentialId = oldCredential.CredentialId;
	newCredential.AuthEpoch = oldCredential.AuthEpoch + 1;
	if (newCredential.AuthEpoch == 0)
		newCredential.AuthEpoch = 1;

	RtlSecureZeroMemory(&envelope, sizeof(envelope));
	status = CdpPasswordEnvelopeCreate(
		Password, PasswordLength, &newCredential, &envelope);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpCommitSharedCredentialBundle(
		(PCdp_DRIVER_EXTENSION)DriverExt,
		&oldCredential,
		&newCredential,
		&envelope);
	if (!NT_SUCCESS(status))
		goto done;

	/* 这个句柄如果本来就已认证，让会话跟上新的代次。 */
	if (context->Authenticated &&
		RtlCompareMemory(&context->CredentialId, &oldCredential.CredentialId,
			sizeof(GUID)) == sizeof(GUID) &&
		context->AuthEpoch == oldCredential.AuthEpoch)
	{
		context->AuthEpoch = newCredential.AuthEpoch;
		context->ExpiresAt100ns =
			KeQueryInterruptTime() + 60ULL * 60ULL * 10000000ULL;
	}
	CdpRecoveryClearFileContext(Context);
	status = STATUS_SUCCESS;

done:
	if (!NT_SUCCESS(status))
		Cdp_LOG("[RECOVERY] change failed status=0x%08X\n", status);
	RtlSecureZeroMemory(&oldCredential, sizeof(oldCredential));
	RtlSecureZeroMemory(&newCredential, sizeof(newCredential));
	RtlSecureZeroMemory(&envelope, sizeof(envelope));
	return status;
}

#else

/* 非授权构建不生成找回材料。 */
NTSTATUS CdpBuildRecoveryQr(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const Cdp_BUILD_RECOVERY_QR_REQUEST* Request,
	Cdp_RECOVERY_QR_REPLY* Reply)
{
	UNREFERENCED_PARAMETER(DriverExt);
	UNREFERENCED_PARAMETER(Context);
	UNREFERENCED_PARAMETER(Request);
	UNREFERENCED_PARAMETER(Reply);
	return STATUS_NOT_SUPPORTED;
}

/* 非授权构建不接受重设凭据。 */
NTSTATUS CdpImportRecoveryCredential(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const UCHAR* Credential,
	ULONG CredentialLength)
{
	UNREFERENCED_PARAMETER(DriverExt);
	UNREFERENCED_PARAMETER(Context);
	UNREFERENCED_PARAMETER(Credential);
	UNREFERENCED_PARAMETER(CredentialLength);
	return STATUS_NOT_SUPPORTED;
}

/* 非授权构建不能凭找回凭据改密。 */
NTSTATUS CdpChangePasswordWithRecovery(
	struct _Cdp_DRIVER_EXTENSION* DriverExt,
	struct _Cdp_CONTROL_FILE_CONTEXT* Context,
	const UCHAR* Password,
	ULONG PasswordLength)
{
	UNREFERENCED_PARAMETER(DriverExt);
	UNREFERENCED_PARAMETER(Context);
	UNREFERENCED_PARAMETER(Password);
	UNREFERENCED_PARAMETER(PasswordLength);
	return STATUS_NOT_SUPPORTED;
}

#endif
