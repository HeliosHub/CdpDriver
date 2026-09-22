/*
 * 授权闸门实现（LICENSE_DESIGN.md §5 / §6 / §11）
 *
 * 全局状态 g_CdpLicenseState 由本文件独占更新；落盘通过
 * CdpJournalSetLicenseState 写到「所有已挂载」日志分区超级块。
 *
 * 闸门顺序（回滚/重做）：
 *   完整性(O-16) → BeforeOp: 解密 E0 → 对照 g_* → 判期/次数 → OPS_T++ 写 E0
 *                → 签发 Capability Token（O-18）
 *   ArmOp: 再次完整性 + 验 token，然后才允许 RecoveryBegin
 *   AfterSuccess: 再验 token → OPS_S++ [, C0--] → 写 E0 → 作废 token
 *   对照失败 / token 失败 / 完整性失败: A_MOD++ → 写 E0 → 拒绝
 */

#ifdef CDP_LICENSE

#include "CdpLicenseGate.h"
#include "CdpLocalSeal.h"
#include "CdpLicenseTrust.h"
#include "CdpLicenseHw.h"
#include "CdpIrpDispatchs.h"
#include "CdpLicenseProtect.h"
#include "CdpLicenseCodec.h"
#include <ntstrsafe.h>
#include "CdpLicenseSeg.h" /* 此后本文件代码/常量进入 .licprot / .licpr */

Cdp_LICENSE_STATE g_CdpLicenseState;
KMUTEX g_CdpLicenseMutex;

static PCdp_DRIVER_EXTENSION g_CdpLicenseDriverExt = NULL;
static KDPC g_CdpLicenseTimerDpc;
static KTIMER g_CdpLicenseTimer;
static BOOLEAN g_CdpLicenseTimerArmed = FALSE;
static WORK_QUEUE_ITEM g_CdpLicenseTimerWorkItem;
static volatile LONG g_CdpLicenseTimerWorkQueued = 0;
/* Hardware identity can be unavailable while boot-time Journal discovery runs. */
static volatile LONG g_CdpLicenseBootstrapAttempt = 0;
/* Set only after auto-discovery finds a Journal carrying persisted license state. */
static volatile LONG g_CdpLicenseRestorePending = 0;
static ULONG g_CdpLicMirrorA = 0;       /* Gate 侧镜像：crc ^ 0xA55A5AA5 */
static ULONG g_CdpLicMirrorB = 0;       /* 与 A 异或还原出 crc；与 Protect 分片分开存放 */
static volatile LONG g_CdpLicMirrorReady = 0;

static UINT64 CdpLicenseQuerySystemTime100ns(VOID);
static NTSTATUS CdpLicensePersistToAllJournals(
	_In_ PCdp_DRIVER_EXTENSION DriverExt);

/*
 * 作废当前回滚周期的 Capability Token。调用方必须已持有 g_CdpLicenseMutex。
 * SecureZero 覆盖 token 字节，避免残留在全局结构里被转储。
 */
static VOID CdpLicenseClearCapLocked(VOID)
{
	CdpLocalSealSecureZero(g_CdpLicenseState.PendingCapToken, Cdp_CAP_TOKEN_BYTES);
	g_CdpLicenseState.PendingCapNonce = 0;
	g_CdpLicenseState.PendingCapIssued100ns = 0;
	g_CdpLicenseState.PendingCapValid = FALSE;
	g_CdpLicenseState.PendingCapArmed = FALSE;
}

/*
 * 在 Protect 快照成功后，再存一份 CRC 到本文件的 .data。
 * 攻击者若只 patch CdpLicenseProtectVerify 为「恒成功」，镜像对照仍会失败。
 * 0xA55A5AA5 是固定混合常数：期望 crc = MirrorA ^ MirrorB。
 */
static VOID CdpLicenseIntegrityStoreMirror(VOID)
{
	ULONG crc = CdpLicenseProtectComputeCrc();

	g_CdpLicMirrorA = crc ^ 0xA55A5AA5UL;
	g_CdpLicMirrorB = 0xA55A5AA5UL;
	InterlockedExchange(&g_CdpLicMirrorReady, (crc != 0) ? 1 : 0);
}

/* 用 ComputeCrc() 重算现场值，与镜像期望比较。 */
static NTSTATUS CdpLicenseIntegrityMirror(VOID)
{
	ULONG crc;

	if (InterlockedCompareExchange(&g_CdpLicMirrorReady, 0, 0) == 0)
	{
		Cdp_LIC_FAIL("integrity mirror not ready");
		return STATUS_CDP_LICENSE_TAMPER;
	}
	crc = CdpLicenseProtectComputeCrc();
	if (crc == 0 || crc != (g_CdpLicMirrorA ^ g_CdpLicMirrorB))
	{
		Cdp_LIC_FAIL("integrity mirror mismatch");
		return STATUS_CDP_LICENSE_TAMPER;
	}
	return STATUS_SUCCESS;
}

/*
 * 完整性失败的统一善后：有证则 A_MOD++ 并尝试写回 E0，同时作废 pending token。
 * 返回值固定为 TAMPER（即使 Persist 成功也不改成 SUCCESS，避免调用方当成放行）。
 */
static NTSTATUS CdpLicenseNoteIntegrityFailure(
	_In_opt_ PCdp_DRIVER_EXTENSION DriverExt)
{
	BOOLEAN persist = FALSE;

	CdpLicenseLock();
	if (g_CdpLicenseState.HasLicense && g_CdpLicenseState.SealKeyValid)
	{
		g_CdpLicenseState.g_A_MOD += 1;
		persist = TRUE;
	}
	CdpLicenseClearCapLocked();
	CdpLicenseUnlock();
	if (persist && DriverExt)
		(void)CdpLicensePersistToAllJournals(DriverExt);
	return STATUS_CDP_LICENSE_TAMPER;
}

/*
 * 双检：Protect 分片还原 + Gate 镜像。任一失败都走 NoteIntegrityFailure。
 * 必须在 PASSIVE_LEVEL 调用（CRC 表初始化与后续 Persist 都假定如此）。
 */
static NTSTATUS CdpLicenseRequireIntegrity(
	_In_opt_ PCdp_DRIVER_EXTENSION DriverExt)
{
	if (NT_SUCCESS(CdpLicenseProtectVerify()) &&
		NT_SUCCESS(CdpLicenseIntegrityMirror()))
	{
		return STATUS_SUCCESS;
	}
	Cdp_LIC_FAIL("license code integrity failed");
	return CdpLicenseNoteIntegrityFailure(DriverExt);
}

/*
 * 在闸门已通过、OPS_T 已 +1 的前提下签发 token。调用方须已持锁。
 * token = SHA256(SealKey || "cdp-cap-v1" || 当前水位 || nonce || issued)
 * nonce 优先用 CNG 随机数；失败则退化为当前时间低 32 位，避免导入路径卡死。
 * Armed 在此保持 FALSE，必须再经 GateArmOp 才允许 RecoveryBegin。
 */
static NTSTATUS CdpLicenseMintCapLocked(
	_Inout_ PCdp_LICENSE_LOCAL_STATE Local)
{
	UCHAR rnd[4];
	UCHAR token[Cdp_CAP_TOKEN_BYTES];
	NTSTATUS status;
	ULONG nonce;
	UINT64 issued;

	status = CdpLicenseRandomBytes(rnd, sizeof(rnd));
	if (NT_SUCCESS(status))
		RtlCopyMemory(&nonce, rnd, sizeof(nonce));
	else
		nonce = (ULONG)CdpLicenseQuerySystemTime100ns();
	issued = CdpLicenseQuerySystemTime100ns();
	status = CdpLocalSealMakeCapToken(
		g_CdpLicenseState.SealKey,
		Local->l_T0,
		Local->l_C0,
		Local->l_OPS_T,
		Local->l_OPS_S,
		Local->l_A_MOD,
		nonce,
		issued,
		token);
	CdpLocalSealSecureZero(rnd, sizeof(rnd));
	if (!NT_SUCCESS(status))
	{
		CdpLocalSealSecureZero(token, sizeof(token));
		CdpLicenseClearCapLocked();
		Cdp_LIC_FAIL("mint cap token failed 0x%08X", status);
		return status;
	}
	RtlCopyMemory(g_CdpLicenseState.PendingCapToken, token, Cdp_CAP_TOKEN_BYTES);
	RtlCopyMemory(Local->CapToken, token, Cdp_CAP_TOKEN_BYTES);
	CdpLocalSealSecureZero(token, sizeof(token));
	g_CdpLicenseState.PendingCapNonce = nonce;
	g_CdpLicenseState.PendingCapIssued100ns = issued;
	g_CdpLicenseState.PendingCapValid = TRUE;
	g_CdpLicenseState.PendingCapArmed = FALSE;
	Local->CapNonce = nonce;
	Local->CapIssued100ns = issued;
	Local->CapTokenValid = 1;
	return STATUS_SUCCESS;
}

