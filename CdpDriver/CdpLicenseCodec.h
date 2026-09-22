#pragma once

#include "CdpLicenseDefs.h"

#ifdef CDP_LICENSE

SIZE_T CdpLicenseCodecStrLen(const CHAR* String, SIZE_T Maximum);
BOOLEAN CdpLicenseCodecCStrEq(const CHAR* Left, const CHAR* Right);
NTSTATUS CdpLicenseCodecHexDecode(const CHAR* Hex, ULONG HexLen,
    UCHAR* Output, ULONG OutputCapacity, PULONG OutputLength);
NTSTATUS CdpLicenseCodecBase64Decode(const CHAR* Input, ULONG InputLength,
    UCHAR* Output, ULONG OutputCapacity, PULONG OutputLength);
NTSTATUS CdpLicenseCodecBase64Encode(const UCHAR* Input, ULONG InputLength,
    CHAR* Output, ULONG OutputCapacity, PULONG OutputLength);
NTSTATUS CdpLicenseCodecSanitizePrintablePrefix(const CHAR* Input,
    CHAR* Output, ULONG OutputCapacity);
BOOLEAN CdpLicenseCodecFindJsonString(const CHAR* Json, ULONG JsonLength,
    const CHAR* Key, CHAR* Output, ULONG OutputCapacity);
BOOLEAN CdpLicenseCodecFindJsonNumber(const CHAR* Json, ULONG JsonLength,
    const CHAR* Key, PLONGLONG Value);
NTSTATUS CdpLicenseCodecParseIso8601ToFileTime(const CHAR* Iso,
    PUINT64 Output100ns);
NTSTATUS CdpLicenseCodecBuildCanonicalWithoutSignature(const CHAR* Json,
    ULONG JsonLength, UCHAR* Output, ULONG OutputCapacity,
    PULONG OutputLength);
NTSTATUS CdpLicenseCodecCheckValidity(BOOLEAN HasLicense, ULONG Mode,
    UINT64 LocalTime100ns, ULONG RemainingCount, UINT64 Now100ns,
    UINT64 ExpiryTime100ns);

#endif
