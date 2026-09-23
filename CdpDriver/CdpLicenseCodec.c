/*
 * 授权编解码（canonical JSON 组装 / 解析 / 有效性判定）。
 *
 * 本单元链入 .licprot，并被 O-16 的 .licprot/.licpr CRC32C 完整性校验覆盖。
 *
 * 原因：CdpLicenseCodecBuildCanonicalWithoutSignature 定义"被签名保护的到底是
 * 哪些字节"，CdpLicenseCodecCheckValidity 定义"证件是否有效"，而两者都在
 * CdpLicenseGate 的验签之前被调用。此前本文件虽带全套混淆 pass，却不进受保护
 * 节，攻击者可以只改这里的解析/长度判定（例如放宽字段校验或伪造 canonical
 * 视图）而不触碰任何受完整性保护或被控制流平坦化的代码。
 *
 * 注：本文件内只有字符串字面量，没有 const 全局，因此不会向 .licpr 引入新内容
 *（.licpr 应保持只装 Cdp_PRODUCT_MAGIC / ARX 常量 / 公钥分片）。
 */

#include "CdpLicenseCodec.h"

#include <ntstrsafe.h>

#ifdef CDP_LICENSE

#include "CdpLicenseSeg.h" /* 此后本文件代码进入 .licprot */

SIZE_T CdpLicenseCodecStrLen(const CHAR* String, SIZE_T Maximum)
{
    SIZE_T index = 0;

    if (!String)
        return 0;
    while (index < Maximum && String[index])
        ++index;
    return index;
}

VOID CdpLicenseCodecNormalizeMbUuid(const CHAR* Input, CHAR* Output)
{
    SIZE_T index;
    SIZE_T outputIndex = 0;

    RtlZeroMemory(Output, Cdp_LICENSE_MB_UUID_CHARS);
    if (!Input)
        return;
    for (index = 0; Input[index] &&
        outputIndex + 1 < Cdp_LICENSE_MB_UUID_CHARS; ++index)
    {
        CHAR character = Input[index];
        if (character == '{' || character == '}' || character == ' ' ||
            character == '\t')
            continue;
        if (character >= 'A' && character <= 'Z')
            character = (CHAR)(character - 'A' + 'a');
        Output[outputIndex++] = character;
    }
    Output[outputIndex] = 0;
}

VOID CdpLicenseCodecNormalizeDiskSerial(const CHAR* Input, CHAR* Output)
{
    SIZE_T index;
    SIZE_T outputIndex = 0;
    SIZE_T start = 0;
    SIZE_T end;

    RtlZeroMemory(Output, Cdp_LICENSE_DISK_SERIAL_CHARS);
    if (!Input)
        return;
    end = CdpLicenseCodecStrLen(Input, Cdp_LICENSE_DISK_SERIAL_CHARS - 1);
    while (start < end && (Input[start] == ' ' || Input[start] == '\t'))
        ++start;
    while (end > start && (Input[end - 1] == ' ' || Input[end - 1] == '\t'))
        --end;
    for (index = start; index < end &&
        outputIndex + 1 < Cdp_LICENSE_DISK_SERIAL_CHARS; ++index)
    {
        CHAR character = Input[index];
        if (character >= 'a' && character <= 'z')
            character = (CHAR)(character - 'a' + 'A');
        Output[outputIndex++] = character;
    }
    Output[outputIndex] = 0;
}

VOID CdpLicenseCodecUuidBytesToString(const UCHAR* Bytes, CHAR* Output)
{
    static const CHAR hex[] = "0123456789abcdef";
    ULONG index;
    ULONG outputIndex = 0;

    RtlZeroMemory(Output, Cdp_LICENSE_MB_UUID_CHARS);
    for (index = 0; index < 16; ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            Output[outputIndex++] = '-';
        Output[outputIndex++] = hex[(Bytes[index] >> 4) & 0xF];
        Output[outputIndex++] = hex[Bytes[index] & 0xF];
    }
    Output[outputIndex] = 0;
}

