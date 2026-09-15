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

/* 使用两个全局数组g_CdpVendorPubShardA和g_CdpVendorPubShardB组装Vendor公钥 */
NTSTATUS CdpLicenseTrustAssemblePublicKey(
	_Out_writes_bytes_to_(OutCapacity, *OutLength) UCHAR* Out,
	_In_ ULONG OutCapacity,
	_Out_ PULONG OutLength)
{
	/*
	 * O-01/O-02：运行时 XOR 组装；并与分片再算一遍交叉校验，
	 * 防止只 patch 其中一个副本。
	 */
	UCHAR copyA[Cdp_VENDOR_PUB_BLOB_SIZE];
	UCHAR copyB[Cdp_VENDOR_PUB_BLOB_SIZE];
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
	{
		UCHAR v = (UCHAR)(copyA[i] ^ copyB[i]); // 组装Vendor公钥
		UCHAR check = (UCHAR)(g_CdpVendorPubShardA[i] ^ g_CdpVendorPubShardB[i]); // 交叉校验
		if (v != check)
		{
			CdpLocalSealSecureZero(copyA, sizeof(copyA));
			CdpLocalSealSecureZero(copyB, sizeof(copyB));
			Cdp_LIC_FAIL("STATUS_CDP_LICENSE_TAMPER: if (v != check)");
			return STATUS_CDP_LICENSE_TAMPER;
		}
		Out[i] = v;
	}
	*OutLength = Cdp_VENDOR_PUB_BLOB_SIZE;
	CdpLocalSealSecureZero(copyA, sizeof(copyA));
	CdpLocalSealSecureZero(copyB, sizeof(copyB));
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
