#pragma once

#ifdef QH_USERMODE

#include <stdlib.h>

#define QH_ALLOC(size) malloc(size)
#define QH_ALLOC0(size) calloc(1, (size))

static __forceinline void QHFreeOptional(PVOID Ptr)
{
	if (Ptr)
		free(Ptr);
}

#define QH_FREE(ptr) QHFreeOptional((ptr))

#else

#include "QHEngineDefs.h"

#define QH_ALLOC(size) qhalloc(size)

static __forceinline VOID QHFreeOptional(_In_opt_ PVOID Ptr)
{
	if (Ptr)
		qhfree(Ptr);
}

#define QH_FREE(ptr) QHFreeOptional((ptr))

static __forceinline PVOID QH_ALLOC0(_In_ SIZE_T Size)
{
	PVOID p = qhalloc(Size);
	if (p)
		RtlZeroMemory(p, Size);
	return p;
}

#endif
