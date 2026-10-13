/*
 * Journal v19 密码信封。
 * 用厂商公钥做 RSA-OAEP-SHA256，CRC32C 只覆盖算法、长度和 256 字节密文。
 * 凭据与信封由 CdpJournalSetCredentialBundle 一次写入；本文件不做 WP9 找回。
 */
#include "CdpEngineDefs.h"
#include "CdpCredential.h"

#ifdef CDP_LICENSE
#include "CdpPasswordEnvelope.h"
#ifdef CDP_LICENSE_OBFUSCATE
#include "CdpLicenseTrust.h"
#endif

#define CDP_ENVELOPE_CRC_POLY 0x82F63B78UL

static ULONG g_CdppEnvelopeCrcTable[256];
static volatile LONG g_CdppEnvelopeCrcReady;

/* 短暂让出 CPU，避免初始化 CRC 表时空转占满。 */
static VOID CdppStall(VOID)
{
	KeStallExecutionProcessor(1);
}

/* CRC 表只初始化一次，其它线程等到表就绪。 */
static VOID CdppInitCrc32c(VOID)
{
	ULONG table[256];
	ULONG index;

	if (InterlockedCompareExchange(&g_CdppEnvelopeCrcReady, 1, 0) != 0)
	{
		while (InterlockedCompareExchange(&g_CdppEnvelopeCrcReady, 0, 0) != 2)
			CdppStall();
		return;
	}
	for (index = 0; index < RTL_NUMBER_OF(table); ++index)
	{
		ULONG crc = index;
		ULONG bit;

		for (bit = 0; bit < 8; ++bit)
			crc = (crc & 1) ? ((crc >> 1) ^ CDP_ENVELOPE_CRC_POLY) : (crc >> 1);
		table[index] = crc;
	}
	RtlCopyMemory(g_CdppEnvelopeCrcTable, table, sizeof(table));
	InterlockedExchange(&g_CdppEnvelopeCrcReady, 2);
}

