/*
 * 硬件指纹（与 server app/crypto/fingerprint.py 必须字节级一致）
 *
 * normalize_mb_uuid  : 小写、去 {} 与空白
 * normalize_disk     : Trim + 大写
 * fingerprint        : SHA256(mb || 0x00 || disk || 0x00 || tpm || 0x00 || salt)
 */

#ifdef CDP_LICENSE
#ifdef CDP_LICENSE_OBFUSCATE

#include "CdpLicenseHw.h"
#include "CdpLicenseCodec.h"
#include "CdpLicenseTrust.h"
#include "CdpEngineDefs.h"
#include <ntddstor.h>
#include <ntstrsafe.h>
#include "CdpLicenseSeg.h" /* 指纹采集代码进入 .licprot */

#ifndef SystemFirmwareTableInformation
#define SystemFirmwareTableInformation 76
#endif

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
	_In_ ULONG SystemInformationClass,
	_Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
	_In_ ULONG SystemInformationLength,
	_Out_opt_ PULONG ReturnLength);

typedef struct _CDP_SYSTEM_FIRMWARE_TABLE_INFORMATION
{
	ULONG ProviderSignature;
	ULONG Action;
	ULONG TableID;
	ULONG TableBufferLength;
	UCHAR TableBuffer[1];
} CDP_SYSTEM_FIRMWARE_TABLE_INFORMATION, *PCDP_SYSTEM_FIRMWARE_TABLE_INFORMATION;

#define SystemFirmwareTable_Get 1

VOID CdpLicenseNormalizeMbUuid(
	_In_z_ const CHAR* In,
	_Out_writes_(Cdp_LICENSE_MB_UUID_CHARS) CHAR* Out)
{
	CdpLicenseCodecNormalizeMbUuid(In, Out);
}

VOID CdpLicenseNormalizeDiskSerial(
	_In_z_ const CHAR* In,
	_Out_writes_(Cdp_LICENSE_DISK_SERIAL_CHARS) CHAR* Out)
{
	CdpLicenseCodecNormalizeDiskSerial(In, Out);
}

static VOID CdpUuidBytesToString(
	_In_reads_bytes_(16) const UCHAR* Bytes,
	_Out_writes_(Cdp_LICENSE_MB_UUID_CHARS) CHAR* Out)
{
	CdpLicenseCodecUuidBytesToString(Bytes, Out);
}

