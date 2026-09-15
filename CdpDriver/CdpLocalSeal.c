/*
 * CdpLocalSeal-v1 实现（LICENSE_DESIGN.md §7）
 *
 * 流程概要：
 *   1) DeriveKey：K = SHA256(fp || BE(T_EXP) || PRODUCT_MAGIC)
 *   2) 轮密钥：RKi = SHA256(K || "cdp-rk" || i)[0..15]，i=0..11
 *   3) 12 轮 128-bit Feistel + 专有 ARX 轮函数 F
 *   4) 多块 CBC；E0 使用确定性 IV = SHA256(K||"e0-iv")[0..15]
 *   5) tag = SHA256(K || iv || ciphertext)[0..15]
 *
 * 用毕对 K / 轮密钥 / 临时缓冲 RtlSecureZeroMemory。
 */

#ifdef CDP_LICENSE
#ifdef CDP_LICENSE_OBFUSCATE

#include "CdpLocalSeal.h"
#include <bcrypt.h>
#include "CdpLicenseSeg.h" /* 此后本文件代码/常量进入 .licprot / .licpr */

#pragma comment(lib, "cng.lib")

/* PRODUCT_MAGIC 分片存储，运行时 XOR（O-13）；勿把明文 magic 直接入库 */
static const UCHAR g_CdpProductMagicA[16] = {
	0xA3, 0x51, 0xC7, 0x2E, 0x19, 0x8B, 0x44, 0xF0,
	0x6D, 0xE2, 0x37, 0x91, 0x0C, 0x5A, 0xB8, 0xD4
};
static const UCHAR g_CdpProductMagicB[16] = {
	0x5C, 0x2E, 0x9A, 0x71, 0xE6, 0x34, 0xB8, 0x0F,
	0xC1, 0x47, 0x82, 0x3E, 0xA9, 0x15, 0x6F, 0x03
};

/* 轮函数 F 的 ARX 常量分片（实现写死，勿随意更改） */
static const UCHAR g_CdpArxConstA[8] = {
	0x9E, 0x37, 0x79, 0xB9, 0x7F, 0x4A, 0x7C, 0x15
};
static const UCHAR g_CdpArxConstB[8] = {
	0x63, 0xC8, 0x86, 0x46, 0x80, 0xB5, 0x83, 0xEA
};

#define Cdp_CRC32C_POLY 0x82F63B78UL

static ULONG g_CdpLicCrcTable[256];
static volatile LONG g_CdpLicCrcReady;

/* 将内存区域清零 */
VOID CdpLocalSealSecureZero(
	_Out_writes_bytes_(Length) PVOID Buffer,
	_In_ SIZE_T Length)
{
	if (Buffer && Length)
		RtlSecureZeroMemory(Buffer, Length);
}

/*
 * 初始化 CRC 表 g_CdpLicCrcTable 。
 */
static VOID CdpLicCrcInit(VOID)
{
	ULONG table[256];
	ULONG i;

	if (InterlockedCompareExchange(&g_CdpLicCrcReady, 1, 0) != 0)
	{
		while (InterlockedCompareExchange(&g_CdpLicCrcReady, 0, 0) != 2)
			YieldProcessor();
		return;
	}
	for (i = 0; i < 256; ++i)
	{
		ULONG crc = i;
		ULONG bit;
		for (bit = 0; bit < 8; ++bit)
			crc = (crc & 1) ? (Cdp_CRC32C_POLY ^ (crc >> 1)) : (crc >> 1);
		table[i] = crc;
	}
	RtlCopyMemory(g_CdpLicCrcTable, table, sizeof(table));
	InterlockedExchange(&g_CdpLicCrcReady, 2);
}

