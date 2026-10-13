/*
 * 找回凭据策略单测。
 * 覆盖错设备、过期、旧代次、错误 nonce、重放，以及截断的凭据文件。
 * 不验 RSA-PSS；签名由驱动在内核里调用厂商公钥完成。
 */

#include "CdpPasswordRecoveryPolicy.h"

#include <stdio.h>
#include <string.h>

static int g_Failed;

/* 打印一条断言。 */
static void Expect(int Cond, const char* Msg)
{
	if (!Cond)
	{
		printf("FAIL: %s\n", Msg);
		++g_Failed;
	}
	else
	{
		printf("OK:   %s\n", Msg);
	}
}

/* 把定长字节写成小写十六进制。 */
static void FillHex(char* Out, unsigned char Value, int Bytes)
{
	int i;

	for (i = 0; i < Bytes; ++i)
		sprintf(Out + i * 2, "%02x", Value);
	Out[Bytes * 2] = 0;
}

/* 按凭据文件布局打包：长度 + JSON + 假签名。 */
static unsigned int PackBlob(
	unsigned char* Out,
	unsigned int OutCap,
	const char* Json,
	unsigned int SigLen)
{
	unsigned int jsonLen = (unsigned int)strlen(Json);
	unsigned int total = 8u + jsonLen + SigLen;
	unsigned int i;

	if (total > OutCap)
		return 0;
	Out[0] = (unsigned char)((jsonLen >> 24) & 0xFF);
	Out[1] = (unsigned char)((jsonLen >> 16) & 0xFF);
	Out[2] = (unsigned char)((jsonLen >> 8) & 0xFF);
	Out[3] = (unsigned char)(jsonLen & 0xFF);
	memcpy(Out + 4, Json, jsonLen);
	Out[4 + jsonLen] = (unsigned char)((SigLen >> 24) & 0xFF);
	Out[5 + jsonLen] = (unsigned char)((SigLen >> 16) & 0xFF);
	Out[6 + jsonLen] = (unsigned char)((SigLen >> 8) & 0xFF);
	Out[7 + jsonLen] = (unsigned char)(SigLen & 0xFF);
	for (i = 0; i < SigLen; ++i)
		Out[8 + jsonLen + i] = 0x5A;
	return total;
}

/* SHA-256("abc") 以及找回声明的拒绝路径。 */
int TestPasswordRecoveryPolicy(void)
{
	static const unsigned char kAbc[32] =
	{
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
	};
	unsigned char digest[32];
	unsigned char fp[32];
	unsigned char nonce[16];
	unsigned char seen[32];
	unsigned char blob[1536];
	char fpHex[65];
	char nonceHex[33];
	char json[768];
	CDP_RECOVERY_CLAIM claim;
	CDP_RECOVERY_EXPECT expect;
	const unsigned char* jsonPtr = 0;
	const unsigned char* sigPtr = 0;
	unsigned int jsonLen = 0;
	unsigned int sigLen = 0;
	unsigned int blobLen;
	int status;
	const char* jti = "11111111-2222-3333-4444-555555555555";

	printf("\n=== Password recovery policy ===\n");
	g_Failed = 0;
	CdpRecoverySha256("abc", 3, digest);
	Expect(memcmp(digest, kAbc, 32) == 0, "SHA-256 abc");

	memset(fp, 0xAB, sizeof(fp));
	memset(nonce, 0xCD, sizeof(nonce));
	FillHex(fpHex, 0xAB, 32);
	FillHex(nonceHex, 0xCD, 16);
	sprintf(json,
		"{\"auth_epoch\":7,\"credential_id\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
		"\"exp\":2000000000,\"fp\":\"%s\",\"iat\":1700000000,\"jti\":\"%s\","
		"\"request_nonce\":\"%s\",\"typ\":\"cdp-protect-reset-v1\",\"ver\":1}",
		fpHex, jti, nonceHex);

	blobLen = PackBlob(blob, sizeof(blob), json, 256);
	status = CdpRecoverySplitCredential(
		blob, blobLen, &jsonPtr, &jsonLen, &sigPtr, &sigLen);
	Expect(status == CDP_RECOVERY_OK && sigLen == 256, "split credential");
	status = CdpRecoveryParseClaim(jsonPtr, jsonLen, &claim);
	Expect(status == CDP_RECOVERY_OK, "parse claim");
	Expect(claim.AuthEpoch == 7, "auth epoch");
	Expect(claim.Ver == 1, "version");

	memset(&expect, 0, sizeof(expect));
	expect.Fingerprint = fp;
	expect.CredentialId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
	expect.AuthEpoch = 7;
	expect.Nonce = nonce;
	expect.NowUnix = 1800000000;
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_OK, "matching claim");

	fp[0] ^= 0x01;
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_FP, "wrong device fingerprint");
	fp[0] ^= 0x01;

	expect.NowUnix = 2000000000;
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_EXPIRED, "expired credential");
	expect.NowUnix = 1800000000;

	expect.NowUnix = 100;
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_BAD_TIME, "iat too far ahead");
	expect.NowUnix = 1800000000;

	expect.AuthEpoch = 6;
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_EPOCH, "old auth epoch");
	expect.AuthEpoch = 7;

	expect.CredentialId = "bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee";
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_CREDENTIAL, "wrong credential id");
	expect.CredentialId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";

	nonce[0] ^= 0x01;
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_NONCE, "wrong request nonce");
	nonce[0] ^= 0x01;

	CdpRecoverySha256(jti, (unsigned int)strlen(jti), seen);
	expect.SeenJtiHashes = seen;
	expect.SeenCount = 1;
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_REPLAY, "replayed jti");
	expect.SeenJtiHashes = 0;
	expect.SeenCount = 0;

	sprintf(json,
		"{\"auth_epoch\":7,\"credential_id\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
		"\"exp\":2000000000,\"fp\":\"%s\",\"iat\":1700000000,\"jti\":\"%s\","
		"\"request_nonce\":\"%s\",\"typ\":\"cdp-protect-recovery-v1\",\"ver\":1}",
		fpHex, jti, nonceHex);
	status = CdpRecoveryParseClaim((const unsigned char*)json, (unsigned int)strlen(json), &claim);
	Expect(status == CDP_RECOVERY_OK, "reparse after typ swap setup");
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_BAD_TYP, "wrong typ");

	sprintf(json,
		"{\"auth_epoch\":7,\"credential_id\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
		"\"exp\":2000000000,\"fp\":\"%s\",\"iat\":1700000000,\"jti\":\"%s\","
		"\"request_nonce\":\"%s\",\"typ\":\"cdp-protect-reset-v1\",\"ver\":2}",
		fpHex, jti, nonceHex);
	status = CdpRecoveryParseClaim((const unsigned char*)json, (unsigned int)strlen(json), &claim);
	Expect(status == CDP_RECOVERY_OK, "parse bad version");
	status = CdpRecoveryCheckClaim(&claim, &expect);
	Expect(status == CDP_RECOVERY_BAD_VER, "wrong ver");

	Expect(CdpRecoverySplitCredential(blob, 4, &jsonPtr, &jsonLen, &sigPtr, &sigLen) ==
		CDP_RECOVERY_BAD_FORMAT, "truncated blob");
	blobLen = PackBlob(blob, sizeof(blob), json, 32);
	Expect(CdpRecoverySplitCredential(blob, blobLen, &jsonPtr, &jsonLen, &sigPtr, &sigLen) ==
		CDP_RECOVERY_BAD_FORMAT, "short signature");

	return g_Failed;
}
