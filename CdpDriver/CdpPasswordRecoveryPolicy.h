#pragma once

/*
 * 找回凭据的纯策略检查。
 * 不验厂商签名，也不碰内核对象，用户态单测和驱动共用这一份。
 * 签名通过之后，驱动再用这里的结果决定是否武装句柄。
 */

#define CDP_RECOVERY_OK 0
#define CDP_RECOVERY_BAD_FORMAT 1
#define CDP_RECOVERY_BAD_TYP 2
#define CDP_RECOVERY_BAD_VER 3
#define CDP_RECOVERY_EXPIRED 4
#define CDP_RECOVERY_FP 5
#define CDP_RECOVERY_CREDENTIAL 6
#define CDP_RECOVERY_EPOCH 7
#define CDP_RECOVERY_NONCE 8
#define CDP_RECOVERY_REPLAY 9
#define CDP_RECOVERY_BAD_TIME 10

#define CDP_RECOVERY_FP_LEN 32u
#define CDP_RECOVERY_NONCE_LEN 16u
#define CDP_RECOVERY_JTI_HASH_LEN 32u
#define CDP_RECOVERY_IAT_SKEW_SEC 300

typedef struct _CDP_RECOVERY_CLAIM
{
	char Typ[32];
	unsigned int Ver;
	long long Iat;
	long long Exp;
	unsigned char Fp[CDP_RECOVERY_FP_LEN];
	char CredentialId[64];
	unsigned long long AuthEpoch;
	unsigned char Nonce[CDP_RECOVERY_NONCE_LEN];
	unsigned char JtiHash[CDP_RECOVERY_JTI_HASH_LEN];
} CDP_RECOVERY_CLAIM;

typedef struct _CDP_RECOVERY_EXPECT
{
	const unsigned char* Fingerprint;
	const char* CredentialId;
	unsigned long long AuthEpoch;
	const unsigned char* Nonce;
	long long NowUnix;
	const unsigned char* SeenJtiHashes;
	unsigned int SeenCount;
} CDP_RECOVERY_EXPECT;

/* 对任意字节做 SHA-256。jti 的重放摘要用的就是这个结果。 */
void CdpRecoverySha256(
	const void* Data,
	unsigned int Length,
	unsigned char Out[32]);

/*
 * 凭据文件：u32BE JSON 长度 + UTF-8 JSON + u32BE 签名长度 + RSA-PSS 签名。
 * 成功时 Json/Sig 指向 Blob 内部，调用方不要释放。
 */
int CdpRecoverySplitCredential(
	const unsigned char* Blob,
	unsigned int BlobLength,
	const unsigned char** Json,
	unsigned int* JsonLength,
	const unsigned char** Signature,
	unsigned int* SignatureLength);

/* 从已验签的 canonical JSON 体里取出重设声明。不接受转义字符。 */
int CdpRecoveryParseClaim(
	const unsigned char* Json,
	unsigned int JsonLength,
	CDP_RECOVERY_CLAIM* Claim);

/*
 * 依次核对类型、版本、时间窗、指纹、凭据、代次、一次性 nonce 和已见 jti。
 * 指纹与 nonce 的比较走完整缓冲区，避免提前返回暴露差异位置。
 */
int CdpRecoveryCheckClaim(
	const CDP_RECOVERY_CLAIM* Claim,
	const CDP_RECOVERY_EXPECT* Expect);