ULONG CdpLicenseCrc32c(
	_In_ ULONG Crc,
	_In_reads_bytes_(Length) const VOID* Buffer,
	_In_ ULONG Length)
{
	const UCHAR* bytes = (const UCHAR*)Buffer;
	ULONG crc = Crc ^ 0xFFFFFFFFUL;

	if (InterlockedCompareExchange(&g_CdpLicCrcReady, 0, 0) != 2)
		CdpLicCrcInit();
	while (Length--)
		crc = g_CdpLicCrcTable[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFUL;
}

static VOID CdpAssembleProductMagic(_Out_writes_bytes_(16) UCHAR* Magic)
{
	ULONG i;
	for (i = 0; i < 16; ++i)
		Magic[i] = (UCHAR)(g_CdpProductMagicA[i] ^ g_CdpProductMagicB[i]);
}

static UINT64 CdpRotl64(_In_ UINT64 Value, _In_ ULONG Bits)
{
	return (Value << Bits) | (Value >> (64 - Bits));
}

/* 专有 ARX 轮函数 F(R, RKi) → 64-bit（设计 §7.2） */
static UINT64 CdpFeistelF(
	_In_ UINT64 R,
	_In_reads_bytes_(16) const UCHAR* RoundKey)
{
	UINT64 k0;
	UINT64 k1;
	UINT64 c;
	UINT64 x;

	RtlCopyMemory(&k0, RoundKey, 8);
	RtlCopyMemory(&k1, RoundKey + 8, 8);
	c = 0;
	{
		UCHAR tmp[8];
		ULONG i;
		for (i = 0; i < 8; ++i)
			tmp[i] = (UCHAR)(g_CdpArxConstA[i] ^ g_CdpArxConstB[i]);
		RtlCopyMemory(&c, tmp, 8);
		CdpLocalSealSecureZero(tmp, sizeof(tmp));
	}
	x = R + k0;
	x = CdpRotl64(x, 13) ^ k1;
	x = CdpRotl64(x, 17) + c;
	x ^= R;
	x = CdpRotl64(x, 11) + k0;
	return x;
}

static NTSTATUS CdpSha256(
	_In_reads_bytes_(Length) const UCHAR* Data,
	_In_ ULONG Length,
	_Out_writes_bytes_(32) UCHAR* Digest)
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	NTSTATUS status;
	ULONG hashObjSize = 0;
	ULONG cbResult = 0;
	PUCHAR hashObj = NULL;

	if (KeGetCurrentIrql() > PASSIVE_LEVEL)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_DEVICE_STATE: SHA256 at IRQL=%u",
			(ULONG)KeGetCurrentIrql());
		return STATUS_INVALID_DEVICE_STATE;
	}
	status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
		(PUCHAR)&hashObjSize, sizeof(hashObjSize), &cbResult, 0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	hashObj = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, hashObjSize, 'leaS');
	if (!hashObj)
	{
		Cdp_LIC_FAIL("STATUS_INSUFFICIENT_RESOURCES then goto done;");
		status = STATUS_INSUFFICIENT_RESOURCES;
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	status = BCryptCreateHash(alg, &hash, hashObj, hashObjSize, NULL, 0, 0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	status = BCryptHashData(hash, (PUCHAR)Data, Length, 0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	status = BCryptFinishHash(hash, Digest, 32, 0);
done:
	if (hash)
		BCryptDestroyHash(hash);
	if (hashObj)
	{
		CdpLocalSealSecureZero(hashObj, hashObjSize);
		ExFreePoolWithTag(hashObj, 'leaS');
	}
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	return status;
}

// 将64位整数转换为大端字节序
static VOID CdpEncodeU64Be(_In_ UINT64 Value, _Out_writes_bytes_(8) UCHAR* Out)
{
	ULONG i;
	for (i = 0; i < 8; ++i)
		Out[i] = (UCHAR)((Value >> (56 - i * 8)) & 0xFF);
}

static NTSTATUS CdpDeriveRoundKey(
	_In_reads_bytes_(32) const UCHAR* K,
	_In_ ULONG RoundIndex,
	_Out_writes_bytes_(16) UCHAR* RoundKey)
{
	UCHAR material[32 + 6 + 4];
	UCHAR digest[32];
	NTSTATUS status;

	RtlCopyMemory(material, K, 32);
	RtlCopyMemory(material + 32, "cdp-rk", 6);
	material[38] = (UCHAR)((RoundIndex >> 24) & 0xFF);
	material[39] = (UCHAR)((RoundIndex >> 16) & 0xFF);
	material[40] = (UCHAR)((RoundIndex >> 8) & 0xFF);
	material[41] = (UCHAR)(RoundIndex & 0xFF);
	status = CdpSha256(material, sizeof(material), digest);
	if (NT_SUCCESS(status))
		RtlCopyMemory(RoundKey, digest, 16);
	CdpLocalSealSecureZero(material, sizeof(material));
	CdpLocalSealSecureZero(digest, sizeof(digest));
	return status;
}

/* 单块 16 字节：12 轮 Feistel 加密 */
static VOID CdpFeistelEncryptBlock(
	_Inout_updates_bytes_(16) UCHAR* Block,
	_In_reads_(12 * 16) const UCHAR* RoundKeys)
{
	UINT64 L;
	UINT64 R;
	ULONG round;

	RtlCopyMemory(&L, Block, 8);
	RtlCopyMemory(&R, Block + 8, 8);
	for (round = 0; round < 12; ++round)
	{
		UINT64 nL = R;
		UINT64 nR = L ^ CdpFeistelF(R, RoundKeys + round * 16);
		L = nL;
		R = nR;
	}
	RtlCopyMemory(Block, &L, 8);
	RtlCopyMemory(Block + 8, &R, 8);
}

/* 单块解密：逆序轮，结构与加密对称 */
static VOID CdpFeistelDecryptBlock(
	_Inout_updates_bytes_(16) UCHAR* Block,
	_In_reads_(12 * 16) const UCHAR* RoundKeys)
{
	UINT64 L;
	UINT64 R;
	LONG round;

	RtlCopyMemory(&L, Block, 8);
	RtlCopyMemory(&R, Block + 8, 8);
	for (round = 11; round >= 0; --round)
	{
		UINT64 nR = L;
		UINT64 nL = R ^ CdpFeistelF(L, RoundKeys + (ULONG)round * 16);
		L = nL;
		R = nR;
	}
	RtlCopyMemory(Block, &L, 8);
	RtlCopyMemory(Block + 8, &R, 8);
}

static NTSTATUS CdpBuildRoundKeys(
	_In_reads_bytes_(32) const UCHAR* Key,
	_Out_writes_bytes_(12 * 16) UCHAR* RoundKeys)
{
	ULONG i;
	NTSTATUS status = STATUS_SUCCESS;

	for (i = 0; i < 12; ++i)
	{
		status = CdpDeriveRoundKey(Key, i, RoundKeys + i * 16);
		if (!NT_SUCCESS(status)) {
			Cdp_LIC_FAIL("CdpDeriveRoundKey failed status=0x%08X", status);
			break;
		}
	}
	return status;
}

static NTSTATUS CdpDeriveIv(
	_In_reads_bytes_(32) const UCHAR* Key,
	_Out_writes_bytes_(16) UCHAR* Iv)
{
	UCHAR material[32 + 5];
	UCHAR digest[32];
	NTSTATUS status;

	RtlCopyMemory(material, Key, 32);
	RtlCopyMemory(material + 32, "e0-iv", 5);
	status = CdpSha256(material, sizeof(material), digest);
	if (NT_SUCCESS(status))
		RtlCopyMemory(Iv, digest, 16);
	CdpLocalSealSecureZero(material, sizeof(material));
	CdpLocalSealSecureZero(digest, sizeof(digest));
	return status;
}

static NTSTATUS CdpComputeTag(
	_In_reads_bytes_(32) const UCHAR* Key,
	_In_reads_bytes_(16) const UCHAR* Iv,
	_In_reads_bytes_(CipherLen) const UCHAR* Cipher,
	_In_ ULONG CipherLen,
	_Out_writes_bytes_(16) UCHAR* Tag)
{
	PUCHAR material;
	UCHAR digest[32];
	NTSTATUS status;
	ULONG total = 32 + 16 + CipherLen;

	material = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, total, 'leaS');
	if (!material)
	{
		Cdp_LIC_FAIL("STATUS_INSUFFICIENT_RESOURCES: if (!material)");
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlCopyMemory(material, Key, 32);
	RtlCopyMemory(material + 32, Iv, 16);
	RtlCopyMemory(material + 48, Cipher, CipherLen);
	status = CdpSha256(material, total, digest);
	if (NT_SUCCESS(status))
		RtlCopyMemory(Tag, digest, 16);
	CdpLocalSealSecureZero(material, total);
	ExFreePoolWithTag(material, 'leaS');
	CdpLocalSealSecureZero(digest, sizeof(digest));
	return status;
}

NTSTATUS CdpLocalSealDeriveKey(
	_In_reads_bytes_(Cdp_LICENSE_FP_BYTES) const UCHAR* HardwareId,
	_In_ UINT64 TExp100ns,
	_Out_writes_bytes_(32) UCHAR* KeyOut)
{
	/* 材料布局必须与设计 §7.1 字节序一致，否则旧 E0 全部无法解密 */
	UCHAR material[Cdp_LICENSE_FP_BYTES + 8 + 16];
	UCHAR magic[16];
	NTSTATUS status;

	if (!HardwareId || !KeyOut)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!HardwareId || !KeyOut)");
		return STATUS_INVALID_PARAMETER;
	}
	CdpAssembleProductMagic(magic);
	RtlCopyMemory(material, HardwareId, Cdp_LICENSE_FP_BYTES);
	CdpEncodeU64Be(TExp100ns, material + Cdp_LICENSE_FP_BYTES);
	RtlCopyMemory(material + Cdp_LICENSE_FP_BYTES + 8, magic, 16);
	status = CdpSha256(material, sizeof(material), KeyOut);
	CdpLocalSealSecureZero(material, sizeof(material));
	CdpLocalSealSecureZero(magic, sizeof(magic));
	return status;
}

