#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL QhResolveDriverInfPath(
	_Out_writes_(cchInfPath) wchar_t* infPath,
	_In_ size_t cchInfPath);

BOOL QhIsDriverServiceInstalled(void);

BOOL QhInstallDriverFromInf(_In_ const wchar_t* infPath);

BOOL QhRegisterVolumeUpperFilter(void);

BOOL QhInstallDriverPackage(void);

#ifdef __cplusplus
}
#endif