/*
 * 用「当前水位 + 签发时保存的 nonce/issued」重算 token。
 * Arm / AfterSuccess 都走这里：水位被改过则 SHA256 对不上 PendingCapToken。
 */
static NTSTATUS CdpLicenseRecomputeCap(
	_In_ UINT64 T0,
	_In_ ULONG C0,
	_In_ ULONG OpsT,
	_In_ ULONG OpsS,
	_In_ ULONG AMod,
	_Out_writes_bytes_(Cdp_CAP_TOKEN_BYTES) UCHAR* TokenOut)
{
	if (!g_CdpLicenseState.SealKeyValid || !g_CdpLicenseState.PendingCapValid)
		return STATUS_CDP_LICENSE_TAMPER;
	return CdpLocalSealMakeCapToken(
		g_CdpLicenseState.SealKey,
		T0,
		C0,
		OpsT,
		OpsS,
		AMod,
		g_CdpLicenseState.PendingCapNonce,
		g_CdpLicenseState.PendingCapIssued100ns,
		TokenOut);
}

static UINT64 CdpLicenseQuerySystemTime100ns(VOID)
{
	LARGE_INTEGER now;
	KeQuerySystemTimePrecise(&now);
	return (UINT64)now.QuadPart;
}

static SIZE_T CdpLicenseStrLen(
	_In_reads_or_z_(Max) const CHAR* S,
	_In_ SIZE_T Max)
{
	return CdpLicenseCodecStrLen(S, Max);
}

/*
 * 从 IOCTL 请求拷贝扫码 URI 前缀：必须非空、缓冲内当成 C 字符串，
 * 且仅允许 ASCII 可打印字符。调用方应自带 #c= / ?c= 等拼接位点，
 * 驱动只做 prefix + Base64，不补 fragment。
 */
static NTSTATUS CdpLicenseSanitizeApplyQrPrefix(
	_In_reads_(Cdp_APPLY_QR_PREFIX_MAX) const CHAR* In,
	_Out_writes_z_(Cdp_APPLY_QR_PREFIX_MAX) CHAR* Out)
{
	return CdpLicenseCodecSanitizePrintablePrefix(
		In, Out, Cdp_APPLY_QR_PREFIX_MAX);
}

static BOOLEAN CdpLicenseCStrEq(
	_In_z_ const CHAR* A,
	_In_z_ const CHAR* B)
{
	return CdpLicenseCodecCStrEq(A, B);
}

static NTSTATUS CdpLicenseHexDecode(
	_In_reads_(HexLen) const CHAR* Hex,
	_In_ ULONG HexLen,
	_Out_writes_bytes_to_(OutCap, *OutLen) UCHAR* Out,
	_In_ ULONG OutCap,
	_Out_ PULONG OutLen)
{
	return CdpLicenseCodecHexDecode(Hex, HexLen, Out, OutCap, OutLen);
}

static NTSTATUS CdpLicenseBase64Decode(
	_In_reads_(InLen) const CHAR* In,
	_In_ ULONG InLen,
	_Out_writes_bytes_to_(OutCap, *OutLen) UCHAR* Out,
	_In_ ULONG OutCap,
	_Out_ PULONG OutLen)
{
	return CdpLicenseCodecBase64Decode(In, InLen, Out, OutCap, OutLen);
}

static NTSTATUS CdpLicenseBase64Encode(
	_In_reads_bytes_(InLen) const UCHAR* In,
	_In_ ULONG InLen,
	_Out_writes_bytes_to_(OutCap, *OutLen) CHAR* Out,
	_In_ ULONG OutCap,
	_Out_ PULONG OutLen)
{
	return CdpLicenseCodecBase64Encode(In, InLen, Out, OutCap, OutLen);
}

static BOOLEAN CdpLicenseFindJsonString(
	_In_reads_(JsonLen) const CHAR* Json,
	_In_ ULONG JsonLen,
	_In_z_ const CHAR* Key,
	_Out_writes_(OutCap) CHAR* Out,
	_In_ ULONG OutCap)
{
	return CdpLicenseCodecFindJsonString(Json, JsonLen, Key, Out, OutCap);
}

static BOOLEAN CdpLicenseFindJsonNumber(
	_In_reads_(JsonLen) const CHAR* Json,
	_In_ ULONG JsonLen,
	_In_z_ const CHAR* Key,
	_Out_ PLONGLONG Value)
{
	return CdpLicenseCodecFindJsonNumber(Json, JsonLen, Key, Value);
}

static NTSTATUS CdpLicenseParseIso8601ToFileTime(
	_In_z_ const CHAR* Iso,
	_Out_ PUINT64 Out100ns)
{
	return CdpLicenseCodecParseIso8601ToFileTime(Iso, Out100ns);
}

/*
 * 从 License JSON 重建「无 signature」的长度前缀 canonical，供 PSS 验签。
 * 键序必须与服务端 dumps_canonical(sort_keys=True) 一致。
 */
static NTSTATUS CdpLicenseBuildCanonicalWithoutSignature(
	_In_reads_(JsonLen) const CHAR* Json,
	_In_ ULONG JsonLen,
	_Out_writes_bytes_to_(OutCap, *OutLen) UCHAR* Out,
	_In_ ULONG OutCap,
	_Out_ PULONG OutLen)
{
	return CdpLicenseCodecBuildCanonicalWithoutSignature(
		Json, JsonLen, Out, OutCap, OutLen);
}

/* 用当前 g_* 密封为 E0 字节（不落盘） */
static NTSTATUS CdpLicenseSealCurrentState(
	_Out_writes_bytes_to_(OutCap, *OutLen) UCHAR* Out,
	_In_ ULONG OutCap,
	_Out_ PULONG OutLen)
{
	Cdp_E0_PLAINTEXT plain;
	NTSTATUS status;

	if (!g_CdpLicenseState.SealKeyValid)
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_REQUIRED: if (!g_CdpLicenseState.SealKeyValid)");
		return STATUS_CDP_LICENSE_REQUIRED;
	}
	RtlZeroMemory(&plain, sizeof(plain));
	plain.T0_100ns = g_CdpLicenseState.g_T0;
	plain.C0 = g_CdpLicenseState.g_C0;
	plain.OPS_T = g_CdpLicenseState.g_OPS_T;
	plain.OPS_S = g_CdpLicenseState.g_OPS_S;
	plain.A_MOD = g_CdpLicenseState.g_A_MOD;
	status = CdpLocalSealEncrypt(
		g_CdpLicenseState.SealKey, &plain, Out, OutCap, OutLen);
	CdpLocalSealSecureZero(&plain, sizeof(plain));
	return status;
}

/*
 * 将 LicenseBlob + 最新 E0 写入全部已挂载 Journal 超级块（设计：导入/状态变更后）。
 * 任一分区失败即返回错误，避免状态分叉。
 */
static NTSTATUS CdpLicensePersistToAllJournals(
	_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	PCdp_VOLUME_HANDLE_ENTRY* journals = NULL;
	ULONG journalCount = 0;
	ULONG i;
	NTSTATUS status = STATUS_SUCCESS;
	ULONG e0Len = 0;
	UCHAR e0[Cdp_E0_SEAL_MAX];

	/*
	 * 同步 superblock I/O 必须在 PASSIVE_LEVEL，且不得持有 FastMutex。
	 * VolumeHandleMutex 会升到 APC_LEVEL，挡住 I/O 完成 APC，表现为
	 * JOURNAL-RAW wait begin 之后 GUI/IOCTL 永久卡住。
	 */
	if (KeGetCurrentIrql() > PASSIVE_LEVEL)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_DEVICE_STATE: persist at IRQL=%u",
			(ULONG)KeGetCurrentIrql());
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (!DriverExt || !g_CdpLicenseState.HasLicense)
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_REQUIRED: if (!DriverExt || !g_CdpLicenseState.HasLicense)");
		return STATUS_CDP_LICENSE_REQUIRED;
	}
	status = CdpLicenseSealCurrentState(e0, sizeof(e0), &e0Len);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}

	CdpLicenseLock();
	if (!g_CdpLicenseState.HasLicense)
	{
		CdpLicenseUnlock();
		CdpLocalSealSecureZero(e0, sizeof(e0));
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_REQUIRED: license dropped before persist");
		return STATUS_CDP_LICENSE_REQUIRED;
	}
	g_CdpLicenseState.E0Length = e0Len;
	RtlCopyMemory(g_CdpLicenseState.E0, e0, e0Len);
	CdpLicenseUnlock();

	status = CdpPinMountedJournals(DriverExt, &journals, &journalCount);
	if (!NT_SUCCESS(status))
	{
		CdpLocalSealSecureZero(e0, sizeof(e0));
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	Cdp_LOG("[LICENSE] persist mountedJournals=%lu blobBytes=%lu e0Bytes=%lu\n",
		journalCount, g_CdpLicenseState.LicenseBlobLength, e0Len);
	for (i = 0; i < journalCount; ++i)
	{
		status = CdpJournalSetLicenseState(
			&journals[i]->Journal,
			g_CdpLicenseState.LicenseBlob,
			g_CdpLicenseState.LicenseBlobLength,
			e0,
			e0Len);
		if (!NT_SUCCESS(status))
			break;
	}
	for (i = 0; i < journalCount; ++i)
		CdpReleaseVolumeHandleEntry(journals[i]);
	if (journals)
		cdpfree(journals);
	CdpLocalSealSecureZero(e0, sizeof(e0));
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[LICENSE] persist failed status=0x%08X journalIndex=%lu\n", status, i);
		Cdp_LIC_FAIL("failed status=0x%08X", status);
	}
	return status;
}

