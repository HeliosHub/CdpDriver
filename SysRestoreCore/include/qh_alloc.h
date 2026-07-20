#pragma once

#ifdef QH_USERMODE

#include <stdlib.h>

#define QH_ALLOC(size) malloc(size)
#define QH_ALLOC0(size) calloc(1, (size))
#define QH_FREE(ptr) free(ptr)

#else

#include "QHEngineDefs.h"

#define QH_ALLOC(size) qhalloc(size)
#define QH_FREE(ptr) qhfree(ptr)

static __forceinline PVOID QH_ALLOC0(_In_ SIZE_T Size)
{
	PVOID p = qhalloc(Size);
	if (p)
		RtlZeroMemory(p, Size);
	return p;
}

#endif
