/*
 * Vendor 公钥与 CNG 封装（LICENSE_DESIGN.md §3.1 / §8）
 *
 * 申请密文布局（与 server hybrid.py version=1 一致）：
 *   u8 version=1 | u16 BE ek_len | RSA-OAEP(ek) | 12B nonce | AES-GCM(ct||tag)
 *   AAD = 单字节 0x01
 *
 * License：长度前缀 JSON + base64(PSS 签名)；验签用内嵌公钥，禁止自带公钥。
 */

#ifdef CDP_LICENSE
#ifdef CDP_LICENSE_OBFUSCATE

#include "CdpLicenseTrust.h"
#include "CdpLocalSeal.h"
#include <bcrypt.h>
#include "CdpLicenseSeg.h" /* 公钥分片与验签代码进入 .licpr / .licprot */
#include "CdpLicensePubKey.inc"

/* RSA-2048 + SHA-256 下 cryptography.PSS.MAX_LENGTH = 256-32-2 = 222 */
static const ULONG Cdp_RSA_PSS_SALT_LEN = 222;

/* 计算数据的SHA-256摘要 */
NTSTATUS CdpLicenseSha256(
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

	if (!Data || !Digest)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!Data || !Digest)");
		return STATUS_INVALID_PARAMETER;
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
	hashObj = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, hashObjSize, 'tsrT');
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
		RtlSecureZeroMemory(hashObj, hashObjSize);
		ExFreePoolWithTag(hashObj, 'tsrT');
	}
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	return status;
}