NTSTATUS CdpLicenseCodecBuildHardwareFingerprintMaterial(
    const CHAR* MbUuidInput, const CHAR* DiskSerialInput,
    CHAR* NormalizedMbUuid, CHAR* NormalizedDiskSerial,
    UCHAR* Material, ULONG MaterialCapacity, PULONG MaterialLength)
{
    ULONG mbLength;
    ULONG diskLength;
    ULONG total = 0;

    if (!NormalizedMbUuid || !NormalizedDiskSerial || !Material ||
        !MaterialLength)
    {
        return STATUS_INVALID_PARAMETER;
    }
    CdpLicenseCodecNormalizeMbUuid(MbUuidInput, NormalizedMbUuid);
    CdpLicenseCodecNormalizeDiskSerial(DiskSerialInput, NormalizedDiskSerial);
    mbLength = (ULONG)CdpLicenseCodecStrLen(NormalizedMbUuid,
        Cdp_LICENSE_MB_UUID_CHARS);
    diskLength = (ULONG)CdpLicenseCodecStrLen(NormalizedDiskSerial,
        Cdp_LICENSE_DISK_SERIAL_CHARS);
    if (mbLength > MaterialCapacity || MaterialCapacity - mbLength < 1 ||
        diskLength > MaterialCapacity - mbLength - 1 ||
        MaterialCapacity - mbLength - 1 - diskLength < 2)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(Material + total, NormalizedMbUuid, mbLength);
    total += mbLength;
    Material[total++] = 0;
    RtlCopyMemory(Material + total, NormalizedDiskSerial, diskLength);
    total += diskLength;
    Material[total++] = 0;
    Material[total++] = 0;
    *MaterialLength = total;
    return STATUS_SUCCESS;
}

BOOLEAN CdpLicenseCodecCStrEq(const CHAR* Left, const CHAR* Right)
{
    if (!Left || !Right)
        return FALSE;
    while (*Left && *Right)
    {
        if (*Left != *Right)
            return FALSE;
        ++Left;
        ++Right;
    }
    return *Left == *Right;
}

static int CdpLicenseCodecHexNibble(CHAR Character)
{
    if (Character >= '0' && Character <= '9')
        return Character - '0';
    if (Character >= 'a' && Character <= 'f')
        return Character - 'a' + 10;
    if (Character >= 'A' && Character <= 'F')
        return Character - 'A' + 10;
    return -1;
}

NTSTATUS CdpLicenseCodecHexDecode(const CHAR* Hex, ULONG HexLen,
    UCHAR* Output, ULONG OutputCapacity, PULONG OutputLength)
{
    ULONG index;

    if (!Hex || !Output || !OutputLength || (HexLen & 1) != 0 ||
        HexLen / 2 > OutputCapacity)
    {
        return STATUS_INVALID_PARAMETER;
    }
    for (index = 0; index < HexLen; index += 2)
    {
        int high = CdpLicenseCodecHexNibble(Hex[index]);
        int low = CdpLicenseCodecHexNibble(Hex[index + 1]);

        if (high < 0 || low < 0)
            return STATUS_INVALID_PARAMETER;
        Output[index / 2] = (UCHAR)((high << 4) | low);
    }
    *OutputLength = HexLen / 2;
    return STATUS_SUCCESS;
}

static const CHAR* CdpLicenseCodecStrChr(const CHAR* String, CHAR Character)
{
    if (!String)
        return NULL;
    while (*String)
    {
        if (*String == Character)
            return String;
        ++String;
    }
    return NULL;
}

NTSTATUS CdpLicenseCodecBase64Decode(const CHAR* Input, ULONG InputLength,
    UCHAR* Output, ULONG OutputCapacity, PULONG OutputLength)
{
    static const CHAR table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    ULONG index;
    ULONG written = 0;
    ULONG value = 0;
    LONG valueBits = -8;

    if (!Input || !Output || !OutputLength)
        return STATUS_INVALID_PARAMETER;
    *OutputLength = 0;
    for (index = 0; index < InputLength; ++index)
    {
        CHAR character = Input[index];
        const CHAR* position;
        ULONG digit;

        if (character == '=' || character == '\r' || character == '\n')
            continue;
        position = CdpLicenseCodecStrChr(table, character);
        if (!position)
            return STATUS_INVALID_PARAMETER;
        digit = (ULONG)(position - table);
        value = (value << 6) + digit;
        valueBits += 6;
        if (valueBits >= 0)
        {
            if (written >= OutputCapacity)
                return STATUS_BUFFER_TOO_SMALL;
            Output[written++] = (UCHAR)((value >> valueBits) & 0xFF);
            valueBits -= 8;
        }
    }
    *OutputLength = written;
    return STATUS_SUCCESS;
}