/* 与 CdpJournal.c 的 Castagnoli CRC32C 使用同一多项式。InitialCrc 先异或再按字节更新。 */
static ULONG CdppCrc32c(
	_In_ ULONG InitialCrc,
	_In_reads_bytes_(Length) const VOID* Buffer,
	_In_ SIZE_T Length)
{
	const UCHAR* bytes = (const UCHAR*)Buffer;
	ULONG crc = InitialCrc ^ 0xFFFFFFFFUL;

	if (InterlockedCompareExchange(&g_CdppEnvelopeCrcReady, 0, 0) != 2)
		CdppInitCrc32c();
	while (Length--)
		crc = g_CdppEnvelopeCrcTable[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFUL;
}

/* 卷句柄表的释放函数定义在 CdpIrpDispatchs.c，这里只增加引用后归还。 */
VOID CdpReleaseVolumeHandleEntry(_In_ PCdp_VOLUME_HANDLE_ENTRY Item);

/* 钉住的 Journal 及其改写前的 Bundle，回滚时按这份快照写回。 */
typedef struct _CDP_CREDENTIAL_SNAP
{
	Cdp_CREDENTIAL_DESCRIPTOR Credential;
	Cdp_PASSWORD_ENVELOPE Envelope;
	BOOLEAN EnvelopeConfigured;
	ULONG SuperblockVersion;
} CDP_CREDENTIAL_SNAP;

/* 校验算法、密文长度和信封自身 CRC。不解密。 */
BOOLEAN CdpPasswordEnvelopeValid(
	_In_ const Cdp_PASSWORD_ENVELOPE* Envelope)
{
	ULONG crc;

	if (!Envelope)
		return FALSE;
	/* 长度必须刚好是 RSA-2048 密文，不能把未使用的数组尾部算进 CRC。 */
	if (Envelope->Algorithm != Cdp_PASSWORD_ENVELOPE_ALG_RSA_OAEP_SHA256 ||
		Envelope->CiphertextLength != Cdp_PASSWORD_ENVELOPE_MAX)
	{
		return FALSE;
	}
	crc = CdppCrc32c(
		0,
		Envelope,
		FIELD_OFFSET(Cdp_PASSWORD_ENVELOPE, Crc32c));
	return crc == Envelope->Crc32c;
}

/* 用厂商公钥加密口令明文，并写入信封 CRC。返回前清零栈上明文。 */
NTSTATUS CdpPasswordEnvelopeCreate(
	_In_reads_bytes_(PasswordBytes) const UCHAR* Password,
	_In_ ULONG PasswordBytes,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_ Cdp_PASSWORD_ENVELOPE* Envelope)
{
#ifndef CDP_LICENSE_OBFUSCATE
	UNREFERENCED_PARAMETER(Password);
	UNREFERENCED_PARAMETER(PasswordBytes);
	UNREFERENCED_PARAMETER(Credential);
	UNREFERENCED_PARAMETER(Envelope);
	return STATUS_NOT_SUPPORTED;
#else
	Cdp_PASSWORD_ENVELOPE_PLAIN plain;
	ULONG plainBytes;
	ULONG outLength = 0;
	NTSTATUS status;

	if (!Password || !Credential || !Envelope ||
		PasswordBytes == 0 ||
		PasswordBytes > Cdp_PASSWORD_MAX_UTF8_BYTES ||
		Credential->AuthEpoch == 0)
	{
		return STATUS_INVALID_PARAMETER;
	}

	RtlZeroMemory(Envelope, sizeof(*Envelope));
	RtlZeroMemory(&plain, sizeof(plain));
	plain.Magic = Cdp_PASSWORD_ENVELOPE_MAGIC;
	plain.Version = Cdp_PASSWORD_ENVELOPE_PLAIN_VERSION;
	plain.CredentialId = Credential->CredentialId;
	plain.AuthEpoch = Credential->AuthEpoch;
	plain.PasswordBytes = PasswordBytes;
	RtlCopyMemory(plain.Password, Password, PasswordBytes);
	plainBytes = FIELD_OFFSET(Cdp_PASSWORD_ENVELOPE_PLAIN, Password) + PasswordBytes;

	Envelope->Algorithm = Cdp_PASSWORD_ENVELOPE_ALG_RSA_OAEP_SHA256;
	status = CdpLicenseRsaOaepEncrypt(
		(const UCHAR*)&plain,
		plainBytes,
		Envelope->Ciphertext,
		Cdp_PASSWORD_ENVELOPE_MAX,
		&outLength);
	RtlSecureZeroMemory(&plain, sizeof(plain));
	if (!NT_SUCCESS(status) || outLength != Cdp_PASSWORD_ENVELOPE_MAX)
	{
		RtlSecureZeroMemory(Envelope, sizeof(*Envelope));
		return NT_SUCCESS(status) ? STATUS_INVALID_BUFFER_SIZE : status;
	}
	Envelope->CiphertextLength = outLength;
	Envelope->Crc32c = CdppCrc32c(
		0,
		Envelope,
		FIELD_OFFSET(Cdp_PASSWORD_ENVELOPE, Crc32c));
	return STATUS_SUCCESS;
#endif
}

/* 释放钉住期间增加的引用，并擦掉快照里的校验器。 */
static VOID CdppFreePinnedJournals(
	_In_opt_ PCdp_VOLUME_HANDLE_ENTRY* Entries,
	_In_ ULONG Count,
	_In_opt_ CDP_CREDENTIAL_SNAP* Snaps)
{
	ULONG index;

	for (index = 0; index < Count; ++index)
		CdpReleaseVolumeHandleEntry(Entries[index]);
	if (Snaps && Count != 0)
		RtlSecureZeroMemory(Snaps, sizeof(*Snaps) * Count);
	if (Entries)
		cdpfree(Entries);
	if (Snaps)
		cdpfree(Snaps);
}

/*
 * 在卷句柄锁内钉住已挂载且带凭据的 Journal。
 * Expected 非空时只钉住整份凭据描述符都相同的项。锁外才能写盘。
 */
static NTSTATUS CdppPinCredentialJournals(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_opt_ const Cdp_CREDENTIAL_DESCRIPTOR* Expected,
	_Outptr_result_maybenull_ PCdp_VOLUME_HANDLE_ENTRY** Entries,
	_Outptr_result_maybenull_ CDP_CREDENTIAL_SNAP** Snaps,
	_Out_ PULONG Count)
{
	PLIST_ENTRY entry;
	ULONG capacity = 0;
	NTSTATUS status = STATUS_NOT_FOUND;

	*Entries = NULL;
	*Snaps = NULL;
	*Count = 0;
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	for (entry = DriverExt->VolumeHandleList.Flink;
		entry != &DriverExt->VolumeHandleList;
		entry = entry->Flink)
	{
		++capacity;
	}
	if (capacity == 0)
	{
		ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
		return STATUS_NOT_FOUND;
	}
	*Entries = (PCdp_VOLUME_HANDLE_ENTRY*)cdpalloc(sizeof(**Entries) * capacity);
	*Snaps = (CDP_CREDENTIAL_SNAP*)cdpalloc(sizeof(**Snaps) * capacity);
	if (!*Entries || !*Snaps)
	{
		ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
		if (*Entries)
			cdpfree(*Entries);
		if (*Snaps)
			cdpfree(*Snaps);
		*Entries = NULL;
		*Snaps = NULL;
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(*Snaps, sizeof(**Snaps) * capacity);
	for (entry = DriverExt->VolumeHandleList.Flink;
		entry != &DriverExt->VolumeHandleList;
		entry = entry->Flink)
	{
		PCdp_VOLUME_HANDLE_ENTRY item =
			CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		CDP_CREDENTIAL_SNAP* snap = &(*Snaps)[*Count];

		if (item->Closing || !item->Journal.Mounted)
			continue;
		if (!CdpJournalGetCredentialBundle(
			&item->Journal,
			&snap->Credential,
			&snap->Envelope,
			&snap->EnvelopeConfigured,
			&snap->SuperblockVersion))
		{
			continue;
		}
		if (Expected &&
			RtlCompareMemory(Expected, &snap->Credential, sizeof(*Expected)) !=
				sizeof(*Expected))
		{
			RtlSecureZeroMemory(snap, sizeof(*snap));
			continue;
		}
		InterlockedIncrement(&item->ReferenceCount);
		(*Entries)[(*Count)++] = item;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	if (*Count == 0)
	{
		CdppFreePinnedJournals(*Entries, 0, *Snaps);
		*Entries = NULL;
		*Snaps = NULL;
		return status;
	}
	return STATUS_SUCCESS;
}

/* 按快照写回。v19 用 Bundle，v18 去掉信封。写失败只记录，由调用方返回原始错误。 */
static VOID CdppRollbackSnaps(
	_In_reads_(Changed) PCdp_VOLUME_HANDLE_ENTRY* Entries,
	_In_reads_(Changed) CDP_CREDENTIAL_SNAP* Snaps,
	_In_ ULONG Changed,
	_In_ NTSTATUS OriginalStatus)
{
	ULONG index;

	for (index = 0; index < Changed; ++index)
	{
		NTSTATUS rollback;

		if (Snaps[index].EnvelopeConfigured &&
			Snaps[index].SuperblockVersion == Cdp_JOURNAL_VERSION_V19)
		{
			rollback = CdpJournalSetCredentialBundle(
				&Entries[index]->Journal,
				&Snaps[index].Credential,
				&Snaps[index].Envelope);
		}
		else
		{
			rollback = CdpJournalRestoreCredentialWithoutEnvelope(
				&Entries[index]->Journal,
				&Snaps[index].Credential);
		}
		if (!NT_SUCCESS(rollback))
		{
			Cdp_LOG("[ENVELOPE] rollback write failed status=0x%08X original=0x%08X\n",
				rollback,
				OriginalStatus);
		}
	}
}

/* 把同一份新 Bundle 写到全部钉住的 Journal。中途失败立即回滚已写项。 */
static NTSTATUS CdppWriteBundleAll(
	_In_reads_(Count) PCdp_VOLUME_HANDLE_ENTRY* Entries,
	_In_reads_(Count) CDP_CREDENTIAL_SNAP* Snaps,
	_In_ ULONG Count,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_In_ const Cdp_PASSWORD_ENVELOPE* Envelope)
{
	ULONG index;
	ULONG changed = 0;
	NTSTATUS status = STATUS_SUCCESS;

	for (index = 0; index < Count; ++index)
	{
		status = CdpJournalSetCredentialBundle(
			&Entries[index]->Journal,
			Credential,
			Envelope);
		changed = index + 1;
		if (!NT_SUCCESS(status))
			break;
	}
	if (!NT_SUCCESS(status))
		CdppRollbackSnaps(Entries, Snaps, changed, status);
	return status;
}

/* 读取当前已挂载 Journal 的共享凭据和信封。缺信封则要求先迁移。 */
NTSTATUS CdpFindSharedPasswordBundle(
	_In_ struct _Cdp_DRIVER_EXTENSION* DriverExt,
	_Out_ Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_ Cdp_PASSWORD_ENVELOPE* Envelope)
{
	PCdp_VOLUME_HANDLE_ENTRY* entries = NULL;
	CDP_CREDENTIAL_SNAP* snaps = NULL;
	ULONG count = 0;
	ULONG index;
	NTSTATUS status;

	if (!DriverExt || !Credential || !Envelope)
		return STATUS_INVALID_PARAMETER;
	RtlZeroMemory(Credential, sizeof(*Credential));
	RtlZeroMemory(Envelope, sizeof(*Envelope));
	status = CdppPinCredentialJournals(
		(PCdp_DRIVER_EXTENSION)DriverExt,
		NULL,
		&entries,
		&snaps,
		&count);
	if (!NT_SUCCESS(status))
		return status;

	/* 已挂载凭据必须完全一致，并且都已经有同一份信封。 */
	for (index = 0; index < count; ++index)
	{
		if (RtlCompareMemory(
			&snaps[0].Credential,
			&snaps[index].Credential,
			sizeof(snaps[0].Credential)) != sizeof(snaps[0].Credential))
		{
			status = STATUS_OBJECT_TYPE_MISMATCH;
			break;
		}
		if (!snaps[index].EnvelopeConfigured ||
			snaps[index].SuperblockVersion != Cdp_JOURNAL_VERSION_V19 ||
			!CdpPasswordEnvelopeValid(&snaps[index].Envelope))
		{
			status = STATUS_REVISION_MISMATCH;
			break;
		}
		if (index > 0 &&
			RtlCompareMemory(
				&snaps[0].Envelope,
				&snaps[index].Envelope,
				sizeof(snaps[0].Envelope)) != sizeof(snaps[0].Envelope))
		{
			status = STATUS_OBJECT_TYPE_MISMATCH;
			break;
		}
	}
	if (NT_SUCCESS(status))
	{
		*Credential = snaps[0].Credential;
		*Envelope = snaps[0].Envelope;
	}
	CdppFreePinnedJournals(entries, count, snaps);
	return status;
}

/* 改密事务：钉住旧凭据匹配项，写入新 Bundle，失败则按快照回滚。 */
NTSTATUS CdpCommitSharedCredentialBundle(
	_In_ struct _Cdp_DRIVER_EXTENSION* DriverExt,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* OldCredential,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* NewCredential,
	_In_ const Cdp_PASSWORD_ENVELOPE* NewEnvelope)
{
	PCdp_VOLUME_HANDLE_ENTRY* entries = NULL;
	CDP_CREDENTIAL_SNAP* snaps = NULL;
	ULONG count = 0;
	NTSTATUS status;

	if (!DriverExt || !OldCredential || !NewCredential || !NewEnvelope ||
		!CdpPasswordEnvelopeValid(NewEnvelope))
	{
		return STATUS_INVALID_PARAMETER;
	}
	status = CdppPinCredentialJournals(
		(PCdp_DRIVER_EXTENSION)DriverExt,
		OldCredential,
		&entries,
		&snaps,
		&count);
	if (!NT_SUCCESS(status))
		return status;
	status = CdppWriteBundleAll(
		entries,
		snaps,
		count,
		NewCredential,
		NewEnvelope);
	CdppFreePinnedJournals(entries, count, snaps);
	return status;
}

/* 重验口令。信封已经一致则直接成功，否则补写成同一份 v19 Bundle。 */
NTSTATUS CdpEnsurePasswordEnvelopes(
	_In_ struct _Cdp_DRIVER_EXTENSION* DriverExt,
	_In_reads_bytes_(PasswordBytes) const UCHAR* Password,
	_In_ ULONG PasswordBytes)
{
	PCdp_DRIVER_EXTENSION ext = (PCdp_DRIVER_EXTENSION)DriverExt;
	PCdp_VOLUME_HANDLE_ENTRY* entries = NULL;
	CDP_CREDENTIAL_SNAP* snaps = NULL;
	Cdp_PASSWORD_ENVELOPE envelope;
	ULONG count = 0;
	ULONG index;
	BOOLEAN uniform = TRUE;
	BOOLEAN haveEnvelope = FALSE;
	NTSTATUS status;

	if (!ext || !Password ||
		PasswordBytes == 0 ||
		PasswordBytes > Cdp_PASSWORD_MAX_UTF8_BYTES)
	{
		return STATUS_INVALID_PARAMETER;
	}
	RtlZeroMemory(&envelope, sizeof(envelope));
	status = CdppPinCredentialJournals(ext, NULL, &entries, &snaps, &count);
	if (!NT_SUCCESS(status))
		return status;

	for (index = 0; index < count; ++index)
	{
		if (RtlCompareMemory(
			&snaps[0].Credential,
			&snaps[index].Credential,
			sizeof(snaps[0].Credential)) != sizeof(snaps[0].Credential))
		{
			status = STATUS_OBJECT_TYPE_MISMATCH;
			goto done;
		}
	}
	if (!CdpCredentialVerify(Password, PasswordBytes, &snaps[0].Credential))
	{
		status = STATUS_ACCESS_DENIED;
		goto done;
	}

	/*
	 * 已有 v19 信封就复用它，避免 OAEP 每次生成不同密文。
	 * 全部相同才是幂等成功；有的还是 v18 时把这份信封补到其余 Journal。
	 */
	for (index = 0; index < count; ++index)
	{
		if (!snaps[index].EnvelopeConfigured ||
			snaps[index].SuperblockVersion != Cdp_JOURNAL_VERSION_V19 ||
			!CdpPasswordEnvelopeValid(&snaps[index].Envelope))
		{
			uniform = FALSE;
			continue;
		}
		if (!haveEnvelope)
		{
			envelope = snaps[index].Envelope;
			haveEnvelope = TRUE;
			continue;
		}
		if (RtlCompareMemory(
			&envelope,
			&snaps[index].Envelope,
			sizeof(envelope)) != sizeof(envelope))
		{
			status = STATUS_OBJECT_TYPE_MISMATCH;
			goto done;
		}
	}
	if (uniform && haveEnvelope)
	{
		Cdp_LOG("[ENVELOPE] already uniform v19 count=%lu\n", count);
		status = STATUS_SUCCESS;
		goto done;
	}
	if (!haveEnvelope)
	{
		status = CdpPasswordEnvelopeCreate(
			Password,
			PasswordBytes,
			&snaps[0].Credential,
			&envelope);
		if (!NT_SUCCESS(status))
			goto done;
	}
	Cdp_LOG("[ENVELOPE] writing v19 bundle count=%lu reuse=%u\n",
		count,
		haveEnvelope ? 1u : 0u);
	status = CdppWriteBundleAll(
		entries,
		snaps,
		count,
		&snaps[0].Credential,
		&envelope);
done:
	RtlSecureZeroMemory(&envelope, sizeof(envelope));
	CdppFreePinnedJournals(entries, count, snaps);
	return status;
}

#else

/* 非授权构建不产生信封。保留编译单元，避免工程按配置拆文件。 */
typedef int CdpPasswordEnvelopeLicenseOnly;

#endif