NTSTATUS CdpLicenseRandomBytes(
	_Out_writes_bytes_(Length) UCHAR* Buffer,
	_In_ ULONG Length)
{
	if (!Buffer || Length == 0)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!Buffer || Length == 0)");
		return STATUS_INVALID_PARAMETER;
	}
	return BCryptGenRandom(NULL, Buffer, Length, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

/*
 * 组装结果的结构自检常量（O-01/O-02）
 *
 * 注意：内核 WDK 的 bcrypt.h 里 BCRYPT_RSAPUBLIC_BLOB 只是宽字符串宏
 * （L"RSAPUBLICBLOB"，用于 BCryptImportKeyPair 的 blob 类型参数），并没有对应的
 * 结构体；可解析的头部结构是 BCRYPT_RSAKEY_BLOB（6 个 ULONG，不含指针）。
 * 因此这里按"头部 + 紧跟的指数 + 紧跟的模数"的紧凑布局自行按偏移解析，
 * 不能用用户态 SDK winternl.h 里那种带 PublicExponent/Modulus 指针的结构。
 *
 * 紧凑布局：
 *   Magic(4) | BitLength(4) | cbPublicExp(4) | cbModulus(4) | exp[cbExp] | mod[cbMod]
 *   4+4+4+4 + 3 + 256 = 283 = Cdp_VENDOR_PUB_BLOB_SIZE
 */
#define Cdp_VENDOR_KEY_BITS      2048u
#define Cdp_VENDOR_PUB_EXP       65537UL
#define Cdp_VENDOR_EXP_BYTES     3u  /* 65537 = 0x010001 */
#define Cdp_VENDOR_HDR_BYTES     24u /* sizeof(BCRYPT_RSAKEY_BLOB) 在本实现上为 24 */
#define Cdp_VENDOR_EXP_OFFSET    24u /* 指数紧随头部 */
#define Cdp_VENDOR_MOD_OFFSET    27u /* 24 + 3 */

C_ASSERT(sizeof(BCRYPT_RSAKEY_BLOB) == Cdp_VENDOR_HDR_BYTES);
C_ASSERT(FIELD_OFFSET(BCRYPT_RSAKEY_BLOB, Magic) == 0);
C_ASSERT(FIELD_OFFSET(BCRYPT_RSAKEY_BLOB, BitLength) == 4);
C_ASSERT(FIELD_OFFSET(BCRYPT_RSAKEY_BLOB, cbPublicExp) == 8);
C_ASSERT(FIELD_OFFSET(BCRYPT_RSAKEY_BLOB, cbModulus) == 12);
C_ASSERT(Cdp_VENDOR_MOD_OFFSET + (Cdp_VENDOR_KEY_BITS / 8) ==
	Cdp_VENDOR_PUB_BLOB_SIZE);

/*
 * 使用两个全局数组g_CdpVendorPubShardA和g_CdpVendorPubShardB组装Vendor公钥。
 *
 * O-01/O-02：
 *   blob[i] = ShardA[i] ^ ShardB[i]     —— 分片存放，二进制中不出现完整公钥
 *   再对组装结果做结构自检              —— 见 CdpLicenseCheckVendorBlobStructure
 *
 * 历史实现里的"交叉校验"
 *     check = g_CdpVendorPubShardA[i] ^ g_CdpVendorPubShardB[i];
 *     if (v != check) return TAMPER;
 * 是恒真式：v 与 check 由同一对全局变量按同一表达式算出，对任何取值都不成立，
 * 等于没有校验。分片方案本身保留（能提高静态搜索成本），但校验换成对组装结果
 * 头部/指数/长度的独立断言：攻击者若要换用自己的公钥，必须伪造出能通过该断言的
 * blob（含 2048 位、65537 指数与紧凑布局），而不再是改写任意一个分片就自动成立。
 */
static NTSTATUS CdpLicenseCheckVendorBlobStructure(
	_In_reads_bytes_(Cdp_VENDOR_PUB_BLOB_SIZE) const UCHAR* Blob)
{
	const BCRYPT_RSAKEY_BLOB* hdr = (const BCRYPT_RSAKEY_BLOB*)Blob;
	ULONG expValue = 0;
	ULONG i;

	/* 头部 magic / 位长 / 指数与模数长度 */
	if (hdr->Magic != BCRYPT_RSAPUBLIC_MAGIC ||
		hdr->BitLength != Cdp_VENDOR_KEY_BITS ||
		hdr->cbModulus != (Cdp_VENDOR_KEY_BITS / 8) ||
		hdr->cbPublicExp != Cdp_VENDOR_EXP_BYTES ||
		hdr->cbPrime1 != 0 ||
		hdr->cbPrime2 != 0)
	{
		Cdp_LIC_FAIL("vendor blob header invalid");
		return STATUS_CDP_LICENSE_TAMPER;
	}

	/* 指数按小端读入并比对 65537（0x010001） */
	for (i = 0; i < Cdp_VENDOR_EXP_BYTES; ++i)
		expValue |= ((ULONG)Blob[Cdp_VENDOR_EXP_OFFSET + i]) << (8u * i);
	if (expValue != Cdp_VENDOR_PUB_EXP)
	{
		Cdp_LIC_FAIL("vendor blob exponent != 65537");
		return STATUS_CDP_LICENSE_TAMPER;
	}

	return STATUS_SUCCESS;
}

/* 使用两个全局数组g_CdpVendorPubShardA和g_CdpVendorPubShardB组装Vendor公钥 */
NTSTATUS CdpLicenseTrustAssemblePublicKey(
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength)
{
	UCHAR copyA[Cdp_VENDOR_PUB_BLOB_SIZE];
	UCHAR copyB[Cdp_VENDOR_PUB_BLOB_SIZE];
	NTSTATUS status;
	ULONG i;

	if (!Out || !OutLength)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!Out || !OutLength)");
		return STATUS_INVALID_PARAMETER;
	}
	if (OutCapacity < Cdp_VENDOR_PUB_BLOB_SIZE)
	{
		Cdp_LIC_FAIL("STATUS_BUFFER_TOO_SMALL: if (OutCapacity < Cdp_VENDOR_PUB_BLOB_SIZE)");
		return STATUS_BUFFER_TOO_SMALL;
	}
	RtlCopyMemory(copyA, g_CdpVendorPubShardA, Cdp_VENDOR_PUB_BLOB_SIZE);
	RtlCopyMemory(copyB, g_CdpVendorPubShardB, Cdp_VENDOR_PUB_BLOB_SIZE);
	for (i = 0; i < Cdp_VENDOR_PUB_BLOB_SIZE; ++i)
		Out[i] = (UCHAR)(copyA[i] ^ copyB[i]); // 组装Vendor公钥

	CdpLocalSealSecureZero(copyA, sizeof(copyA));
	CdpLocalSealSecureZero(copyB, sizeof(copyB));

	/* 分片本身的一致性由 XOR 组装保证；这里校验组装结果的正确性 */
	status = CdpLicenseCheckVendorBlobStructure(Out);
	if (!NT_SUCCESS(status))
	{
		CdpLocalSealSecureZero(Out, Cdp_VENDOR_PUB_BLOB_SIZE);
		return status;
	}

	*OutLength = Cdp_VENDOR_PUB_BLOB_SIZE;
	return STATUS_SUCCESS;
}