NTSTATUS CdpLicensePersistIfLoaded(
	_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	if (!DriverExt)
		return STATUS_INVALID_PARAMETER;
	if (!g_CdpLicenseState.HasLicense)
		return STATUS_SUCCESS;
	return CdpLicensePersistToAllJournals(DriverExt);
}

static NTSTATUS CdpLicenseLoadStateFromPlain(
	_In_ const Cdp_E0_PLAINTEXT* Plain)
{
	g_CdpLicenseState.g_T0 = Plain->T0_100ns;
	g_CdpLicenseState.g_C0 = Plain->C0;
	g_CdpLicenseState.g_OPS_T = Plain->OPS_T;
	g_CdpLicenseState.g_OPS_S = Plain->OPS_S;
	g_CdpLicenseState.g_A_MOD = Plain->A_MOD;
	return STATUS_SUCCESS;
}

/*
 * 完整验签路径（O-03/O-04）：
 *   解析 signature → 重建 canonical → PSS 验签 → 解析字段
 *   → 本机指纹与 device_id_hash 比对 → 派生 SealKey
 * 成功后填充 g_CdpLicenseState 中与证书相关的字段（尚未写 g_T0/C0 等水位）。
 * Blob的组成：u32BE(jsonLen) + json 
 */
static NTSTATUS CdpLicenseVerifyBlobAndBind(
	_In_reads_bytes_(BlobLength) const UCHAR* Blob,
	_In_ ULONG BlobLength)
{
	ULONG jsonLen;
	const CHAR* json;
	CHAR signatureB64[512];
	UCHAR signature[256];
	ULONG sigLen = 0;
	UCHAR canonical[1800];
	ULONG canonicalLen = 0;
	CHAR deviceHash[80];
	CHAR mbUuid[Cdp_LICENSE_MB_UUID_CHARS];
	CHAR diskSerial[Cdp_LICENSE_DISK_SERIAL_CHARS];
	CHAR licenseId[Cdp_LICENSE_ID_CHARS];
	CHAR t0Issue[64];
	CHAR tExp[64];
	CHAR kind[16];
	BOOLEAN isTrial = FALSE;
	LONGLONG mode = 0;
	LONGLONG c0 = 0;
	LONGLONG formatVersion = 0;
	UCHAR fpFromLic[Cdp_LICENSE_FP_BYTES];
	ULONG fpLen = 0;
	CHAR localMb[Cdp_LICENSE_MB_UUID_CHARS];
	CHAR localDisk[Cdp_LICENSE_DISK_SERIAL_CHARS];
	UCHAR localFp[Cdp_LICENSE_FP_BYTES];
	NTSTATUS status;

	if (BlobLength < 8)
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: if (BlobLength < 8)");
		return STATUS_CDP_LICENSE_INVALID;
	}
	jsonLen = ((ULONG)Blob[0] << 24) | ((ULONG)Blob[1] << 16) |
		((ULONG)Blob[2] << 8) | (ULONG)Blob[3];
	if (jsonLen + 4 > BlobLength)
	{
		Cdp_LIC_FAIL(
			"STATUS_CDP_LICENSE_INVALID: jsonLen=%lu blobLen=%lu (expect u32BE+JSON; GUI must Base64-decode first)",
			jsonLen, BlobLength);
		return STATUS_CDP_LICENSE_INVALID;
	}
	json = (const CHAR*)(Blob + 4);

	if (!CdpLicenseFindJsonString(json, jsonLen, "signature",
		signatureB64, sizeof(signatureB64)))
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: if (!CdpLicenseFindJsonString(json, jsonLen, \"signature\",");
		return STATUS_CDP_LICENSE_INVALID;
	}
	status = CdpLicenseBase64Decode(
		signatureB64,
		(ULONG)CdpLicenseStrLen(signatureB64, sizeof(signatureB64)),
		signature,
		sizeof(signature),
		&sigLen); // 将signatureB64解码为signature（这个signature是服务端对json使用Vendor的私钥进行RSA-PSS签名后的结果）
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: if (!NT_SUCCESS(status))");
		return STATUS_CDP_LICENSE_INVALID;
	}
	status = CdpLicenseBuildCanonicalWithoutSignature(
		json, jsonLen, canonical, sizeof(canonical), &canonicalLen); // 构建canonical，canonical是json的副本（由长度和json字符串组成），其中不包含signature键，且各字段按照给定顺序排列
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	status = CdpLicenseVerifySignedCanonical(
		canonical, canonicalLen, signature, sigLen);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}

	// 解析json中的字段, 验证json中的字段是否符合要求
	if (!CdpLicenseFindJsonNumber(json, jsonLen, "format_version", &formatVersion) ||
		formatVersion < (LONGLONG)Cdp_LICENSE_FORMAT_VERSION_MIN ||
		formatVersion > (LONGLONG)Cdp_LICENSE_FORMAT_VERSION_MAX ||
		!CdpLicenseFindJsonNumber(json, jsonLen, "mode", &mode) ||
		!CdpLicenseFindJsonNumber(json, jsonLen, "c0_initial", &c0) ||
		!CdpLicenseFindJsonString(json, jsonLen, "device_id_hash", deviceHash, sizeof(deviceHash)) ||
		!CdpLicenseFindJsonString(json, jsonLen, "mb_uuid", mbUuid, sizeof(mbUuid)) ||
		!CdpLicenseFindJsonString(json, jsonLen, "disk_serial", diskSerial, sizeof(diskSerial)) ||
		!CdpLicenseFindJsonString(json, jsonLen, "license_id", licenseId, sizeof(licenseId)) ||
		!CdpLicenseFindJsonString(json, jsonLen, "t0_issue", t0Issue, sizeof(t0Issue)) ||
		!CdpLicenseFindJsonString(json, jsonLen, "t_exp", tExp, sizeof(tExp)))
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: if (!CdpLicenseFindJsonNumber(json, jsonLen, \"format_version\", &formatVersion) ||");
		Cdp_LIC_INFO("json: %s", json);
		return STATUS_CDP_LICENSE_INVALID;
	}
	RtlZeroMemory(kind, sizeof(kind));
	if (formatVersion >= 2)
	{
		if (!CdpLicenseFindJsonString(json, jsonLen, "kind", kind, sizeof(kind)))
		{
			Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: missing kind");
			return STATUS_CDP_LICENSE_INVALID;
		}
		if (CdpLicenseCStrEq(kind, "trial"))
			isTrial = TRUE;
		else if (!CdpLicenseCStrEq(kind, "paid"))
		{
			Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: kind not paid/trial");
			return STATUS_CDP_LICENSE_INVALID;
		}
	}

	// 将deviceHash转换为fpFromLic（十六进制字符串转换为二进制数据），然后与本机指纹localFp进行比对
	status = CdpLicenseHexDecode(
		deviceHash,
		(ULONG)CdpLicenseStrLen(deviceHash, sizeof(deviceHash)),
		fpFromLic,
		sizeof(fpFromLic),
		&fpLen);
	if (!NT_SUCCESS(status) || fpLen != Cdp_LICENSE_FP_BYTES)
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: if (!NT_SUCCESS(status) || fpLen != Cdp_LICENSE_FP_BYTES)");
		return STATUS_CDP_LICENSE_INVALID;
	}
	status = CdpLicenseCollectHardwareId(localMb, localDisk, localFp);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	if (RtlCompareMemory(localFp, fpFromLic, Cdp_LICENSE_FP_BYTES) !=
		Cdp_LICENSE_FP_BYTES)
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: if (RtlCompareMemory(localFp, fpFromLic, Cdp_LICENSE_FP_BYTES) !=");
		return STATUS_CDP_LICENSE_INVALID;
	}

	// 将t0Issue和tExp转换为100ns时间戳
	status = CdpLicenseParseIso8601ToFileTime(
		t0Issue, &g_CdpLicenseState.T0_Issue_100ns);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	status = CdpLicenseParseIso8601ToFileTime(
		tExp, &g_CdpLicenseState.T_EXP_100ns);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	g_CdpLicenseState.Mode = (ULONG)mode;
	g_CdpLicenseState.C0_Initial = (ULONG)c0;
	g_CdpLicenseState.IsTrial = isTrial;
	RtlStringCbCopyA(g_CdpLicenseState.LicenseId,
		sizeof(g_CdpLicenseState.LicenseId), licenseId);
	RtlStringCbCopyA(g_CdpLicenseState.MbUuid,
		sizeof(g_CdpLicenseState.MbUuid), localMb);
	RtlStringCbCopyA(g_CdpLicenseState.DiskSerial,
		sizeof(g_CdpLicenseState.DiskSerial), localDisk);
	RtlCopyMemory(g_CdpLicenseState.DeviceFingerprint, localFp,
		Cdp_LICENSE_FP_BYTES);

	// 派生 SealKey（根据本机指纹和证书结束时刻，使用sha256算法派生出SealKey）
	status = CdpLocalSealDeriveKey(
		localFp,
		g_CdpLicenseState.T_EXP_100ns,
		g_CdpLicenseState.SealKey);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	g_CdpLicenseState.SealKeyValid = TRUE;
	return STATUS_SUCCESS;
}

