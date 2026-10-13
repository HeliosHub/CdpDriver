/*
 * 找回凭据的策略检查。
 * 只解析长度前缀凭据文件里的 JSON，并判断它是否还能用于这一台机器、这一份信封。
 * 厂商签名由驱动在调用这里之前完成。本文件不链接 CNG，用户态测试可以直接编译。
 */

#include "CdpPasswordRecoveryPolicy.h"

#define CDP_RECOVERY_JSON_MAX 2048u
#define CDP_RECOVERY_SIG_LEN 256u

/* SHA-256 压缩轮常量。 */
static const unsigned int g_CdpRecoveryShaK[64] =
{
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
	0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
	0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
	0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
	0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
	0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

typedef struct _CDP_RECOVERY_SHA256
{
	unsigned long long BitLength;
	unsigned int State[8];
	unsigned char Block[64];
	unsigned int BlockUsed;
} CDP_RECOVERY_SHA256;

/* 循环右移，SHA-256 的大端字操作。 */
static unsigned int CdpRecoveryRotr(unsigned int Value, unsigned int Bits)
{
	return (Value >> Bits) | (Value << (32u - Bits));
}

/* 压缩一个 64 字节分组。 */
static void CdpRecoveryShaTransform(
	CDP_RECOVERY_SHA256* Ctx,
	const unsigned char Block[64])
{
	unsigned int words[64];
	unsigned int a, b, c, d, e, f, g, h;
	unsigned int i;

	for (i = 0; i < 16; ++i)
	{
		words[i] =
			((unsigned int)Block[i * 4] << 24) |
			((unsigned int)Block[i * 4 + 1] << 16) |
			((unsigned int)Block[i * 4 + 2] << 8) |
			(unsigned int)Block[i * 4 + 3];
	}
	for (; i < 64; ++i)
	{
		unsigned int s0 = CdpRecoveryRotr(words[i - 15], 7) ^
			CdpRecoveryRotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
		unsigned int s1 = CdpRecoveryRotr(words[i - 2], 17) ^
			CdpRecoveryRotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
		words[i] = words[i - 16] + s0 + words[i - 7] + s1;
	}

	a = Ctx->State[0];
	b = Ctx->State[1];
	c = Ctx->State[2];
	d = Ctx->State[3];
	e = Ctx->State[4];
	f = Ctx->State[5];
	g = Ctx->State[6];
	h = Ctx->State[7];
	for (i = 0; i < 64; ++i)
	{
		unsigned int s1 = CdpRecoveryRotr(e, 6) ^
			CdpRecoveryRotr(e, 11) ^ CdpRecoveryRotr(e, 25);
		unsigned int ch = (e & f) ^ ((~e) & g);
		unsigned int t1 = h + s1 + ch + g_CdpRecoveryShaK[i] + words[i];
		unsigned int s0 = CdpRecoveryRotr(a, 2) ^
			CdpRecoveryRotr(a, 13) ^ CdpRecoveryRotr(a, 22);
		unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
		unsigned int t2 = s0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	Ctx->State[0] += a;
	Ctx->State[1] += b;
	Ctx->State[2] += c;
	Ctx->State[3] += d;
	Ctx->State[4] += e;
	Ctx->State[5] += f;
	Ctx->State[6] += g;
	Ctx->State[7] += h;
}

/* 初始化 SHA-256 链接变量。 */
static void CdpRecoveryShaInit(CDP_RECOVERY_SHA256* Ctx)
{
	unsigned int i;

	Ctx->BitLength = 0;
	Ctx->BlockUsed = 0;
	for (i = 0; i < 64; ++i)
		Ctx->Block[i] = 0;
	Ctx->State[0] = 0x6a09e667u;
	Ctx->State[1] = 0xbb67ae85u;
	Ctx->State[2] = 0x3c6ef372u;
	Ctx->State[3] = 0xa54ff53au;
	Ctx->State[4] = 0x510e527fu;
	Ctx->State[5] = 0x9b05688cu;
	Ctx->State[6] = 0x1f83d9abu;
	Ctx->State[7] = 0x5be0cd19u;
}

/* 追加消息字节。满 64 字节就压缩，长度在收尾时再补。 */
static void CdpRecoveryShaUpdate(
	CDP_RECOVERY_SHA256* Ctx,
	const unsigned char* Data,
	unsigned int Length)
{
	unsigned int i;

	for (i = 0; i < Length; ++i)
	{
		Ctx->Block[Ctx->BlockUsed++] = Data[i];
		if (Ctx->BlockUsed == 64)
		{
			CdpRecoveryShaTransform(Ctx, Ctx->Block);
			Ctx->BitLength += 512;
			Ctx->BlockUsed = 0;
		}
	}
}

/* 填充并写出 32 字节摘要。 */
static void CdpRecoveryShaFinal(
	CDP_RECOVERY_SHA256* Ctx,
	unsigned char Out[32])
{
	unsigned int i;
	unsigned long long bits;

	bits = Ctx->BitLength + (unsigned long long)Ctx->BlockUsed * 8ull;
	Ctx->Block[Ctx->BlockUsed++] = 0x80;
	if (Ctx->BlockUsed > 56)
	{
		while (Ctx->BlockUsed < 64)
			Ctx->Block[Ctx->BlockUsed++] = 0;
		CdpRecoveryShaTransform(Ctx, Ctx->Block);
		Ctx->BlockUsed = 0;
	}
	while (Ctx->BlockUsed < 56)
		Ctx->Block[Ctx->BlockUsed++] = 0;
	for (i = 0; i < 8; ++i)
		Ctx->Block[63 - i] = (unsigned char)((bits >> (8 * i)) & 0xFFu);
	CdpRecoveryShaTransform(Ctx, Ctx->Block);
	for (i = 0; i < 8; ++i)
	{
		Out[i * 4] = (unsigned char)((Ctx->State[i] >> 24) & 0xFFu);
		Out[i * 4 + 1] = (unsigned char)((Ctx->State[i] >> 16) & 0xFFu);
		Out[i * 4 + 2] = (unsigned char)((Ctx->State[i] >> 8) & 0xFFu);
		Out[i * 4 + 3] = (unsigned char)(Ctx->State[i] & 0xFFu);
	}
}

/* 计算整段输入的 SHA-256。 */
void CdpRecoverySha256(
	const void* Data,
	unsigned int Length,
	unsigned char Out[32])
{
	CDP_RECOVERY_SHA256 ctx;

	CdpRecoveryShaInit(&ctx);
	if (Data && Length)
		CdpRecoveryShaUpdate(&ctx, (const unsigned char*)Data, Length);
	CdpRecoveryShaFinal(&ctx, Out);
}

/* 读取大端 32 位长度。 */
static unsigned int CdpRecoveryReadBe32(const unsigned char* Bytes)
{
	return ((unsigned int)Bytes[0] << 24) |
		((unsigned int)Bytes[1] << 16) |
		((unsigned int)Bytes[2] << 8) |
		(unsigned int)Bytes[3];
}

/* 两个等长缓冲区逐字节异或，比较过程不因首个差异提前结束。 */
static int CdpRecoveryConstEqual(
	const unsigned char* Left,
	const unsigned char* Right,
	unsigned int Length)
{
	unsigned int i;
	unsigned char diff = 0;

	if (!Left || !Right)
		return 0;
	for (i = 0; i < Length; ++i)
		diff |= (unsigned char)(Left[i] ^ Right[i]);
	return diff == 0;
}

/* 固定上限内的字符串长度。没有结尾 0 时返回 Cap。 */
static unsigned int CdpRecoveryTextLen(const char* Text, unsigned int Cap)
{
	unsigned int i = 0;

	if (!Text)
		return 0;
	while (i < Cap && Text[i])
		++i;
	return i;
}

/* 在长度受限的 JSON 里查找 "key":，返回值的起始位置。 */
static const unsigned char* CdpRecoveryFindKey(
	const unsigned char* Json,
	unsigned int JsonLength,
	const char* Key,
	unsigned int KeyLength)
{
	unsigned int i;

	if (!Json || !Key || KeyLength == 0 || JsonLength < KeyLength + 3)
		return 0;
	for (i = 0; i + KeyLength + 3 <= JsonLength; ++i)
	{
		unsigned int k;
		int match;

		if (Json[i] != '"' || Json[i + 1 + KeyLength] != '"' ||
			Json[i + 2 + KeyLength] != ':')
		{
			continue;
		}
		match = 1;
		for (k = 0; k < KeyLength; ++k)
		{
			if (Json[i + 1 + k] != (unsigned char)Key[k])
			{
				match = 0;
				break;
			}
		}
		if (match)
			return Json + i + KeyLength + 3;
	}
	return 0;
}

/* 读取 JSON 字符串。遇到反斜杠直接拒绝，避免自己实现转义。 */
static int CdpRecoveryReadString(
	const unsigned char* Json,
	unsigned int JsonLength,
	const unsigned char* At,
	char* Out,
	unsigned int OutCap)
{
	unsigned int i;
	unsigned int n = 0;

	if (!At || At < Json || At >= Json + JsonLength || *At != '"')
		return CDP_RECOVERY_BAD_FORMAT;
	++At;
	i = (unsigned int)(At - Json);
	while (i < JsonLength && Json[i] != '"')
	{
		if (Json[i] == '\\' || n + 1 >= OutCap)
			return CDP_RECOVERY_BAD_FORMAT;
		Out[n++] = (char)Json[i];
		++i;
	}
	if (i >= JsonLength || Json[i] != '"')
		return CDP_RECOVERY_BAD_FORMAT;
	Out[n] = 0;
	return CDP_RECOVERY_OK;
}

/* 读取非负十进制整数。 */
static int CdpRecoveryReadUint(
	const unsigned char* Json,
	unsigned int JsonLength,
	const unsigned char* At,
	unsigned long long* Value)
{
	unsigned int i;
	unsigned long long n = 0;
	int digits = 0;

	if (!At || At < Json || At >= Json + JsonLength)
		return CDP_RECOVERY_BAD_FORMAT;
	i = (unsigned int)(At - Json);
	while (i < JsonLength && Json[i] >= '0' && Json[i] <= '9')
	{
		n = n * 10ull + (unsigned long long)(Json[i] - '0');
		++digits;
		++i;
		if (digits > 20)
			return CDP_RECOVERY_BAD_FORMAT;
	}
	if (digits == 0)
		return CDP_RECOVERY_BAD_FORMAT;
	if (i < JsonLength && Json[i] != ',' && Json[i] != '}')
		return CDP_RECOVERY_BAD_FORMAT;
	*Value = n;
	return CDP_RECOVERY_OK;
}

/* 把偶数位的小写或大写十六进制解码成定长字节。 */
static int CdpRecoveryDecodeHex(
	const char* Text,
	unsigned int TextLength,
	unsigned char* Out,
	unsigned int OutLength)
{
	unsigned int i;

	if (!Text || TextLength != OutLength * 2)
		return CDP_RECOVERY_BAD_FORMAT;
	for (i = 0; i < OutLength; ++i)
	{
		unsigned int nibble;
		unsigned int value = 0;
		unsigned int shift;

		for (shift = 0; shift < 2; ++shift)
		{
			char c = Text[i * 2 + shift];
			if (c >= '0' && c <= '9')
				nibble = (unsigned int)(c - '0');
			else if (c >= 'a' && c <= 'f')
				nibble = (unsigned int)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				nibble = (unsigned int)(c - 'A' + 10);
			else
				return CDP_RECOVERY_BAD_FORMAT;
			value = (value << 4) | nibble;
		}
		Out[i] = (unsigned char)value;
	}
	return CDP_RECOVERY_OK;
}

/* 切出 JSON 与签名。签名必须是 RSA-2048 的 256 字节，且文件没有尾随字节。 */
int CdpRecoverySplitCredential(
	const unsigned char* Blob,
	unsigned int BlobLength,
	const unsigned char** Json,
	unsigned int* JsonLength,
	const unsigned char** Signature,
	unsigned int* SignatureLength)
{
	unsigned int jsonLen;
	unsigned int sigLen;

	if (!Blob || !Json || !JsonLength || !Signature || !SignatureLength)
		return CDP_RECOVERY_BAD_FORMAT;
	*Json = 0;
	*JsonLength = 0;
	*Signature = 0;
	*SignatureLength = 0;
	if (BlobLength < 8)
		return CDP_RECOVERY_BAD_FORMAT;
	jsonLen = CdpRecoveryReadBe32(Blob);
	if (jsonLen == 0 || jsonLen > CDP_RECOVERY_JSON_MAX ||
		BlobLength < 8u + jsonLen)
	{
		return CDP_RECOVERY_BAD_FORMAT;
	}
	sigLen = CdpRecoveryReadBe32(Blob + 4 + jsonLen);
	if (sigLen != CDP_RECOVERY_SIG_LEN ||
		BlobLength != 8u + jsonLen + sigLen)
	{
		return CDP_RECOVERY_BAD_FORMAT;
	}
	*Json = Blob + 4;
	*JsonLength = jsonLen;
	*Signature = Blob + 8 + jsonLen;
	*SignatureLength = sigLen;
	return CDP_RECOVERY_OK;
}

/* 取出重设凭据声明，并把 jti 文本收成 SHA-256。 */
int CdpRecoveryParseClaim(
	const unsigned char* Json,
	unsigned int JsonLength,
	CDP_RECOVERY_CLAIM* Claim)
{
	const unsigned char* at;
	unsigned long long number = 0;
	char fp[80];
	char nonce[40];
	char jti[80];
	int status;

	if (!Json || !Claim || JsonLength == 0 || JsonLength > CDP_RECOVERY_JSON_MAX)
		return CDP_RECOVERY_BAD_FORMAT;
	for (number = 0; number < sizeof(*Claim); ++number)
		((unsigned char*)Claim)[number] = 0;

	at = CdpRecoveryFindKey(Json, JsonLength, "typ", 3);
	status = CdpRecoveryReadString(Json, JsonLength, at, Claim->Typ, sizeof(Claim->Typ));
	if (status != CDP_RECOVERY_OK)
		return status;

	at = CdpRecoveryFindKey(Json, JsonLength, "ver", 3);
	status = CdpRecoveryReadUint(Json, JsonLength, at, &number);
	if (status != CDP_RECOVERY_OK)
		return status;
	Claim->Ver = (unsigned int)number;

	at = CdpRecoveryFindKey(Json, JsonLength, "iat", 3);
	status = CdpRecoveryReadUint(Json, JsonLength, at, &number);
	if (status != CDP_RECOVERY_OK)
		return status;
	Claim->Iat = (long long)number;

	at = CdpRecoveryFindKey(Json, JsonLength, "exp", 3);
	status = CdpRecoveryReadUint(Json, JsonLength, at, &number);
	if (status != CDP_RECOVERY_OK)
		return status;
	Claim->Exp = (long long)number;

	at = CdpRecoveryFindKey(Json, JsonLength, "fp", 2);
	status = CdpRecoveryReadString(Json, JsonLength, at, fp, sizeof(fp));
	if (status != CDP_RECOVERY_OK)
		return status;
	status = CdpRecoveryDecodeHex(
		fp, CdpRecoveryTextLen(fp, sizeof(fp)), Claim->Fp, CDP_RECOVERY_FP_LEN);
	if (status != CDP_RECOVERY_OK)
		return status;

	at = CdpRecoveryFindKey(Json, JsonLength, "credential_id", 13);
	status = CdpRecoveryReadString(
		Json, JsonLength, at, Claim->CredentialId, sizeof(Claim->CredentialId));
	if (status != CDP_RECOVERY_OK)
		return status;

	at = CdpRecoveryFindKey(Json, JsonLength, "auth_epoch", 10);
	status = CdpRecoveryReadUint(Json, JsonLength, at, &Claim->AuthEpoch);
	if (status != CDP_RECOVERY_OK)
		return status;

	at = CdpRecoveryFindKey(Json, JsonLength, "request_nonce", 13);
	status = CdpRecoveryReadString(Json, JsonLength, at, nonce, sizeof(nonce));
	if (status != CDP_RECOVERY_OK)
		return status;
	status = CdpRecoveryDecodeHex(
		nonce,
		CdpRecoveryTextLen(nonce, sizeof(nonce)),
		Claim->Nonce,
		CDP_RECOVERY_NONCE_LEN);
	if (status != CDP_RECOVERY_OK)
		return status;

	at = CdpRecoveryFindKey(Json, JsonLength, "jti", 3);
	status = CdpRecoveryReadString(Json, JsonLength, at, jti, sizeof(jti));
	if (status != CDP_RECOVERY_OK)
		return status;
	if (CdpRecoveryTextLen(jti, sizeof(jti)) == 0)
		return CDP_RECOVERY_BAD_FORMAT;
	CdpRecoverySha256(jti, CdpRecoveryTextLen(jti, sizeof(jti)), Claim->JtiHash);
	return CDP_RECOVERY_OK;
}

/* 按固定顺序做策略判断。调用方已经验过签名。 */
int CdpRecoveryCheckClaim(
	const CDP_RECOVERY_CLAIM* Claim,
	const CDP_RECOVERY_EXPECT* Expect)
{
	static const char kTyp[] = "cdp-protect-reset-v1";
	unsigned int i;
	unsigned int idLen;
	unsigned int expectLen;
	unsigned char idDiff;

	if (!Claim || !Expect || !Expect->Fingerprint || !Expect->CredentialId ||
		!Expect->Nonce)
	{
		return CDP_RECOVERY_BAD_FORMAT;
	}
	for (i = 0; kTyp[i]; ++i)
	{
		if (Claim->Typ[i] != kTyp[i])
			return CDP_RECOVERY_BAD_TYP;
	}
	if (Claim->Typ[i] != 0)
		return CDP_RECOVERY_BAD_TYP;
	if (Claim->Ver != 1)
		return CDP_RECOVERY_BAD_VER;
	if (Claim->Exp <= Claim->Iat)
		return CDP_RECOVERY_BAD_TIME;
	if (Expect->NowUnix + CDP_RECOVERY_IAT_SKEW_SEC < Claim->Iat)
		return CDP_RECOVERY_BAD_TIME;
	if (Expect->NowUnix >= Claim->Exp)
		return CDP_RECOVERY_EXPIRED;
	if (!CdpRecoveryConstEqual(Claim->Fp, Expect->Fingerprint, CDP_RECOVERY_FP_LEN))
		return CDP_RECOVERY_FP;
	idLen = CdpRecoveryTextLen(Claim->CredentialId, sizeof(Claim->CredentialId));
	expectLen = CdpRecoveryTextLen(Expect->CredentialId, 64);
	idDiff = (unsigned char)(idLen != expectLen);
	for (i = 0; i < 64; ++i)
	{
		char left = i < idLen ? Claim->CredentialId[i] : 0;
		char right = i < expectLen ? Expect->CredentialId[i] : 0;
		idDiff |= (unsigned char)(left ^ right);
	}
	if (idDiff)
		return CDP_RECOVERY_CREDENTIAL;
	if (Claim->AuthEpoch != Expect->AuthEpoch)
		return CDP_RECOVERY_EPOCH;
	if (!CdpRecoveryConstEqual(Claim->Nonce, Expect->Nonce, CDP_RECOVERY_NONCE_LEN))
		return CDP_RECOVERY_NONCE;
	if (Expect->SeenJtiHashes)
	{
		for (i = 0; i < Expect->SeenCount; ++i)
		{
			if (CdpRecoveryConstEqual(
				Claim->JtiHash,
				Expect->SeenJtiHashes + i * CDP_RECOVERY_JTI_HASH_LEN,
				CDP_RECOVERY_JTI_HASH_LEN))
			{
				return CDP_RECOVERY_REPLAY;
			}
		}
	}
	return CDP_RECOVERY_OK;
}
