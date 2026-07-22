#pragma once

#ifdef Cdp_USERMODE

#include <stdlib.h>

#define Cdp_ALLOC(size) malloc(size)
#define Cdp_ALLOC0(size) calloc(1, (size))

static __forceinline void CdpFreeOptional(PVOID Ptr)
{
	if (Ptr)
		free(Ptr);
}

#define Cdp_FREE(ptr) CdpFreeOptional((ptr))

#else

#include "CdpEngineDefs.h"

#define Cdp_ALLOC(size) cdpalloc(size)

static __forceinline VOID CdpFreeOptional(_In_opt_ PVOID Ptr)
{
	if (Ptr)
		cdpfree(Ptr);
}

#define Cdp_FREE(ptr) CdpFreeOptional((ptr))

static __forceinline PVOID Cdp_ALLOC0(_In_ SIZE_T Size)
{
	PVOID p = cdpalloc(Size);
	if (p)
		RtlZeroMemory(p, Size);
	return p;
}

#endif
