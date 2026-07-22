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

BOOL CdpInstallDriverPackage(void);

#ifdef __cplusplus
}
#endif