NTSTATUS CdpLocalSealEncrypt(
	_In_reads_bytes_(32) const UCHAR* Key,
	_In_ const Cdp_E0_PLAINTEXT* Plain,
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength)
{
	/*
	 * 明文 32B → CBC 两块：
	 *   block0 ^= IV; Enc; block1 ^= block0; Enc
	 * 输出：magic(4)|ver(2)|res(2)|iv(16)|ct(32)|tag(16) = 72B
	 */
	UCHAR roundKeys[12 * 16];
	UCHAR iv[16];
	UCHAR blocks[32];
	UCHAR tag[16];
	Cdp_E0_PLAINTEXT plainCopy;
	ULONG i;
	NTSTATUS status;
	const ULONG header = 8 + 16; /* magic/ver/res + iv */
	const ULONG cipherLen = 32;
	const ULONG total = header + cipherLen + 16;

	if (!Key || !Plain || !Out || !OutLength)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!Key || !Plain || !Out || !OutLength)");
		return STATUS_INVALID_PARAMETER;
	}
	if (OutCapacity < total)
	{
		Cdp_LIC_FAIL("STATUS_BUFFER_TOO_SMALL: if (OutCapacity < total)");
		return STATUS_BUFFER_TOO_SMALL;
	}
	plainCopy = *Plain;
	plainCopy.SealVersion = Cdp_LOCAL_SEAL_VERSION;
	plainCopy.Reserved = 0;
	plainCopy.Crc32c = 0;
	plainCopy.Crc32c = CdpLicenseCrc32c(
		0, &plainCopy, FIELD_OFFSET(Cdp_E0_PLAINTEXT, Crc32c));

	status = CdpBuildRoundKeys(Key, roundKeys);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	status = CdpDeriveIv(Key, iv);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	RtlCopyMemory(blocks, &plainCopy, 32);
	for (i = 0; i < 16; ++i)
		blocks[i] ^= iv[i];
	CdpFeistelEncryptBlock(blocks, roundKeys);
	for (i = 0; i < 16; ++i)
		blocks[16 + i] ^= blocks[i];
	CdpFeistelEncryptBlock(blocks + 16, roundKeys);

	status = CdpComputeTag(Key, iv, blocks, cipherLen, tag);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	*(PULONG)Out = Cdp_E0_SEAL_MAGIC;
	*(PUSHORT)(Out + 4) = Cdp_E0_SEAL_VERSION;
	*(PUSHORT)(Out + 6) = 0;
	RtlCopyMemory(Out + 8, iv, 16);
	RtlCopyMemory(Out + 24, blocks, cipherLen);
	RtlCopyMemory(Out + 56, tag, 16);
	*OutLength = total;
	status = STATUS_SUCCESS;