/*
 * 有效性：T0 < now < T_EXP；次数/混合模式还要求 C0 > 0。
 * 时长-only（mode=1）允许 C0==0。
 */
static NTSTATUS CdpLicenseCheckValidityWithLocal(
	_In_ const Cdp_LICENSE_LOCAL_STATE* Local)
{
	UINT64 now = CdpLicenseQuerySystemTime100ns();

	return CdpLicenseCodecCheckValidity(g_CdpLicenseState.HasLicense,
		g_CdpLicenseState.Mode, Local->l_T0, Local->l_C0, now,
		g_CdpLicenseState.T_EXP_100ns);
}

/* 开机/每小时：T0 := max(T0, now)。调用方须已持有 g_CdpLicenseMutex。 */
static VOID CdpLicenseAdvanceT0Locked(VOID)
{
	UINT64 now;

	if (!g_CdpLicenseState.HasLicense || !g_CdpLicenseState.SealKeyValid)
		return;
	now = CdpLicenseQuerySystemTime100ns();
	if (now > g_CdpLicenseState.g_T0)
		g_CdpLicenseState.g_T0 = now;
}

static BOOLEAN CdpLicenseIsLoaded(VOID)
{
	BOOLEAN loaded;

	CdpLicenseLock();
	loaded = g_CdpLicenseState.HasLicense;
	CdpLicenseUnlock();
	return loaded;
}

/*
 * Run only from a PASSIVE_LEVEL work item.  It deliberately happens after
 * device start: the system-disk serial is often unavailable during the early
 * auto-discovery callback, which would otherwise produce a false bind error.
 */
static NTSTATUS CdpLicenseRestoreFromMountedJournals(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Out_ PBOOLEAN FoundConfiguredJournal)
{
	PCdp_VOLUME_HANDLE_ENTRY* journals = NULL;
	ULONG journalCount = 0;
	ULONG i;
	NTSTATUS status;

	if (FoundConfiguredJournal)
		*FoundConfiguredJournal = FALSE;
	status = CdpPinMountedJournals(DriverExt, &journals, &journalCount);
	if (!NT_SUCCESS(status))
		return status;
	for (i = 0; i < journalCount; ++i)
	{
		if (!journals[i]->Journal.LicenseConfigured)
			continue;
		if (FoundConfiguredJournal)
			*FoundConfiguredJournal = TRUE;
		status = CdpLicenseOnJournalMounted(DriverExt, &journals[i]->Journal);
		Cdp_LOG("[LICENSE] deferred restore journal=%lu status=0x%08X\n", i, status);
		if (NT_SUCCESS(status) && CdpLicenseIsLoaded())
			break;
	}
	for (i = 0; i < journalCount; ++i)
		CdpReleaseVolumeHandleEntry(journals[i]);
	if (journals)
		cdpfree(journals);
	return CdpLicenseIsLoaded() ? STATUS_SUCCESS : STATUS_CDP_LICENSE_REQUIRED;
}

static VOID CdpLicenseTimerWorker(_In_ PVOID Context)
{
	PCdp_DRIVER_EXTENSION driverExt = (PCdp_DRIVER_EXTENSION)Context;
	LARGE_INTEGER due;
	BOOLEAN configuredJournal = FALSE;
	BOOLEAN bootstrapRetry = FALSE;
	LONG bootstrapAttempt = 0;

	InterlockedExchange(&g_CdpLicenseTimerWorkQueued, 0);
	if (!driverExt)
		return;

	if (InterlockedCompareExchange(&g_CdpLicenseRestorePending, 0, 0) != 0)
	{
		if (CdpLicenseIsLoaded())
		{
			InterlockedExchange(&g_CdpLicenseRestorePending, 0);
		}
		else
		{
			NTSTATUS restoreStatus = CdpLicenseRestoreFromMountedJournals(
				driverExt, &configuredJournal);
			bootstrapAttempt = InterlockedIncrement(&g_CdpLicenseBootstrapAttempt);
			if (CdpLicenseIsLoaded())
				InterlockedExchange(&g_CdpLicenseRestorePending, 0);
			else if (bootstrapAttempt < 5)
				bootstrapRetry = TRUE;
			else
				InterlockedExchange(&g_CdpLicenseRestorePending, 0);
			Cdp_LOG("[LICENSE] deferred restore attempt=%ld configuredJournal=%u status=0x%08X retry=%u\n",
				bootstrapAttempt, configuredJournal ? 1u : 0u, restoreStatus,
				bootstrapRetry ? 1u : 0u);
		}
	}

	/*
	 * 小时定时器在 PASSIVE 工作项里跑，适合做完整性对照。
	 * 失败只记 A_MOD / 作废 token，仍推进 T0：时钟水位不能因为被 patch 而停住。
	 */
	if (!NT_SUCCESS(CdpLicenseProtectVerify()) ||
		!NT_SUCCESS(CdpLicenseIntegrityMirror()))
	{
		Cdp_LOG("[LICENSE] integrity failed on hourly timer\n");
		(void)CdpLicenseNoteIntegrityFailure(driverExt);
	}

	CdpLicenseLock();
	CdpLicenseAdvanceT0Locked();
	CdpLicenseUnlock();
	if (g_CdpLicenseState.HasLicense && g_CdpLicenseState.SealKeyValid)
	{
		NTSTATUS status = CdpLicensePersistToAllJournals(driverExt);
		if (!NT_SUCCESS(status))
			Cdp_LOG("[LICENSE] T0 advance persist failed 0x%08X\n", status);
	}

	if (g_CdpLicenseTimerArmed)
	{
		ULONG delaySeconds = 60 * 60; /* normal hourly maintenance */
		if (bootstrapRetry)
		{
			delaySeconds = 10u << (bootstrapAttempt - 1); /* 10, 20, 40, 80 */
			if (delaySeconds > 60u) delaySeconds = 60u;
		}
		due.QuadPart = -((LONGLONG)delaySeconds * 10000000LL);
		KeSetTimer(&g_CdpLicenseTimer, due, &g_CdpLicenseTimerDpc);
	}
}

static VOID CdpLicenseTimerDpc(
	_In_ PKDPC Dpc,
	_In_opt_ PVOID DeferredContext,
	_In_opt_ PVOID SystemArgument1,
	_In_opt_ PVOID SystemArgument2)
{
	PCdp_DRIVER_EXTENSION driverExt = (PCdp_DRIVER_EXTENSION)DeferredContext;

	UNREFERENCED_PARAMETER(Dpc);
	UNREFERENCED_PARAMETER(SystemArgument1);
	UNREFERENCED_PARAMETER(SystemArgument2);
	if (!driverExt)
		return;
	/* DPC 在 DISPATCH_LEVEL：不得持 FastMutex，也不得同步写盘。 */
	if (InterlockedCompareExchange(&g_CdpLicenseTimerWorkQueued, 1, 0) == 0)
		ExQueueWorkItem(&g_CdpLicenseTimerWorkItem, DelayedWorkQueue);
}