NTSTATUS CdpLicenseCodecBase64Encode(const UCHAR* Input, ULONG InputLength,
    CHAR* Output, ULONG OutputCapacity, PULONG OutputLength)
{
    static const CHAR table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    ULONG index;
    ULONG written = 0;

    if (!Input || !Output || !OutputLength)
        return STATUS_INVALID_PARAMETER;
    *OutputLength = 0;
    for (index = 0; index < InputLength; index += 3)
    {
        ULONG value = ((ULONG)Input[index]) << 16;
        ULONG remain = InputLength - index;

        if (remain > 1)
            value |= ((ULONG)Input[index + 1]) << 8;
        if (remain > 2)
            value |= (ULONG)Input[index + 2];
        if (written + 4 >= OutputCapacity)
            return STATUS_BUFFER_TOO_SMALL;
        Output[written++] = table[(value >> 18) & 63];
        Output[written++] = table[(value >> 12) & 63];
        Output[written++] = remain > 1 ? table[(value >> 6) & 63] : '=';
        Output[written++] = remain > 2 ? table[value & 63] : '=';
    }
    if (written >= OutputCapacity)
        return STATUS_BUFFER_TOO_SMALL;
    Output[written] = 0;
    *OutputLength = written;
    return STATUS_SUCCESS;
}

NTSTATUS CdpLicenseCodecSanitizePrintablePrefix(const CHAR* Input,
    CHAR* Output, ULONG OutputCapacity)
{
    ULONG index;

    if (!Input || !Output || OutputCapacity == 0)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Output, OutputCapacity);
    for (index = 0; index + 1 < OutputCapacity; ++index)
    {
        UCHAR character = (UCHAR)Input[index];

        if (character == 0)
            break;
        if (character < 0x20 || character > 0x7E)
            return STATUS_INVALID_PARAMETER;
        Output[index] = (CHAR)character;
    }
    return Output[0] ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

BOOLEAN CdpLicenseCodecFindJsonString(const CHAR* Json, ULONG JsonLength,
    const CHAR* Key, CHAR* Output, ULONG OutputCapacity)
{
    CHAR pattern[96];
    SIZE_T keyLength;
    ULONG index;

    RtlZeroMemory(Output, OutputCapacity);
    keyLength = CdpLicenseCodecStrLen(Key, 64);
    if (keyLength + 4 >= sizeof(pattern))
        return FALSE;
    pattern[0] = '"';
    RtlCopyMemory(pattern + 1, Key, keyLength);
    pattern[1 + keyLength] = '"';
    pattern[2 + keyLength] = ':';
    pattern[3 + keyLength] = '"';
    pattern[4 + keyLength] = 0;

    for (index = 0; index + (ULONG)(keyLength + 4) < JsonLength; ++index)
    {
        if (RtlCompareMemory(Json + index, pattern, keyLength + 4) ==
            keyLength + 4)
        {
            ULONG input = index + (ULONG)keyLength + 4;
            ULONG output = 0;

            while (input < JsonLength && Json[input] != '"' &&
                output + 1 < OutputCapacity)
            {
                if (Json[input] == '\\' && input + 1 < JsonLength)
                {
                    Output[output++] = Json[input + 1];
                    input += 2;
                    continue;
                }
                Output[output++] = Json[input++];
            }
            Output[output] = 0;
            return TRUE;
        }
    }
    return FALSE;
}

BOOLEAN CdpLicenseCodecFindJsonNumber(const CHAR* Json, ULONG JsonLength,
    const CHAR* Key, PLONGLONG Value)
{
    CHAR pattern[96];
    SIZE_T keyLength;
    ULONG index;

    *Value = 0;
    keyLength = CdpLicenseCodecStrLen(Key, 64);
    if (keyLength + 3 >= sizeof(pattern))
        return FALSE;
    pattern[0] = '"';
    RtlCopyMemory(pattern + 1, Key, keyLength);
    pattern[1 + keyLength] = '"';
    pattern[2 + keyLength] = ':';
    pattern[3 + keyLength] = 0;

    for (index = 0; index + (ULONG)(keyLength + 3) < JsonLength; ++index)
    {
        if (RtlCompareMemory(Json + index, pattern, keyLength + 3) ==
            keyLength + 3)
        {
            ULONG input = index + (ULONG)keyLength + 3;
            LONGLONG sign = 1;
            LONGLONG number = 0;

            while (input < JsonLength &&
                (Json[input] == ' ' || Json[input] == '\t'))
            {
                ++input;
            }
            if (input < JsonLength && Json[input] == '-')
            {
                sign = -1;
                ++input;
            }
            if (input >= JsonLength || Json[input] < '0' ||
                Json[input] > '9')
            {
                return FALSE;
            }
            while (input < JsonLength && Json[input] >= '0' &&
                Json[input] <= '9')
            {
                number = number * 10 + (Json[input] - '0');
                ++input;
            }
            *Value = number * sign;
            return TRUE;
        }
    }
    return FALSE;
}

