/*
 * Usermode / shared portable types for SysRestoreCore.
 * When QH_USERMODE is defined, replaces ntddk.h for journal + tree code.
 */
#pragma once

#ifdef QH_USERMODE

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL              ((NTSTATUS)0xC0000001L)
#endif
#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED           ((NTSTATUS)0xC0000002L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER         ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES    ((NTSTATUS)0xC000009AL)
#endif
#ifndef STATUS_DEVICE_NOT_READY
#define STATUS_DEVICE_NOT_READY          ((NTSTATUS)0xC00000A3L)
#endif
#ifndef STATUS_NO_SUCH_DEVICE
#define STATUS_NO_SUCH_DEVICE            ((NTSTATUS)0xC000000EL)
#endif
#ifndef STATUS_DISK_CORRUPT_ERROR
#define STATUS_DISK_CORRUPT_ERROR        ((NTSTATUS)0xC0000032L)
#endif
#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND                 ((NTSTATUS)0xC0000225L)
#endif
#ifndef STATUS_INVALID_DEVICE_STATE
#define STATUS_INVALID_DEVICE_STATE      ((NTSTATUS)0xC0000184L)
#endif
#ifndef STATUS_IO_DEVICE_ERROR
#define STATUS_IO_DEVICE_ERROR           ((NTSTATUS)0xC000018DL)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL          ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_UNEXPECTED_IO_ERROR
#define STATUS_UNEXPECTED_IO_ERROR       ((NTSTATUS)0xC00000E9L)
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#endif

#ifndef BOOLEAN
typedef unsigned char BOOLEAN;
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

typedef unsigned __int64 UINT64;
typedef unsigned long ULONG;
typedef long LONG;
typedef unsigned char UCHAR;
typedef unsigned char* PUCHAR;
typedef void* PVOID;
typedef size_t SIZE_T;
typedef ULONG* PULONG;

#ifndef MAXULONG
#define MAXULONG 0xFFFFFFFFUL
#endif

#ifndef GUID_DEFINED
#define GUID_DEFINED
typedef struct _GUID {
	unsigned long Data1;
	unsigned short Data2;
	unsigned short Data3;
	unsigned char Data4[8];
} GUID;
#endif

#define IRP_MJ_READ  0x03
#define IRP_MJ_WRITE 0x04

#ifndef RTL_NUMBER_OF
#define RTL_NUMBER_OF(A) (sizeof(A) / sizeof((A)[0]))
#endif

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) ((void)(P))
#endif

#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) ((LONG)(LONG_PTR)&(((type *)0)->field))
#endif

#ifndef RtlCopyMemory
#define RtlCopyMemory(d, s, n) memcpy((d), (s), (n))
#define RtlZeroMemory(d, n) memset((d), 0, (n))
#define RtlFillMemory(d, n, v) memset((d), (v), (n))
#endif

#ifndef C_ASSERT
#define C_ASSERT(e) static_assert((e), #e)
#endif

#define qhalloc(size) malloc(size)
#define qhfree(P) free(P)

typedef CRITICAL_SECTION QH_LOCK;

static __forceinline void QH_LOCK_INIT(_Out_ QH_LOCK* Lock)
{
	InitializeCriticalSection(Lock);
}

static __forceinline void QH_LOCK_ACQUIRE(_Inout_ QH_LOCK* Lock)
{
	EnterCriticalSection(Lock);
}

static __forceinline void QH_LOCK_RELEASE(_Inout_ QH_LOCK* Lock)
{
	LeaveCriticalSection(Lock);
}

static __forceinline void QH_LOCK_DELETE(_Inout_ QH_LOCK* Lock)
{
	DeleteCriticalSection(Lock);
}

#else /* kernel */

#include <ntddk.h>

typedef KMUTEX QH_LOCK;

static __forceinline VOID QH_LOCK_INIT(_Out_ QH_LOCK* Lock)
{
	KeInitializeMutex(Lock, 0);
}

static __forceinline VOID QH_LOCK_ACQUIRE(_Inout_ QH_LOCK* Lock)
{
	KeWaitForSingleObject(Lock, Executive, KernelMode, FALSE, NULL);
}

static __forceinline VOID QH_LOCK_RELEASE(_Inout_ QH_LOCK* Lock)
{
	KeReleaseMutex(Lock, FALSE);
}

static __forceinline VOID QH_LOCK_DELETE(_Inout_ QH_LOCK* Lock)
{
	UNREFERENCED_PARAMETER(Lock);
}

#endif /* QH_USERMODE */