VOID CdpLicenseScheduleDeferredRestore(_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	LARGE_INTEGER due;

	if (!DriverExt || !g_CdpLicenseTimerArmed || CdpLicenseIsLoaded())
		return;
	if (InterlockedCompareExchange(&g_CdpLicenseRestorePending, 1, 0) != 0)
		return;

	InterlockedExchange(&g_CdpLicenseBootstrapAttempt, 0);
	/* Do not bind until firmware and the system-disk stack are fully available. */
	due.QuadPart = -((LONGLONG)10 * 10000000LL);
	KeSetTimer(&g_CdpLicenseTimer, due, &g_CdpLicenseTimerDpc);
	Cdp_LOG("[LICENSE] deferred restore queued from auto-discovered Journal\n");
}

/*
 * 授权子系统启动：清 g_CdpLicenseState、初始化 KMUTEX 与小时定时器，
 * 并对已加载的驱动映像做 .licprot/.licpr CRC 快照（O-16）。
 */
VOID CdpLicenseInitialize(
	_In_ PDRIVER_OBJECT DriverObject,
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	LARGE_INTEGER due;

	RtlZeroMemory(&g_CdpLicenseState, sizeof(g_CdpLicenseState));
	KeInitializeMutex(&g_CdpLicenseMutex, 0);
	g_CdpLicenseDriverExt = DriverExt;
	ExInitializeWorkItem(&g_CdpLicenseTimerWorkItem, CdpLicenseTimerWorker, DriverExt);
	InterlockedExchange(&g_CdpLicenseTimerWorkQueued, 0);
	InterlockedExchange(&g_CdpLicenseBootstrapAttempt, 0);
	InterlockedExchange(&g_CdpLicenseRestorePending, 0);
	KeInitializeDpc(&g_CdpLicenseTimerDpc, CdpLicenseTimerDpc, DriverExt);
	KeInitializeTimer(&g_CdpLicenseTimer);
	/* Hourly maintenance; boot-time restore re-arms this only when explicitly queued. */
	due.QuadPart = -((LONGLONG)60 * 60 * 10000000LL);
	KeSetTimer(&g_CdpLicenseTimer, due, &g_CdpLicenseTimerDpc);
	g_CdpLicenseTimerArmed = TRUE;
	/*
	 * DriverStart 是映射后的映像基址（已重定位）。Capture 失败不让 DriverEntry 失败，
	 * 否则整机 COW 起不来；之后 GateRequireIntegrity 会 fail-closed 拒绝回滚。
	 */
	if (DriverObject && DriverObject->DriverStart)
	{
		if (NT_SUCCESS(CdpLicenseProtectCapture(DriverObject->DriverStart)))
			CdpLicenseIntegrityStoreMirror();
		else
			Cdp_LOG("[LICENSE] protect capture failed; recovery will fail closed\n");
	}
}

VOID CdpLicenseShutdown(_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	/* 取消小时定时器、抹 token/SealKey、作废完整性快照。 */
	UNREFERENCED_PARAMETER(DriverExt);
	if (g_CdpLicenseTimerArmed)
	{
		KeCancelTimer(&g_CdpLicenseTimer);
		g_CdpLicenseTimerArmed = FALSE;
	}
	CdpLicenseLock();
	CdpLicenseClearCapLocked(); /* 先抹 token，再清 SealKey / 全局状态 */
	if (g_CdpLicenseState.SealKeyValid)
		CdpLocalSealSecureZero(g_CdpLicenseState.SealKey, 32);
	RtlZeroMemory(&g_CdpLicenseState, sizeof(g_CdpLicenseState));
	CdpLicenseUnlock();
	CdpLicenseProtectInvalidate(); /* 丢掉映像基址与 CRC 分片 */
	InterlockedExchange(&g_CdpLicMirrorReady, 0);
	InterlockedExchange(&g_CdpLicenseRestorePending, 0);
	InterlockedExchange(&g_CdpLicenseBootstrapAttempt, 0);
	g_CdpLicenseDriverExt = NULL;
}

NTSTATUS CdpLicenseSetFromBlob(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_reads_bytes_(BlobLength) const UCHAR* Blob,
	_In_ ULONG BlobLength)
{
	/*
	 * 导入成功初始化（设计 §6.2）：
	 *   T0=t0_issue, C0=c0_initial, OPS_T=OPS_S=A_MOD=0，再 Seal 写全部 Journal。
	 * 试用证：本机已有任意证时拒绝再导入另一张试用（同 license_id 视为幂等）。
	 * 付费证可覆盖试用。
	 */
	NTSTATUS status;
	Cdp_LICENSE_STATE saved;
	BOOLEAN hadLicense;
	BOOLEAN incomingTrial;
	BOOLEAN sameId;

	if (!DriverExt || !Blob || BlobLength == 0 ||
		BlobLength > Cdp_LICENSE_BLOB_MAX)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!DriverExt || !Blob || BlobLength == 0 ||");
		return STATUS_INVALID_PARAMETER;
	}

	CdpLicenseLock();
	saved = g_CdpLicenseState;
	hadLicense = g_CdpLicenseState.HasLicense;
	CdpLicenseUnlock();

	/*
	 * 验签（RSA/CNG）必须在 PASSIVE_LEVEL。ExAcquireFastMutex 会升到 APC_LEVEL，
	 * 此时 BCryptOpenAlgorithmProvider(RSA) 会返回 STATUS_NOT_SUPPORTED (0xC00000BB)。
	 * 申请 QR 路径在持锁外做 RSA，故能成功；导入必须同样先解锁再验签。
	 */
	status = CdpLicenseVerifyBlobAndBind(Blob, BlobLength);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}

	incomingTrial = g_CdpLicenseState.IsTrial;
	sameId = hadLicense &&
		CdpLicenseCStrEq(saved.LicenseId, g_CdpLicenseState.LicenseId) &&
		saved.LicenseId[0] != 0;

	if (incomingTrial && hadLicense)
	{
		CdpLicenseLock();
		g_CdpLicenseState = saved;
		CdpLicenseUnlock();
		if (sameId)
			return STATUS_SUCCESS;
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_TRIAL_USED");
		return STATUS_CDP_LICENSE_TRIAL_USED;
	}

	CdpLicenseLock();
	RtlCopyMemory(g_CdpLicenseState.LicenseBlob, Blob, BlobLength);
	g_CdpLicenseState.LicenseBlobLength = BlobLength;
	g_CdpLicenseState.g_T0 = g_CdpLicenseState.T0_Issue_100ns;
	g_CdpLicenseState.g_C0 = g_CdpLicenseState.C0_Initial;
	g_CdpLicenseState.g_OPS_T = 0;
	g_CdpLicenseState.g_OPS_S = 0;
	g_CdpLicenseState.g_A_MOD = 0;
	g_CdpLicenseState.HasLicense = TRUE;
	g_CdpLicenseState.Loaded = TRUE;
	CdpLicenseClearCapLocked(); /* 新证周期：作废上一张证残留的 pending token */
	CdpLicenseUnlock();

	/* Seal/E0 内含 BCrypt SHA256，同样必须在 PASSIVE_LEVEL（勿持 FastMutex）。 */
	status = CdpLicensePersistToAllJournals(DriverExt);
	if (NT_SUCCESS(status))
	{
		Cdp_LOG("[LICENSE] imported mode=%lu c0=%lu trial=%u\n",
			g_CdpLicenseState.Mode,
			g_CdpLicenseState.g_C0,
			g_CdpLicenseState.IsTrial ? 1u : 0u);
	}
	return status;
}