/* 导入Vendor公钥 */
static NTSTATUS CdpImportVendorPublicKey(
	_Out_ BCRYPT_KEY_HANDLE* KeyHandle)
{
	UCHAR blob[Cdp_VENDOR_PUB_BLOB_SIZE];
	ULONG blobLen = 0;
	BCRYPT_ALG_HANDLE alg = NULL;
	NTSTATUS status;

	*KeyHandle = NULL;
	status = CdpLicenseTrustAssemblePublicKey(blob, sizeof(blob), &blobLen); // 组装Vendor公钥, 结果存储在blob中
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	/*
	 * 必须在 PASSIVE_LEVEL 调用。若调用方持有 FastMutex（APC_LEVEL），
	 * 此处常见返回 STATUS_NOT_SUPPORTED (0xC00000BB)。
	 */
	status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, NULL, 0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("BCryptOpenAlgorithmProvider RSA irql=%u status=0x%08X",
			(ULONG)KeGetCurrentIrql(), status);
		goto done;
	}
	status = BCryptImportKeyPair(
		alg,
		NULL,
		BCRYPT_RSAPUBLIC_BLOB,
		KeyHandle,
		blob,
		blobLen,
		0); // 从密钥 BLOB 中导入公钥
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("BCryptImportKeyPair status=0x%08X", status);
	}
done:
	CdpLocalSealSecureZero(blob, sizeof(blob));
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	return status;
}

NTSTATUS CdpLicenseRsaOaepEncrypt(
	_In_reads_bytes_(PlainLength) const UCHAR* Plain,
	_In_ ULONG PlainLength,
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength)
{
	BCRYPT_KEY_HANDLE key = NULL;
	BCRYPT_OAEP_PADDING_INFO oaep;
	ULONG cbResult = 0;
	NTSTATUS status;

	if (!Plain || !Out || !OutLength || PlainLength == 0)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!Plain || !Out || !OutLength || PlainLength == 0)");
		return STATUS_INVALID_PARAMETER;
	}
	status = CdpImportVendorPublicKey(&key);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	RtlZeroMemory(&oaep, sizeof(oaep));
	oaep.pszAlgId = BCRYPT_SHA256_ALGORITHM;
	status = BCryptEncrypt(
		key,
		(PUCHAR)Plain,
		PlainLength,
		&oaep,
		NULL,
		0,
		Out,
		OutCapacity,
		&cbResult,
		BCRYPT_PAD_OAEP);
	if (NT_SUCCESS(status))
		*OutLength = cbResult;
	if (key)
		BCryptDestroyKey(key);
	return status;
}