done:
	CdpLocalSealSecureZero(roundKeys, sizeof(roundKeys));
	CdpLocalSealSecureZero(iv, sizeof(iv));
	CdpLocalSealSecureZero(blocks, sizeof(blocks));
	CdpLocalSealSecureZero(tag, sizeof(tag));
	CdpLocalSealSecureZero(&plainCopy, sizeof(plainCopy));
	return status;
}

NTSTATUS CdpLocalSealDecrypt(
	_In_reads_bytes_(32) const UCHAR* Key,
	_In_reads_bytes_(InLength) const UCHAR* In,
	_In_ ULONG InLength,
	_Out_ PCdp_E0_PLAINTEXT Plain)
{
	/* 先比 tag（常量时间），再解密；明文 CRC / seal_version 二次校验 */
	UCHAR roundKeys[12 * 16];
	UCHAR iv[16];
	UCHAR blocks[32];
	UCHAR tag[16];
	UCHAR expectTag[16];
	Cdp_E0_PLAINTEXT plainCopy;
	ULONG i;
	ULONG crc;
	NTSTATUS status;
	const ULONG total = 8 + 16 + 32 + 16;

	if (!Key || !In || !Plain || InLength < total)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!Key || !In || !Plain || InLength < total)");
		return STATUS_INVALID_PARAMETER;
	}
	if (*(const ULONG*)In != Cdp_E0_SEAL_MAGIC ||
		*(const USHORT*)(In + 4) != Cdp_E0_SEAL_VERSION)
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_INVALID: if (*(const ULONG*)In != Cdp_E0_SEAL_MAGIC ||");
		return STATUS_CDP_LICENSE_INVALID;
	}

	RtlCopyMemory(iv, In + 8, 16);
	RtlCopyMemory(blocks, In + 24, 32);
	RtlCopyMemory(tag, In + 56, 16);

	status = CdpComputeTag(Key, iv, blocks, 32, expectTag);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	{
		UCHAR diff = 0;
		for (i = 0; i < 16; ++i)
			diff |= (UCHAR)(tag[i] ^ expectTag[i]);
		if (diff != 0)
		{
			Cdp_LIC_FAIL("STATUS_CDP_LICENSE_TAMPER then goto done;");
			status = STATUS_CDP_LICENSE_TAMPER;
			goto done;
		}
	}

	status = CdpBuildRoundKeys(Key, roundKeys);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	CdpFeistelDecryptBlock(blocks + 16, roundKeys);
	for (i = 0; i < 16; ++i)
		blocks[16 + i] ^= blocks[i];
	CdpFeistelDecryptBlock(blocks, roundKeys);
	for (i = 0; i < 16; ++i)
		blocks[i] ^= iv[i];

	RtlCopyMemory(&plainCopy, blocks, 32);
	crc = CdpLicenseCrc32c(0, &plainCopy, FIELD_OFFSET(Cdp_E0_PLAINTEXT, Crc32c));
	if (crc != plainCopy.Crc32c || plainCopy.SealVersion != Cdp_LOCAL_SEAL_VERSION)
	{
		Cdp_LIC_FAIL("STATUS_CDP_LICENSE_TAMPER then goto done;");
		status = STATUS_CDP_LICENSE_TAMPER;
		goto done;
	}
	*Plain = plainCopy;
	status = STATUS_SUCCESS;