/* 挂载 Journal 时调用，验证License合法性，解密E0，并推进T0，持久化到Journal*/
NTSTATUS CdpLicenseOnJournalMounted(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_JOURNAL Journal)
{
	/*
	 * 驱动重启后首次挂到带 License 的 Journal：验签 + 解密 E0 恢复五元组，
	 * 并推进 T0。若全局已有证则跳过（多 Journal 共用一份状态）。
	 */
	UCHAR license[Cdp_LICENSE_BLOB_MAX];
	UCHAR e0[Cdp_E0_SEAL_MAX];
	ULONG licenseLen = 0;
	ULONG e0Len = 0;
	Cdp_E0_PLAINTEXT plain;
	NTSTATUS status;
	BOOLEAN persist = FALSE;

	UNREFERENCED_PARAMETER(DriverExt);
	if (!Journal || !Journal->LicenseConfigured)
		return STATUS_SUCCESS;
	if (!CdpJournalGetLicenseState(Journal, license, sizeof(license),
		&licenseLen, e0, sizeof(e0), &e0Len))
	{
		return STATUS_SUCCESS;
	}

	CdpLicenseLock();
	if (g_CdpLicenseState.HasLicense) // 如果已经加载了License，则直接返回成功
	{
		CdpLicenseUnlock();
		return STATUS_SUCCESS;
	}
	CdpLicenseUnlock();

	/* RSA 验签须 PASSIVE_LEVEL，不可在 FastMutex（APC_LEVEL）内调用 CNG。 */
	status = CdpLicenseVerifyBlobAndBind(license, licenseLen);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}

	CdpLicenseLock();
	if (g_CdpLicenseState.HasLicense)
	{
		/* 并发路径已完成导入，丢弃本次验签结果。 */
		status = STATUS_SUCCESS;
		goto done;
	}
	status = CdpLocalSealDecrypt(
		g_CdpLicenseState.SealKey, e0, e0Len, &plain);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	CdpLicenseLoadStateFromPlain(&plain);
	RtlCopyMemory(g_CdpLicenseState.LicenseBlob, license, licenseLen);
	g_CdpLicenseState.LicenseBlobLength = licenseLen;
	RtlCopyMemory(g_CdpLicenseState.E0, e0, e0Len);
	g_CdpLicenseState.E0Length = e0Len;
	g_CdpLicenseState.HasLicense = TRUE;
	g_CdpLicenseState.Loaded = TRUE;
	CdpLicenseAdvanceT0Locked();
	persist = TRUE;
	status = STATUS_SUCCESS;
done:
	CdpLocalSealSecureZero(&plain, sizeof(plain));
	CdpLicenseUnlock();
	if (persist)
	{
		NTSTATUS persistStatus = CdpLicensePersistToAllJournals(DriverExt);
		if (!NT_SUCCESS(persistStatus))
		{
			Cdp_LOG("[LICENSE] mount persist failed 0x%08X\n", persistStatus);
			status = persistStatus;
		}
	}
	return status;
}

NTSTATUS CdpLicenseGateBeforeOp(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Out_ PCdp_LICENSE_LOCAL_STATE Local)
{
	/*
	 * 审阅要点：
	 *  0) .licprot 完整性（O-16）；失败记 A_MOD 并拒绝
	 *  1) 以盘上 E0 镜像（globals.E0）解密为 l_*，再与 g_* 逐项比较
	 *  2) 不一致：信任盘侧字段，A_MOD++，写回，返回 TAMPER（终止业务）
	 *  3) 一致：用 l_T0/l_C0 判有效性；通过后 OPS_T++ 并立即持久化
	 *  4) 签发 Capability Token（O-18）
	 */
	Cdp_E0_PLAINTEXT plain;
	NTSTATUS status;
	BOOLEAN persist = FALSE;
	BOOLEAN tamper = FALSE;

	RtlZeroMemory(Local, sizeof(*Local));
	/* 完整性失败会 A_MOD++ 并返回 TAMPER，不得继续解密 E0 / 签发 token */
	status = CdpLicenseRequireIntegrity(DriverExt);
	if (!NT_SUCCESS(status))
		return status;

	CdpLicenseLock();
	if (!g_CdpLicenseState.HasLicense || !g_CdpLicenseState.SealKeyValid ||
		g_CdpLicenseState.E0Length == 0)
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_REQUIRED then goto done;");
		status = STATUS_CDP_LICENSE_REQUIRED;
		goto done;
	}

	/* Decrypt E0 from last persisted copy (superblock mirror in globals). */
	status = CdpLocalSealDecrypt(
		g_CdpLicenseState.SealKey,
		g_CdpLicenseState.E0,
		g_CdpLicenseState.E0Length,
		&plain);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	Local->l_T0 = plain.T0_100ns;
	Local->l_C0 = plain.C0;
	Local->l_OPS_T = plain.OPS_T;
	Local->l_OPS_S = plain.OPS_S;
	Local->l_A_MOD = plain.A_MOD;

	/* 内存篡改路径：以 E0 明文覆盖 g_*，再累加 A_MOD */
	if (g_CdpLicenseState.g_T0 != Local->l_T0 ||
		g_CdpLicenseState.g_C0 != Local->l_C0 ||
		g_CdpLicenseState.g_OPS_T != Local->l_OPS_T ||
		g_CdpLicenseState.g_OPS_S != Local->l_OPS_S ||
		g_CdpLicenseState.g_A_MOD != Local->l_A_MOD)
	{
		Local->l_A_MOD += 1;
		g_CdpLicenseState.g_A_MOD = Local->l_A_MOD;
		g_CdpLicenseState.g_T0 = Local->l_T0;
		g_CdpLicenseState.g_C0 = Local->l_C0;
		g_CdpLicenseState.g_OPS_T = Local->l_OPS_T;
		g_CdpLicenseState.g_OPS_S = Local->l_OPS_S;
		CdpLicenseClearCapLocked(); /* 篡改路径不得留下可用 token */
		persist = TRUE;
		tamper = TRUE;
		status = STATUS_SUCCESS;
		Cdp_LIC_FAIL("memory/E0 mismatch (tamper)");
		goto done;
	}

	status = CdpLicenseCheckValidityWithLocal(Local);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	/* OPS_T++ then persist before business（设计 §6.5：对照通过后、业务前） */
	Local->l_OPS_T += 1;
	g_CdpLicenseState.g_OPS_T = Local->l_OPS_T;
	/* token 绑定「+1 之后」的 OPS_T；Arm/After 必须用同一组水位才能重算通过 */
	status = CdpLicenseMintCapLocked(Local);
	if (!NT_SUCCESS(status))
		goto done;
	persist = TRUE;
	status = STATUS_SUCCESS;

done:
	CdpLocalSealSecureZero(&plain, sizeof(plain));
	CdpLicenseUnlock();
	if (persist && NT_SUCCESS(status))
	{
		status = CdpLicensePersistToAllJournals(DriverExt);
		if (NT_SUCCESS(status) && tamper)
			status = STATUS_CDP_LICENSE_TAMPER;
	}
	return status;
}

NTSTATUS CdpLicenseGateArmOp(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_LICENSE_LOCAL_STATE* Local)
{
	UCHAR expect[Cdp_CAP_TOKEN_BYTES];
	NTSTATUS status;

	/*
	 * 真正调用 CdpCoreRecoveryBegin 之前的第二道闸。
	 * 仅检查 NT_SUCCESS(BeforeOp) 不够：攻击者可 NOP BeforeOp 后伪造 STATUS_SUCCESS。
	 * 这里要求 Local 里的 token 与全局 pending 一致，并能用 SealKey 从水位重算出来。
	 */
	status = CdpLicenseRequireIntegrity(DriverExt);
	if (!NT_SUCCESS(status))
		return status;
	if (!Local || Local->CapTokenValid == 0)
	{
		Cdp_LIC_FAIL("arm: missing cap token");
		return CdpLicenseNoteIntegrityFailure(DriverExt);
	}

	CdpLicenseLock();
	if (!g_CdpLicenseState.PendingCapValid ||
		Local->CapNonce != g_CdpLicenseState.PendingCapNonce ||
		Local->CapIssued100ns != g_CdpLicenseState.PendingCapIssued100ns ||
		!CdpLocalSealCapTokenEqual(Local->CapToken, g_CdpLicenseState.PendingCapToken))
	{
		CdpLicenseUnlock();
		Cdp_LIC_FAIL("arm: token mismatch vs pending");
		return CdpLicenseNoteIntegrityFailure(DriverExt);
	}
	/* 用 Local 快照（含已 +1 的 OPS_T）重算，防止调用方改了水位却仍拿着旧 token */
	status = CdpLicenseRecomputeCap(
		Local->l_T0,
		Local->l_C0,
		Local->l_OPS_T,
		Local->l_OPS_S,
		Local->l_A_MOD,
		expect);
	if (!NT_SUCCESS(status) ||
		!CdpLocalSealCapTokenEqual(expect, g_CdpLicenseState.PendingCapToken))
	{
		CdpLocalSealSecureZero(expect, sizeof(expect));
		CdpLicenseUnlock();
		Cdp_LIC_FAIL("arm: token recompute failed");
		return CdpLicenseNoteIntegrityFailure(DriverExt);
	}
	CdpLocalSealSecureZero(expect, sizeof(expect));
	g_CdpLicenseState.PendingCapArmed = TRUE;
	CdpLicenseUnlock();
	return STATUS_SUCCESS;
}

/* Begin 中途失败（QueryTimeRange / RecoveryBegin / Arm 失败）时丢掉 pending，避免下次误用。 */
VOID CdpLicenseGateAbortOp(VOID)
{
	CdpLicenseLock();
	CdpLicenseClearCapLocked();
	CdpLicenseUnlock();
}