NTSTATUS CdpLicenseCodecParseIso8601ToFileTime(const CHAR* Iso,
    PUINT64 Output100ns)
{
    TIME_FIELDS fields;
    LARGE_INTEGER fileTime;
    ULONG year = 0;
    ULONG month = 0;
    ULONG day = 0;
    ULONG hour = 0;
    ULONG minute = 0;
    ULONG second = 0;
    const CHAR* cursor = Iso;

    if (!Iso || !Output100ns)
        return STATUS_INVALID_PARAMETER;
    *Output100ns = 0;
    for (year = 0; *cursor >= '0' && *cursor <= '9'; ++cursor)
        year = year * 10 + (*cursor - '0');
    if (*cursor++ != '-')
        return STATUS_INVALID_PARAMETER;
    for (month = 0; *cursor >= '0' && *cursor <= '9'; ++cursor)
        month = month * 10 + (*cursor - '0');
    if (*cursor++ != '-')
        return STATUS_INVALID_PARAMETER;
    for (day = 0; *cursor >= '0' && *cursor <= '9'; ++cursor)
        day = day * 10 + (*cursor - '0');
    if (*cursor != 'T' && *cursor != 't')
        return STATUS_INVALID_PARAMETER;
    ++cursor;
    for (hour = 0; *cursor >= '0' && *cursor <= '9'; ++cursor)
        hour = hour * 10 + (*cursor - '0');
    if (*cursor++ != ':')
        return STATUS_INVALID_PARAMETER;
    for (minute = 0; *cursor >= '0' && *cursor <= '9'; ++cursor)
        minute = minute * 10 + (*cursor - '0');
    if (*cursor++ != ':')
        return STATUS_INVALID_PARAMETER;
    for (second = 0; *cursor >= '0' && *cursor <= '9'; ++cursor)
        second = second * 10 + (*cursor - '0');

    RtlZeroMemory(&fields, sizeof(fields));
    fields.Year = (CSHORT)year;
    fields.Month = (CSHORT)month;
    fields.Day = (CSHORT)day;
    fields.Hour = (CSHORT)hour;
    fields.Minute = (CSHORT)minute;
    fields.Second = (CSHORT)second;
    if (!RtlTimeFieldsToTime(&fields, &fileTime))
        return STATUS_INVALID_PARAMETER;
    *Output100ns = (UINT64)fileTime.QuadPart;
    return STATUS_SUCCESS;
}