done:
	CdpLocalSealSecureZero(roundKeys, sizeof(roundKeys));
	CdpLocalSealSecureZero(iv, sizeof(iv));
	CdpLocalSealSecureZero(blocks, sizeof(blocks));
	CdpLocalSealSecureZero(tag, sizeof(tag));
	CdpLocalSealSecureZero(expectTag, sizeof(expectTag));
	CdpLocalSealSecureZero(&plainCopy, sizeof(plainCopy));
	return status;
}

/*
 * 小端写入 32 位：token 材料必须与平台无关，不能直接 memcpy ULONG
 *（x64 虽也是 LE，但显式拆字节避免以后改对齐/大小端时材料变化导致旧 token 失效）。
 */
static VOID CdpStoreU32Le(_Out_writes_bytes_(4) UCHAR* Out, _In_ ULONG Value)
{
	Out[0] = (UCHAR)(Value & 0xFF);
	Out[1] = (UCHAR)((Value >> 8) & 0xFF);
	Out[2] = (UCHAR)((Value >> 16) & 0xFF);
	Out[3] = (UCHAR)((Value >> 24) & 0xFF);
}

static VOID CdpStoreU64Le(_Out_writes_bytes_(8) UCHAR* Out, _In_ UINT64 Value)
{
	CdpStoreU32Le(Out, (ULONG)(Value & 0xFFFFFFFFUL));
	CdpStoreU32Le(Out + 4, (ULONG)(Value >> 32));
}