NTSTATUS CdpLicenseGateAfterOpSuccess(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_LICENSE_LOCAL_STATE Local)
{
	UCHAR expect[Cdp_CAP_TOKEN_BYTES];
	NTSTATUS status;
	BOOLEAN persist = FALSE;
	BOOLEAN tamper = FALSE;

	UNREFERENCED_PARAMETER(Local);

	RtlZeroMemory(expect, sizeof(expect));
	status = CdpLicenseRequireIntegrity(DriverExt);
	if (!NT_SUCCESS(status))
		return status;

	CdpLicenseLock();
	/*
	 * Commit 在 Recovery 已经写盘之后才调用。即使攻击者跳过 Before/Arm 完成了回填，
	 * 这里没有 Armed token 就不会 OPS_S++ / C0--，并记 A_MOD。
	 * 计数以全局 g_* 为准，不信任调用方传入的 Local（IrpDispatch 现已传零结构）。
	 */
	if (!g_CdpLicenseState.PendingCapValid || !g_CdpLicenseState.PendingCapArmed)
	{
		Cdp_LIC_FAIL("after: cap not armed");
		g_CdpLicenseState.g_A_MOD += 1;
		persist = TRUE;
		tamper = TRUE;
		status = STATUS_SUCCESS;
		goto done;
	}
	status = CdpLicenseRecomputeCap(
		g_CdpLicenseState.g_T0,
		g_CdpLicenseState.g_C0,
		g_CdpLicenseState.g_OPS_T,
		g_CdpLicenseState.g_OPS_S,
		g_CdpLicenseState.g_A_MOD,
		expect);
	if (!NT_SUCCESS(status) ||
		!CdpLocalSealCapTokenEqual(expect, g_CdpLicenseState.PendingCapToken))
	{
		Cdp_LIC_FAIL("after: cap recompute mismatch");
		g_CdpLicenseState.g_A_MOD += 1;
		persist = TRUE;
		tamper = TRUE;
		status = STATUS_SUCCESS;
		goto done;
	}

	g_CdpLicenseState.g_OPS_S += 1;
	if (g_CdpLicenseState.Mode == Cdp_LICENSE_MODE_COUNTER ||
		g_CdpLicenseState.Mode == Cdp_LICENSE_MODE_HYBRID)
	{
		if (g_CdpLicenseState.g_C0 > 0)
			g_CdpLicenseState.g_C0 -= 1;
	}
	if (Local)
	{
		Local->l_T0 = g_CdpLicenseState.g_T0;
		Local->l_C0 = g_CdpLicenseState.g_C0;
		Local->l_OPS_T = g_CdpLicenseState.g_OPS_T;
		Local->l_OPS_S = g_CdpLicenseState.g_OPS_S;
		Local->l_A_MOD = g_CdpLicenseState.g_A_MOD;
	}
	CdpLicenseClearCapLocked(); /* 一次性 token：成功记账后立即作废 */
	persist = TRUE;
	status = STATUS_SUCCESS;

done:
	CdpLocalSealSecureZero(expect, sizeof(expect));
	CdpLicenseUnlock();
	if (persist && NT_SUCCESS(status))
	{
		status = CdpLicensePersistToAllJournals(DriverExt);
		if (NT_SUCCESS(status) && tamper)
			status = STATUS_CDP_LICENSE_TAMPER;
	}
	return status;
}

NTSTATUS CdpLicenseQueryStatus(
	_Out_writes_bytes_to_(OutLength, *Written) PVOID OutBuffer,
	_In_ ULONG OutLength,
	_Out_ PULONG Written)
{
	PCdp_LICENSE_QUERY_REPLY reply;

	*Written = 0;
	if (!OutBuffer || OutLength < Cdp_LICENSE_QUERY_REPLY_V1_BYTES)
	{
		Cdp_LIC_FAIL("STATUS_BUFFER_TOO_SMALL: if (!OutBuffer || OutLength < Cdp_LICENSE_QUERY_REPLY_V1_BYTES)");
		return STATUS_BUFFER_TOO_SMALL;
	}
	reply = (PCdp_LICENSE_QUERY_REPLY)OutBuffer;
	RtlZeroMemory(reply, OutLength < sizeof(*reply) ? OutLength : sizeof(*reply));
	CdpLicenseLock();
	/* 与闸门一致：仅当证书已加载且本地密封状态可用时，才视为“已授权” */
	reply->HasLicense = (g_CdpLicenseState.HasLicense &&
		g_CdpLicenseState.SealKeyValid &&
		g_CdpLicenseState.E0Length > 0) ? 1 : 0;
	reply->Mode = g_CdpLicenseState.Mode;
	reply->T0_100ns = g_CdpLicenseState.g_T0;
	reply->T_EXP_100ns = g_CdpLicenseState.T_EXP_100ns;
	reply->C0 = g_CdpLicenseState.g_C0;
	reply->OPS_T = g_CdpLicenseState.g_OPS_T;
	reply->OPS_S = g_CdpLicenseState.g_OPS_S;
	reply->A_MOD = g_CdpLicenseState.g_A_MOD;
	if (OutLength >= sizeof(*reply))
	{
		reply->IsTrial = g_CdpLicenseState.IsTrial ? 1 : 0;
		reply->Reserved = 0;
		*Written = sizeof(*reply);
	}
	else
	{
		*Written = Cdp_LICENSE_QUERY_REPLY_V1_BYTES;
	}
	CdpLicenseUnlock();
	return STATUS_SUCCESS;
}

NTSTATUS CdpLicenseExportReceipt(
	_Out_writes_bytes_to_(OutLength, *Written) PVOID OutBuffer,
	_In_ ULONG OutLength,
	_Out_ PULONG Written)
{
	PCdp_LICENSE_RECEIPT_REPLY reply;

	*Written = 0;
	if (!OutBuffer || OutLength < sizeof(Cdp_LICENSE_RECEIPT_REPLY))
	{
		Cdp_LIC_FAIL("STATUS_BUFFER_TOO_SMALL: if (!OutBuffer || OutLength < sizeof(Cdp_LICENSE_RECEIPT_REPLY))");
		return STATUS_BUFFER_TOO_SMALL;
	}
	reply = (PCdp_LICENSE_RECEIPT_REPLY)OutBuffer;
	RtlZeroMemory(reply, sizeof(*reply));
	CdpLicenseLock();
	reply->OPS_T = g_CdpLicenseState.g_OPS_T;
	reply->OPS_S = g_CdpLicenseState.g_OPS_S;
	reply->A_MOD = g_CdpLicenseState.g_A_MOD;
	RtlCopyMemory(reply->DeviceFingerprint,
		g_CdpLicenseState.DeviceFingerprint, Cdp_LICENSE_FP_BYTES);
	RtlStringCbCopyA(reply->MbUuid, sizeof(reply->MbUuid),
		g_CdpLicenseState.MbUuid);
	RtlStringCbCopyA(reply->DiskSerial, sizeof(reply->DiskSerial),
		g_CdpLicenseState.DiskSerial);
	RtlStringCbCopyA(reply->PrevLicenseId, sizeof(reply->PrevLicenseId),
		g_CdpLicenseState.LicenseId);
	reply->T_CLIENT_100ns = CdpLicenseQuerySystemTime100ns();
	CdpLicenseUnlock();
	*Written = sizeof(*reply);
	return STATUS_SUCCESS;
}