NTSTATUS CdpLicenseCodecBuildCanonicalWithoutSignature(const CHAR* Json,
    ULONG JsonLength, UCHAR* Output, ULONG OutputCapacity,
    PULONG OutputLength)
{
    CHAR licenseId[Cdp_LICENSE_ID_CHARS];
    CHAR deviceId[Cdp_LICENSE_ID_CHARS];
    CHAR deviceHash[80];
    CHAR motherboardUuid[Cdp_LICENSE_MB_UUID_CHARS];
    CHAR diskSerial[Cdp_LICENSE_DISK_SERIAL_CHARS];
    CHAR issueTime[64];
    CHAR expiryTime[64];
    CHAR issuedAt[64];
    CHAR signingKeyId[Cdp_LICENSE_ID_CHARS];
    CHAR applySessionId[Cdp_LICENSE_ID_CHARS];
    CHAR kind[16];
    LONGLONG mode = 0;
    LONGLONG initialCount = 0;
    LONGLONG formatVersion = 0;
    CHAR body[1600];
    ULONG bodyLength;
    NTSTATUS status;

    if (!Json || !Output || !OutputLength)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(kind, sizeof(kind));
    if (!CdpLicenseCodecFindJsonString(Json, JsonLength, "license_id", licenseId, sizeof(licenseId)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "device_id", deviceId, sizeof(deviceId)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "device_id_hash", deviceHash, sizeof(deviceHash)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "mb_uuid", motherboardUuid, sizeof(motherboardUuid)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "disk_serial", diskSerial, sizeof(diskSerial)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "t0_issue", issueTime, sizeof(issueTime)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "t_exp", expiryTime, sizeof(expiryTime)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "issued_at_server", issuedAt, sizeof(issuedAt)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "signing_key_id", signingKeyId, sizeof(signingKeyId)) ||
        !CdpLicenseCodecFindJsonString(Json, JsonLength, "apply_session_id", applySessionId, sizeof(applySessionId)) ||
        !CdpLicenseCodecFindJsonNumber(Json, JsonLength, "mode", &mode) ||
        !CdpLicenseCodecFindJsonNumber(Json, JsonLength, "c0_initial", &initialCount) ||
        !CdpLicenseCodecFindJsonNumber(Json, JsonLength, "format_version", &formatVersion))
    {
        return STATUS_CDP_LICENSE_INVALID;
    }
    if (formatVersion < (LONGLONG)Cdp_LICENSE_FORMAT_VERSION_MIN ||
        formatVersion > (LONGLONG)Cdp_LICENSE_FORMAT_VERSION_MAX)
    {
        return STATUS_CDP_LICENSE_INVALID;
    }
    if (formatVersion >= 2)
    {
        if (!CdpLicenseCodecFindJsonString(Json, JsonLength, "kind", kind, sizeof(kind)) ||
            (!CdpLicenseCodecCStrEq(kind, "paid") &&
             !CdpLicenseCodecCStrEq(kind, "trial")))
        {
            return STATUS_CDP_LICENSE_INVALID;
        }
        status = RtlStringCbPrintfA(body, sizeof(body),
            "{\"apply_session_id\":\"%s\",\"c0_initial\":%I64d,\"device_id\":\"%s\","
            "\"device_id_hash\":\"%s\",\"disk_serial\":\"%s\",\"format_version\":%I64d,"
            "\"issued_at_server\":\"%s\",\"kind\":\"%s\",\"license_id\":\"%s\","
            "\"mb_uuid\":\"%s\",\"mode\":%I64d,\"signing_key_id\":\"%s\","
            "\"t0_issue\":\"%s\",\"t_exp\":\"%s\"}",
            applySessionId, initialCount, deviceId, deviceHash, diskSerial,
            formatVersion, issuedAt, kind, licenseId, motherboardUuid, mode,
            signingKeyId, issueTime, expiryTime);
    }
    else
    {
        status = RtlStringCbPrintfA(body, sizeof(body),
            "{\"apply_session_id\":\"%s\",\"c0_initial\":%I64d,\"device_id\":\"%s\","
            "\"device_id_hash\":\"%s\",\"disk_serial\":\"%s\",\"format_version\":%I64d,"
            "\"issued_at_server\":\"%s\",\"license_id\":\"%s\",\"mb_uuid\":\"%s\","
            "\"mode\":%I64d,\"signing_key_id\":\"%s\",\"t0_issue\":\"%s\",\"t_exp\":\"%s\"}",
            applySessionId, initialCount, deviceId, deviceHash, diskSerial,
            formatVersion, issuedAt, licenseId, motherboardUuid, mode,
            signingKeyId, issueTime, expiryTime);
    }
    if (!NT_SUCCESS(status))
        return status;
    bodyLength = (ULONG)CdpLicenseCodecStrLen(body, sizeof(body));
    if (OutputCapacity < 4 + bodyLength)
        return STATUS_BUFFER_TOO_SMALL;
    Output[0] = (UCHAR)((bodyLength >> 24) & 0xFF);
    Output[1] = (UCHAR)((bodyLength >> 16) & 0xFF);
    Output[2] = (UCHAR)((bodyLength >> 8) & 0xFF);
    Output[3] = (UCHAR)(bodyLength & 0xFF);
    RtlCopyMemory(Output + 4, body, bodyLength);
    *OutputLength = 4 + bodyLength;
    return STATUS_SUCCESS;
}

NTSTATUS CdpLicenseCodecCheckValidity(BOOLEAN HasLicense, ULONG Mode,
    UINT64 LocalTime100ns, ULONG RemainingCount, UINT64 Now100ns,
    UINT64 ExpiryTime100ns)
{
    if (!HasLicense)
        return STATUS_CDP_LICENSE_REQUIRED;
    if (!(LocalTime100ns < Now100ns && Now100ns < ExpiryTime100ns))
        return STATUS_CDP_LICENSE_EXPIRED;
    if ((Mode == Cdp_LICENSE_MODE_COUNTER ||
         Mode == Cdp_LICENSE_MODE_HYBRID) && RemainingCount == 0)
    {
        return STATUS_CDP_LICENSE_EXHAUSTED;
    }
    return STATUS_SUCCESS;
}

#include "CdpLicenseSegEnd.h" /* 恢复默认 code/const 节 */

#endif