/*
 * Capability Token（O-18）：
 *   SHA256( "cdp-cap-v1"(10) || SealKey(32) || T0(8 LE) || C0 || OPS_T || OPS_S
 *           || A_MOD || nonce || issued(8 LE) )
 * 复用 E0 的 SealKey，不另派生密钥。材料缓冲用毕 SecureZero。
 */
NTSTATUS CdpLocalSealMakeCapToken(
	_In_reads_bytes_(32) const UCHAR* Key,
	_In_ UINT64 T0,
	_In_ ULONG C0,
	_In_ ULONG OpsT,
	_In_ ULONG OpsS,
	_In_ ULONG AMod,
	_In_ ULONG Nonce,
	_In_ UINT64 Issued100ns,
	_Out_writes_bytes_(Cdp_CAP_TOKEN_BYTES) UCHAR* TokenOut)
{
	UCHAR material[10 + 32 + 8 + 4 + 4 + 4 + 4 + 4 + 8];
	ULONG o = 0;
	NTSTATUS status;

	if (!Key || !TokenOut)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: cap token");
		return STATUS_INVALID_PARAMETER;
	}

	RtlCopyMemory(material + o, "cdp-cap-v1", 10);
	o += 10;
	RtlCopyMemory(material + o, Key, 32);
	o += 32;
	CdpStoreU64Le(material + o, T0);
	o += 8;
	CdpStoreU32Le(material + o, C0);
	o += 4;
	CdpStoreU32Le(material + o, OpsT);
	o += 4;
	CdpStoreU32Le(material + o, OpsS);
	o += 4;
	CdpStoreU32Le(material + o, AMod);
	o += 4;
	CdpStoreU32Le(material + o, Nonce);
	o += 4;
	CdpStoreU64Le(material + o, Issued100ns);
	o += 8;
	status = CdpSha256(material, o, TokenOut);
	CdpLocalSealSecureZero(material, sizeof(material));
	return status;
}

/*
 * 恒定时间比较：acc |= A[i]^B[i]，避免 memcmp 在首个差异字节提前返回
 *（给侧信道少一个「错在第几字节」的信号；内核里收益有限但代价为零）。
 */
BOOLEAN CdpLocalSealCapTokenEqual(
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* A,
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* B)
{
	ULONG i;
	UCHAR acc = 0;

	if (!A || !B)
		return FALSE;
	for (i = 0; i < Cdp_CAP_TOKEN_BYTES; ++i)
		acc = (UCHAR)(acc | (A[i] ^ B[i]));
	return acc == 0;
}

#include "CdpLicenseSegEnd.h" /* 恢复默认 code/const 节 */

#endif /* CDP_LICENSE_OBFUSCATE */
#endif /* CDP_LICENSE */