static NTSTATUS CdpReadSmbiosUuid(
	_Out_writes_(Cdp_LICENSE_MB_UUID_CHARS) CHAR* MbUuid)
{
	/* 通过 SystemFirmwareTableInformation('RSMB') 解析 Type 1 System UUID */
	NTSTATUS status;
	ULONG needed = 0;
	PCDP_SYSTEM_FIRMWARE_TABLE_INFORMATION info = NULL;
	ULONG allocSize;
	PUCHAR table;
	ULONG tableLen;
	ULONG offset;

	RtlZeroMemory(MbUuid, Cdp_LICENSE_MB_UUID_CHARS);
	status = ZwQuerySystemInformation(
		SystemFirmwareTableInformation,
		NULL,
		0,
		&needed);
	/* First probe with provider only. */
	allocSize = sizeof(CDP_SYSTEM_FIRMWARE_TABLE_INFORMATION) + 65536;
	info = (PCDP_SYSTEM_FIRMWARE_TABLE_INFORMATION)cdpalloc(allocSize);
	if (!info)
	{
		Cdp_LIC_FAIL("STATUS_INSUFFICIENT_RESOURCES: if (!info)");
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(info, allocSize);
	info->ProviderSignature = 'RSMB';
	info->Action = SystemFirmwareTable_Get;
	info->TableID = 0;
	info->TableBufferLength = allocSize - FIELD_OFFSET(
		CDP_SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer);
	status = ZwQuerySystemInformation(
		SystemFirmwareTableInformation,
		info,
		allocSize,
		&needed);
	if (!NT_SUCCESS(status))
	{
		cdpfree(info);
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}

	/* RawSMB table: 8-byte header then DMI structures. */
	if (info->TableBufferLength < 8)
	{
		cdpfree(info);
		Cdp_LIC_FAIL("STATUS_NOT_FOUND: if (info->TableBufferLength < 8)");
		return STATUS_NOT_FOUND;
	}
	table = info->TableBuffer + 8;
	tableLen = info->TableBufferLength - 8;
	offset = 0;
	status = STATUS_NOT_FOUND;
	while (offset + 4 <= tableLen)
	{
		UCHAR type = table[offset];
		UCHAR length = table[offset + 1];
		ULONG next;

		if (length < 4)
			break;
		if (type == 1 && length >= 0x19 && offset + 0x18 + 16 <= tableLen)
		{
			CdpUuidBytesToString(table + offset + 0x08, MbUuid);
			status = STATUS_SUCCESS;
			break;
		}
		next = offset + length;
		while (next + 1 < tableLen)
		{
			if (table[next] == 0 && table[next + 1] == 0)
			{
				next += 2;
				break;
			}
			++next;
		}
		if (next <= offset)
			break;
		offset = next;
	}
	cdpfree(info);
	return status;
}

static NTSTATUS CdpReadSystemDiskSerial(
	_Out_writes_(Cdp_LICENSE_DISK_SERIAL_CHARS) CHAR* DiskSerial)
{
	/* 默认取 \\Device\\Harddisk0\\DR0 的 StorageDeviceProperty.SerialNumber */
	UNICODE_STRING name;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	HANDLE handle = NULL;
	PDEVICE_OBJECT deviceObject = NULL;
	PFILE_OBJECT fileObject = NULL;
	NTSTATUS status;
	STORAGE_PROPERTY_QUERY query;
	UCHAR buffer[512];
	PSTORAGE_DEVICE_DESCRIPTOR desc;

	RtlZeroMemory(DiskSerial, Cdp_LICENSE_DISK_SERIAL_CHARS);
	RtlInitUnicodeString(&name, L"\\Device\\Harddisk0\\DR0");
	status = IoGetDeviceObjectPointer(&name, FILE_READ_ATTRIBUTES,
		&fileObject, &deviceObject);
	if (!NT_SUCCESS(status))
	{
		/* Fallback open. */
		InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
			NULL, NULL);
		status = ZwCreateFile(
			&handle,
			FILE_READ_ATTRIBUTES | SYNCHRONIZE,
			&oa,
			&iosb,
			NULL,
			FILE_ATTRIBUTE_NORMAL,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			FILE_OPEN,
			FILE_SYNCHRONOUS_IO_NONALERT,
			NULL,
			0);
		if (!NT_SUCCESS(status))
		{
			Cdp_LIC_FAIL("failed status=0x%08X", status);
			return status;
		}
	}

	RtlZeroMemory(&query, sizeof(query));
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;
	RtlZeroMemory(buffer, sizeof(buffer));

	if (deviceObject)
	{
		KEVENT event;
		PIRP irp;

		KeInitializeEvent(&event, NotificationEvent, FALSE);
		irp = IoBuildDeviceIoControlRequest(
			IOCTL_STORAGE_QUERY_PROPERTY,
			deviceObject,
			&query,
			sizeof(query),
			buffer,
			sizeof(buffer),
			FALSE,
			&event,
			&iosb);
		if (!irp)
		{
			ObDereferenceObject(fileObject);
			Cdp_LIC_FAIL("STATUS_INSUFFICIENT_RESOURCES: if (!irp)");
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		status = IoCallDriver(deviceObject, irp);
		if (status == STATUS_PENDING)
		{
			KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
			status = iosb.Status;
		}
		ObDereferenceObject(fileObject);
	}
	else
	{
		status = ZwDeviceIoControlFile(
			handle,
			NULL, NULL, NULL,
			&iosb,
			IOCTL_STORAGE_QUERY_PROPERTY,
			&query,
			sizeof(query),
			buffer,
			sizeof(buffer));
		ZwClose(handle);
	}

	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	desc = (PSTORAGE_DEVICE_DESCRIPTOR)buffer;
	if (desc->SerialNumberOffset != 0 &&
		desc->SerialNumberOffset < sizeof(buffer))
	{
		CHAR raw[Cdp_LICENSE_DISK_SERIAL_CHARS];
		RtlZeroMemory(raw, sizeof(raw));
		RtlStringCbCopyA(raw, sizeof(raw),
			(const CHAR*)buffer + desc->SerialNumberOffset);
		CdpLicenseNormalizeDiskSerial(raw, DiskSerial);
		return STATUS_SUCCESS;
	}
	Cdp_LIC_FAIL("SerialNumberOffset=%lu", desc->SerialNumberOffset);
	return STATUS_NOT_FOUND;
}

NTSTATUS CdpLicenseCollectHardwareId(
	_Out_writes_(Cdp_LICENSE_MB_UUID_CHARS) CHAR* MbUuid,
	_Out_writes_(Cdp_LICENSE_DISK_SERIAL_CHARS) CHAR* DiskSerial,
	_Out_writes_bytes_(Cdp_LICENSE_FP_BYTES) UCHAR* Fingerprint)
{
	/*
	 * 采集失败时用占位串仍计算指纹，避免整条路径崩溃；
	 * 绑机验签会因此失败，强制走正规硬件环境。
	 */
	CHAR mbRaw[Cdp_LICENSE_MB_UUID_CHARS];
	CHAR diskNorm[Cdp_LICENSE_DISK_SERIAL_CHARS];
	CHAR mbNorm[Cdp_LICENSE_MB_UUID_CHARS];
	UCHAR material[Cdp_LICENSE_MB_UUID_CHARS + Cdp_LICENSE_DISK_SERIAL_CHARS + 8];
	ULONG materialLength;
	NTSTATUS status;

	if (!MbUuid || !DiskSerial || !Fingerprint)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!MbUuid || !DiskSerial || !Fingerprint)");
		return STATUS_INVALID_PARAMETER;
	}
	RtlZeroMemory(mbRaw, sizeof(mbRaw));
	RtlZeroMemory(DiskSerial, Cdp_LICENSE_DISK_SERIAL_CHARS);
	status = CdpReadSmbiosUuid(mbRaw);
	if (!NT_SUCCESS(status))
		RtlStringCbCopyA(mbRaw, sizeof(mbRaw), "00000000-0000-0000-0000-000000000000");
	status = CdpReadSystemDiskSerial(DiskSerial);
	if (!NT_SUCCESS(status))
		RtlStringCbCopyA(DiskSerial, Cdp_LICENSE_DISK_SERIAL_CHARS, "UNKNOWN");

	status = CdpLicenseCodecBuildHardwareFingerprintMaterial(mbRaw, DiskSerial,
		mbNorm, diskNorm, material, sizeof(material), &materialLength);
	if (NT_SUCCESS(status))
	{
		RtlStringCbCopyA(MbUuid, Cdp_LICENSE_MB_UUID_CHARS, mbNorm);
		RtlStringCbCopyA(DiskSerial, Cdp_LICENSE_DISK_SERIAL_CHARS, diskNorm);
		status = CdpLicenseSha256(material, materialLength, Fingerprint);
	}
	RtlSecureZeroMemory(material, sizeof(material));
	return status;
}

#include "CdpLicenseSegEnd.h" /* 恢复默认 code/const 节 */

#endif /* CDP_LICENSE_OBFUSCATE */
#endif /* CDP_LICENSE */