NTSTATUS CdpLicenseAesGcmEncrypt(
	_In_reads_bytes_(32) const UCHAR* AesKey,
	_In_reads_bytes_(12) const UCHAR* Nonce,
	_In_reads_bytes_(AadLength) const UCHAR* Aad,
	_In_ ULONG AadLength,
	_In_reads_bytes_(PlainLength) const UCHAR* Plain,
	_In_ ULONG PlainLength,
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength)
{
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_KEY_HANDLE key = NULL;
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
	ULONG keyObjSize = 0;
	ULONG cbResult = 0;
	PUCHAR keyObj = NULL;
	UCHAR tag[16];
	NTSTATUS status;

	if (!AesKey || !Nonce || !Plain || !Out || !OutLength)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!AesKey || !Nonce || !Plain || !Out || !OutLength)");
		return STATUS_INVALID_PARAMETER;
	}
	if (OutCapacity < PlainLength + 16)
	{
		Cdp_LIC_FAIL("STATUS_BUFFER_TOO_SMALL: if (OutCapacity < PlainLength + 16)");
		return STATUS_BUFFER_TOO_SMALL;
	}
	status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}
	status = BCryptSetProperty(
		alg,
		BCRYPT_CHAINING_MODE,
		(PUCHAR)BCRYPT_CHAIN_MODE_GCM,
		sizeof(BCRYPT_CHAIN_MODE_GCM),
		0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
		(PUCHAR)&keyObjSize, sizeof(keyObjSize), &cbResult, 0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	keyObj = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, keyObjSize, 'tsrT');
	if (!keyObj)
	{
		Cdp_LIC_FAIL("STATUS_INSUFFICIENT_RESOURCES then goto done;");
		status = STATUS_INSUFFICIENT_RESOURCES;
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	status = BCryptGenerateSymmetricKey(
		alg, &key, keyObj, keyObjSize, (PUCHAR)AesKey, 32, 0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
	authInfo.pbNonce = (PUCHAR)Nonce;
	authInfo.cbNonce = 12;
	authInfo.pbAuthData = (PUCHAR)Aad;
	authInfo.cbAuthData = AadLength;
	authInfo.pbTag = tag;
	authInfo.cbTag = sizeof(tag);

	status = BCryptEncrypt(
		key,
		(PUCHAR)Plain,
		PlainLength,
		&authInfo,
		NULL,
		0,
		Out,
		PlainLength,
		&cbResult,
		0);
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("goto done, status=0x%08X", status);
		goto done;
	}
	RtlCopyMemory(Out + PlainLength, tag, sizeof(tag));
	*OutLength = PlainLength + sizeof(tag);

done:
	CdpLocalSealSecureZero(tag, sizeof(tag));
	if (key)
		BCryptDestroyKey(key);
	if (keyObj)
	{
		RtlSecureZeroMemory(keyObj, keyObjSize);
		ExFreePoolWithTag(keyObj, 'tsrT');
	}
	if (alg)
		BCryptCloseAlgorithmProvider(alg, 0);
	return status;
}

NTSTATUS CdpLicenseVerifySignedCanonical(
	_In_reads_bytes_(CanonicalLength) const UCHAR* CanonicalWithoutSig,
	_In_ ULONG CanonicalLength,
	_In_reads_bytes_(SignatureLength) const UCHAR* Signature,
	_In_ ULONG SignatureLength)
{
	BCRYPT_KEY_HANDLE key = NULL;
	BCRYPT_PSS_PADDING_INFO pss;
	UCHAR digest[32];
	NTSTATUS status;

	if (!CanonicalWithoutSig || !Signature ||
		CanonicalLength == 0 || SignatureLength == 0)
	{
		Cdp_LIC_FAIL("STATUS_INVALID_PARAMETER: if (!CanonicalWithoutSig || !Signature ||");
		return STATUS_INVALID_PARAMETER;
	}

	/*
	 * BCryptVerifySignature 的 pbHash 对 RSA-PSS 必须是「已算好的摘要」，
	 * 不是原文。cryptography 的 sign(data, PSS, SHA256) 内部先 SHA-256(data)；
	 * 若把整段 canonical 直接传入，cbHash≠32 → STATUS_INVALID_PARAMETER (0xC000000D)。
	 */
	status = CdpLicenseSha256(CanonicalWithoutSig, CanonicalLength, digest); // 计算canonical的SHA-256摘要（结果存储在digest中）
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		return status;
	}

	status = CdpImportVendorPublicKey(&key); // 导入Vendor公钥
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("failed status=0x%08X", status);
		CdpLocalSealSecureZero(digest, sizeof(digest));
		return status;
	}
	RtlZeroMemory(&pss, sizeof(pss));
	pss.pszAlgId = BCRYPT_SHA256_ALGORITHM;
	pss.cbSalt = Cdp_RSA_PSS_SALT_LEN; /* 222 = cryptography PSS.MAX_LENGTH */
	status = BCryptVerifySignature(
		key,
		&pss,
		digest,
		sizeof(digest),
		(PUCHAR)Signature,
		SignatureLength,
		BCRYPT_PAD_PSS); // 验证签名
	if (key)
		BCryptDestroyKey(key);
	CdpLocalSealSecureZero(digest, sizeof(digest));
	if (!NT_SUCCESS(status))
	{
		Cdp_LIC_FAIL("BCryptVerifySignature failed, status=0x%08X sigLen=%lu",
			status, SignatureLength);
		return STATUS_CDP_LICENSE_INVALID;
	}
	return STATUS_SUCCESS;
}

#include "CdpLicenseSegEnd.h" /* 恢复默认 code/const 节 */

#endif /* CDP_LICENSE_OBFUSCATE */
#endif /* CDP_LICENSE */
