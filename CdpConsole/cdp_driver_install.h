#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL CdpResolveDriverInfPath(
	_Out_writes_(cchInfPath) wchar_t* infPath,
	_In_ size_t cchInfPath);

BOOL CdpIsDriverServiceInstalled(void);

BOOL CdpInstallDriverFromInf(_In_ const wchar_t* infPath);

BOOL CdpRegisterVolumeUpperFilter(void);

BOOL CdpRegisterDiskUpperFilter(void);

BOOL CdpInstallBootConfirmService(void);

BOOL CdpInstallDriverPackage(void);

const wchar_t* CdpGetInstallFailureStage(void);

#ifdef __cplusplus
}
#endif
