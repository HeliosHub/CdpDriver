#ifdef CDP_LICENSE

#include "CdpLicenseGate.h"
#include "CdpLocalSeal.h"
#include "CdpLicenseSeg.h"

/*
 * Keep this translation unit deliberately narrow: it is compiled normally
 * first so MSBuild can link, then Release|x64 post-processing replaces only
 * its object with a kernel-profile xollvm virtual machine implementation.
 */
#if defined(CDP_XOLLVM_BUILD)
#define CDP_XOLLVM_VM \
	__attribute__((annotate("obf: vm(minBlocks=1,hardened=0,regEncrypt=0,handlerVariants=1)")))
#else
#define CDP_XOLLVM_VM
#endif

CDP_XOLLVM_VM
NTSTATUS CdpLicenseVmRecomputeCap(
	_In_ ULONG SealKeyValid,
	_In_ ULONG PendingCapValid,
	_In_reads_bytes_(32) const UCHAR* SealKey,
	_In_ UINT64 T0,
	_In_ ULONG C0,
	_In_ ULONG OpsT,
	_In_ ULONG OpsS,
	_In_ ULONG AMod,
	_In_ ULONG Nonce,
	_In_ UINT64 Issued100ns,
	_Out_writes_bytes_(Cdp_CAP_TOKEN_BYTES) UCHAR* TokenOut)
{
	if (SealKeyValid == 0 || PendingCapValid == 0)
		return STATUS_CDP_LICENSE_TAMPER;

	return CdpLocalSealMakeCapToken(
		SealKey, T0, C0, OpsT, OpsS, AMod, Nonce, Issued100ns, TokenOut);
}

CDP_XOLLVM_VM
BOOLEAN CdpLicenseVmArmEvidenceValid(
	_In_ ULONG PendingCapValid,
	_In_ ULONG LocalCapValid,
	_In_ ULONG LocalNonce,
	_In_ ULONG PendingNonce,
	_In_ UINT64 LocalIssued100ns,
	_In_ UINT64 PendingIssued100ns,
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* LocalToken,
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* PendingToken,
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* RecomputedToken)
{
	ULONG i;
	UCHAR localDiff = 0;
	UCHAR recomputedDiff = 0;

	if (PendingCapValid == 0 || LocalCapValid == 0 ||
		LocalNonce != PendingNonce || LocalIssued100ns != PendingIssued100ns)
		return FALSE;
	for (i = 0; i < Cdp_CAP_TOKEN_BYTES; ++i) {
		localDiff |= (UCHAR)(LocalToken[i] ^ PendingToken[i]);
		recomputedDiff |= (UCHAR)(RecomputedToken[i] ^ PendingToken[i]);
	}
	/*
	 * Keep the two checks as distinct exits.  Besides preserving the
	 * constant-time comparison loops above, this avoids an i1 SSA merge in
	 * unoptimised LLVM IR, which the compact kernel VM intentionally omits.
	 */
	if (localDiff != 0)
		return FALSE;
	if (recomputedDiff != 0)
		return FALSE;
	return TRUE;
}

CDP_XOLLVM_VM
BOOLEAN CdpLicenseVmCommitEvidenceValid(
	_In_ ULONG PendingCapValid,
	_In_ ULONG PendingCapArmed,
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* RecomputedToken,
	_In_reads_bytes_(Cdp_CAP_TOKEN_BYTES) const UCHAR* PendingToken)
{
	ULONG i;
	UCHAR diff = 0;

	if (PendingCapValid == 0 || PendingCapArmed == 0)
		return FALSE;
	for (i = 0; i < Cdp_CAP_TOKEN_BYTES; ++i)
		diff |= (UCHAR)(RecomputedToken[i] ^ PendingToken[i]);
	return (diff == 0) ? TRUE : FALSE;
}

#undef CDP_XOLLVM_VM

#include "CdpLicenseSegEnd.h"

#endif /* CDP_LICENSE */
