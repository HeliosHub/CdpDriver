#include <Windows.h>
#include <stdio.h>
#include <wchar.h>
#include <errno.h>
#include "..\CdpDriver\CdpIoctl.h"

/* Keep this definition local so the tool is compatible with the driver's
 * current public ABI even when CDP_LICENSE is not defined for this project. */
#define LICENSE_APPLY_IOCTL CTL_CODE(Cdp_IOCTL_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define LICENSE_APPLY_PREFIX_MAX 256u
#define LICENSE_APPLY_PAYLOAD_MAX 4096u
#define LICENSE_APPLY_CIPHERTEXT_MAX 3072u
#define LICENSE_APPLY_MAX_DAYS 36500u

#pragma pack(push, 8)
typedef struct _LICENSE_APPLY_REQUEST {
	ULONG DesiredDurationSec;
	ULONG DesiredCredits;
	ULONG Mode;
	ULONG Kind;
	CHAR QrPrefix[LICENSE_APPLY_PREFIX_MAX];
} LICENSE_APPLY_REQUEST;

typedef struct _LICENSE_APPLY_REPLY {
	ULONG CiphertextLength;
	ULONG Reserved;
	UCHAR DeviceFingerprint[32];
	UCHAR Ciphertext[LICENSE_APPLY_CIPHERTEXT_MAX];
	CHAR QrPayload[LICENSE_APPLY_PAYLOAD_MAX];
} LICENSE_APPLY_REPLY;
#pragma pack(pop)

static void PrintUsage(void)
{
	fwprintf(stderr, L"Usage: CdpLicenseApply.exe [days]\n"
		L"  days: 1-%u, defaults to 30.\n", LICENSE_APPLY_MAX_DAYS);
}

static BOOL ParseDays(const wchar_t* text, ULONG* days)
{
	wchar_t* end = NULL;
	unsigned long value;

	if (!text || !text[0] || !days)
		return FALSE;
	errno = 0;
	value = wcstoul(text, &end, 10);
	if (errno == ERANGE || end == text || *end != L'\0' || value == 0 || value > LICENSE_APPLY_MAX_DAYS)
		return FALSE;
	*days = (ULONG)value;
	return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
	ULONG days = 30;
	DWORD bytesReturned = 0;
	HANDLE device;
	LICENSE_APPLY_REQUEST request = { 0 };
	LICENSE_APPLY_REPLY reply = { 0 };
	HANDLE output;
	DWORD bytesWritten = 0;
	size_t payloadLength;

	if (argc == 2 && (wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"--help") == 0))
	{
		PrintUsage();
		return 0;
	}
	if (argc > 2 || (argc == 2 && !ParseDays(argv[1], &days)))
	{
		PrintUsage();
		return 1;
	}

	device = CreateFileW(Cdp_CONTROL_SYSTEM_LINK_NAME, GENERIC_READ | GENERIC_WRITE,
		0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (device == INVALID_HANDLE_VALUE)
	{
		fwprintf(stderr, L"Cannot open the Cdp control device (error %lu).\n", GetLastError());
		return 2;
	}

	request.Mode = 1; /* Time-based paid license, equivalent to K -> A -> 1. */
	request.Kind = 0; /* Paid application. */
	request.DesiredDurationSec = days * 86400UL;
	strcpy_s(request.QrPrefix, "http://127.0.0.1:8080/v1/apply#c=");

	if (!DeviceIoControl(device, LICENSE_APPLY_IOCTL, &request, sizeof(request),
		&reply, sizeof(reply), &bytesReturned, NULL))
	{
		fwprintf(stderr, L"Failed to generate the license application code (error %lu).\n", GetLastError());
		CloseHandle(device);
		return 3;
	}
	CloseHandle(device);

	payloadLength = strnlen_s(reply.QrPayload, sizeof(reply.QrPayload));
	if (payloadLength == 0 || payloadLength == sizeof(reply.QrPayload))
	{
		fwprintf(stderr, L"The driver returned an invalid license application code.\n");
		return 4;
	}

	output = CreateFileW(L"CdpLicenseApply.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (output == INVALID_HANDLE_VALUE ||
		!WriteFile(output, reply.QrPayload, (DWORD)payloadLength, &bytesWritten, NULL) ||
		bytesWritten != payloadLength ||
		!WriteFile(output, "\r\n", 2, &bytesWritten, NULL) || bytesWritten != 2)
	{
		DWORD error = GetLastError();
		if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
		fwprintf(stderr, L"Failed to save CdpLicenseApply.txt (error %lu).\n", error);
		return 5;
	}
	CloseHandle(output);

	printf("Generated %lu-day license application code. Saved to CdpLicenseApply.txt.\n", days);
	return 0;
}