NTSTATUS CdpLicenseBuildApplyQrPayload(
	_In_ ULONG DesiredDurationSec,
	_In_ ULONG DesiredCredits,
	_In_ ULONG Mode,
	_In_ ULONG Kind,
	_In_reads_(Cdp_APPLY_QR_PREFIX_MAX) const CHAR* QrPrefix,
	_Out_writes_bytes_to_(OutLength, *Written) PVOID OutBuffer,
	_In_ ULONG OutLength,
	_Out_ PULONG Written)
{
	/*
	 * 组装申请 JSON（含回执 OPS/A_MOD）→ RSA-OAEP+AES-GCM → Base64
	 * → QrPayload = 应用层前缀 + Base64。GUI 只需对 QrPayload 生成二维码。
	 */
	CHAR prefix[Cdp_APPLY_QR_PREFIX_MAX];
	CHAR mb[Cdp_LICENSE_MB_UUID_CHARS];
	CHAR disk[Cdp_LICENSE_DISK_SERIAL_CHARS];
	UCHAR fp[Cdp_LICENSE_FP_BYTES];
	CHAR plain[Cdp_APPLY_PLAINTEXT_MAX];
	UCHAR aesKey[32];
	UCHAR nonce[12];
	UCHAR ek[256];
	ULONG ekLen = 0;
	UCHAR ct[Cdp_APPLY_PLAINTEXT_MAX + 16];
	ULONG ctLen = 0;
	UCHAR aad = 0x01;
	UCHAR packet[Cdp_APPLY_CIPHERTEXT_MAX];
	ULONG packetLen = 0;
	CHAR b64[Cdp_APPLY_QR_PAYLOAD_MAX];
	ULONG b64Len = 0;
	PCdp_LICENSE_APPLY_QR_REPLY reply;
	TIME_FIELDS tf;
	LARGE_INTEGER now;
	CHAR tClient[40];
	CHAR nonceStr[33];
	const CHAR* kindStr;
	ULONG durationSec;
	ULONG credits;
	ULONG i;
	NTSTATUS status;

	*Written = 0;
	if (!OutBuffer || OutLength < sizeof(Cdp_LICENSE_APPLY_QR_REPLY))
	{
		Cdp_LIC_FAIL("STATUS_BUFFER_TOO_SMALL: if (!OutBuffer || OutLength < sizeof(Cdp_LICENSE_APPLY_QR_REPLY))");
		return STATUS_BUFFER_TOO_SMALL;
	}
	status = CdpLicenseSanitizeApplyQrPrefix(QrPrefix, prefix);
	if (!NT_SUCCESS(status))
		return status;
	if (Mode != Cdp_LICENSE_MODE_TIME &&
		Mode != Cdp_LICENSE_MODE_COUNTER &&
		Mode != Cdp_LICENSE_MODE_HYBRID)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (Mode != Cdp_LICENSE_MODE_TIME &&");
		return STATUS_INVALID_PARAMETER;
	}
	if (Kind != Cdp_LICENSE_KIND_PAID && Kind != Cdp_LICENSE_KIND_TRIAL)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: Kind");
		return STATUS_INVALID_PARAMETER;
	}
	durationSec = DesiredDurationSec;
	credits = DesiredCredits;
	if (Kind == Cdp_LICENSE_KIND_PAID)
	{
		if ((Mode == Cdp_LICENSE_MODE_TIME || Mode == Cdp_LICENSE_MODE_HYBRID) &&
			durationSec == 0)
		{
			Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: paid duration");
			return STATUS_INVALID_PARAMETER;
		}
		if ((Mode == Cdp_LICENSE_MODE_COUNTER || Mode == Cdp_LICENSE_MODE_HYBRID) &&
			credits == 0)
		{
			Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: paid credits");
			return STATUS_INVALID_PARAMETER;
		}
		if (Mode == Cdp_LICENSE_MODE_TIME)
			credits = 0;
		else if (Mode == Cdp_LICENSE_MODE_COUNTER)
			durationSec = 0;
	}
	kindStr = (Kind == Cdp_LICENSE_KIND_TRIAL) ? "trial" : "paid";

	reply = (PCdp_LICENSE_APPLY_QR_REPLY)OutBuffer;
	RtlZeroMemory(reply, sizeof(*reply));

	status = CdpLicenseCollectHardwareId(mb, disk, fp);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	KeQuerySystemTimePrecise(&now);
	RtlTimeToTimeFields(&now, &tf);
	RtlStringCbPrintfA(tClient, sizeof(tClient),
		"%04d-%02d-%02dT%02d:%02d:%02dZ",
		tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second);

	status = CdpLicenseRandomBytes(aesKey, sizeof(aesKey));
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	status = CdpLicenseRandomBytes(nonce, sizeof(nonce));
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	for (i = 0; i < 16; ++i)
		RtlStringCbPrintfA(nonceStr + i * 2, 3, "%02x", nonce[i % 12]);

	CdpLicenseLock();
	status = RtlStringCbPrintfA(
		plain,
		sizeof(plain),
		"{\"mb_uuid\":\"%s\",\"disk_serial\":\"%s\",\"t_client\":\"%s\","
		"\"desired_duration_sec\":%lu,\"desired_credits\":%lu,\"mode\":%lu,"
		"\"kind\":\"%s\",\"receipt\":{\"ops_t\":%lu,\"ops_s\":%lu,\"a_mod\":%lu,"
		"\"prev_license_id\":%s},\"nonce\":\"%s\"}",
		mb,
		disk,
		tClient,
		durationSec,
		credits,
		Mode,
		kindStr,
		g_CdpLicenseState.g_OPS_T,
		g_CdpLicenseState.g_OPS_S,
		g_CdpLicenseState.g_A_MOD,
		g_CdpLicenseState.LicenseId[0] ?
			g_CdpLicenseState.LicenseId : "null",
		nonceStr);
	CdpLicenseUnlock();
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	/* Fix prev_license_id quoting when non-null. */
	if (g_CdpLicenseState.LicenseId[0])
	{
		CHAR fixed[Cdp_APPLY_PLAINTEXT_MAX];
		CdpLicenseLock();
		RtlStringCbPrintfA(
			fixed,
			sizeof(fixed),
			"{\"mb_uuid\":\"%s\",\"disk_serial\":\"%s\",\"t_client\":\"%s\","
			"\"desired_duration_sec\":%lu,\"desired_credits\":%lu,\"mode\":%lu,"
			"\"kind\":\"%s\",\"receipt\":{\"ops_t\":%lu,\"ops_s\":%lu,\"a_mod\":%lu,"
			"\"prev_license_id\":\"%s\"},\"nonce\":\"%s\"}",
			mb, disk, tClient, durationSec, credits, Mode, kindStr,
			g_CdpLicenseState.g_OPS_T, g_CdpLicenseState.g_OPS_S,
			g_CdpLicenseState.g_A_MOD, g_CdpLicenseState.LicenseId, nonceStr);
		CdpLicenseUnlock();
		RtlStringCbCopyA(plain, sizeof(plain), fixed);
	}

	#ifdef LIC_DEBUG
	DbgPrint("CdpLicenseBuildApplyQrPayload: plain = %s\n", plain);
	#endif

	status = CdpLicenseRsaOaepEncrypt(
		aesKey, sizeof(aesKey), ek, sizeof(ek), &ekLen); // 使用驱动内置的公钥加密aesKey
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	status = CdpLicenseAesGcmEncrypt(
		aesKey, nonce, &aad, 1,
		(const UCHAR*)plain,
		(ULONG)CdpLicenseStrLen(plain, sizeof(plain)),
		ct, sizeof(ct), &ctLen); // 
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	if (1 + 2 + ekLen + 12 + ctLen > sizeof(packet))
	{
		Cdp_LIC_FAIL("STATUS_BUFFER_TOO_SMALL then goto done;");
		status = STATUS_BUFFER_TOO_SMALL;
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	packet[0] = 0x01;
	packet[1] = (UCHAR)((ekLen >> 8) & 0xFF);
	packet[2] = (UCHAR)(ekLen & 0xFF);
	RtlCopyMemory(packet + 3, ek, ekLen);
	RtlCopyMemory(packet + 3 + ekLen, nonce, 12);
	RtlCopyMemory(packet + 3 + ekLen + 12, ct, ctLen);
	packetLen = 3 + ekLen + 12 + ctLen;

	status = CdpLicenseBase64Encode(
		packet, packetLen, b64, sizeof(b64), &b64Len);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	/* 禁止 printf("%s%s", 用户前缀, …)：前缀里的 % 会炸。 */
	status = RtlStringCbCopyA(reply->QrPayload, sizeof(reply->QrPayload), prefix);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	status = RtlStringCbCatA(reply->QrPayload, sizeof(reply->QrPayload), b64);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	reply->CiphertextLength = packetLen;
	if (packetLen <= sizeof(reply->Ciphertext))
		RtlCopyMemory(reply->Ciphertext, packet, packetLen);
	RtlCopyMemory(reply->DeviceFingerprint, fp, Cdp_LICENSE_FP_BYTES);
	*Written = sizeof(*reply);
#ifdef LIC_DEBUG
	/*
	 * 仅当调用方按 EX 布局预留了缓冲（CdpConsole Debug-Lic）才回填明文。
	 * GUI 使用稳定 ABI 大小，不得因此失败。
	 */
	if (OutLength >= sizeof(Cdp_LICENSE_APPLY_QR_REPLY_EX))
	{
		PCdp_LICENSE_APPLY_QR_REPLY_EX ex =
			(PCdp_LICENSE_APPLY_QR_REPLY_EX)OutBuffer;
		RtlStringCbCopyA(ex->Plaintext, sizeof(ex->Plaintext), plain);
		*Written = sizeof(*ex);
	}
#endif

done:
	CdpLocalSealSecureZero(aesKey, sizeof(aesKey));
	CdpLocalSealSecureZero(plain, sizeof(plain));
	return status;
}

#include "CdpLicenseSegEnd.h" /* 恢复默认 code/const 节 */

#endif /* CDP_LICENSE */
