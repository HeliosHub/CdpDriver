#include "cdp_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_caseFailed;

static void Expect(int cond, const char* msg)
{
	if (!cond)
	{
		printf("FAIL: %s\n", msg);
		g_caseFailed++;
	}
	else
	{
		printf("OK:   %s\n", msg);
	}
}

static void ExpectStatus(NTSTATUS s, NTSTATUS want, const char* msg)
{
	Expect(s == want, msg);
}

#define SECTOR         512
#define SRC_SIZE       (64ULL * 1024)
#define JNL_SIZE       (8ULL * 1024 * 1024)
#define SMALL_JNL_SIZE (Cdp_JOURNAL_HEADER_REGION_SIZE + 1ULL * 1024 * 1024)
#define TINY_JNL_SIZE  (Cdp_JOURNAL_HEADER_REGION_SIZE + 3ULL * SECTOR + 16384ULL)

typedef struct _TEST_CTX
{
	PCdp_STORE Source;
	PCdp_STORE Journal;
	PCdp_CORE Core;
} TEST_CTX, *PTEST_CTX;

typedef struct _TEST_DRAIN_WRITER
{
	PCdp_STORE Store;
	UINT64 LastAbsoluteOffset;
	ULONG LastLength;
	ULONG Calls;
	BOOLEAN FailNext;
} TEST_DRAIN_WRITER, *PTEST_DRAIN_WRITER;

static NTSTATUS TestDrainAbsoluteWriter(
	_In_opt_ PVOID Context,
	_In_ UINT64 AbsoluteOffset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	PTEST_DRAIN_WRITER writer = (PTEST_DRAIN_WRITER)Context;
	if (!writer || !writer->Store)
		return STATUS_INVALID_PARAMETER;
	writer->Calls++;
	writer->LastAbsoluteOffset = AbsoluteOffset;
	writer->LastLength = Length;
	if (writer->FailNext)
	{
		writer->FailNext = FALSE;
		return STATUS_IO_DEVICE_ERROR;
	}
	return writer->Store->Write(
		writer->Store, AbsoluteOffset, Length, Buffer);
}

static void FillPattern(PUCHAR buf, ULONG len, UCHAR seed)
{
	ULONG i;
	for (i = 0; i < len; ++i)
		buf[i] = (UCHAR)(seed + i);
}

static UINT64 TestPointerTime100ns(_In_opt_ PVOID Context)
{
	return (UINT64)(ULONG_PTR)Context;
}

static NTSTATUS TestCtxCreateWithSector(
	_Out_ PTEST_CTX Ctx,
	_In_ UINT64 SourceSize,
	_In_ UINT64 JournalSize,
	_In_ UINT64 InitialTime100ns,
	_In_ ULONG SectorSize)
{
	NTSTATUS st;

	RtlZeroMemory(Ctx, sizeof(*Ctx));
	st = CdpMemStoreCreate(SourceSize, SectorSize, &Ctx->Source);
	if (!NT_SUCCESS(st))
		return st;
	st = CdpMemStoreCreate(JournalSize, SectorSize, &Ctx->Journal);
	if (!NT_SUCCESS(st))
	{
		CdpMemStoreDestroy(Ctx->Source);
		Ctx->Source = NULL;
		return st;
	}
	st = CdpCoreCreate(Ctx->Source, Ctx->Journal, &Ctx->Core);
	if (!NT_SUCCESS(st))
	{
		CdpMemStoreDestroy(Ctx->Journal);
		CdpMemStoreDestroy(Ctx->Source);
		Ctx->Journal = NULL;
		Ctx->Source = NULL;
		return st;
	}
	CdpCoreSetTime100ns(Ctx->Core, InitialTime100ns);
	st = CdpCoreFormatJournal(Ctx->Core);
	if (!NT_SUCCESS(st))
	{
		CdpCoreDestroy(Ctx->Core);
		CdpMemStoreDestroy(Ctx->Journal);
		CdpMemStoreDestroy(Ctx->Source);
		Ctx->Core = NULL;
		Ctx->Journal = NULL;
		Ctx->Source = NULL;
	}
	return st;
}

static NTSTATUS TestCtxCreate(
	_Out_ PTEST_CTX Ctx,
	_In_ UINT64 SourceSize,
	_In_ UINT64 JournalSize,
	_In_ UINT64 InitialTime100ns)
{
	return TestCtxCreateWithSector(
		Ctx, SourceSize, JournalSize, InitialTime100ns, SECTOR);
}

typedef struct _TEST_FAIL_STORE
{
	PVOID OriginalContext;
	Cdp_STORE_READ OriginalRead;
	Cdp_STORE_WRITE OriginalWrite;
	LONG FailNextReads;
	ULONG ReadCallCount;
	ULONG ReadLength32Count;
	ULONG MaxReadLength;
	ULONG OversizeReadCount;
	ULONG LargestSuccessfulRead;
	UINT64 TotalReadBytes;
	UINT64 LastReadOffset;
	ULONG LastReadLength;
	LONG FailNextWrites;
	LONG FailNextSuperblockWrites;
	ULONG WriteCallCount;
	ULONG SuperblockWriteCount;
	ULONG MaxWriteLength;
	ULONG OversizeWriteCount;
	ULONG LargestSuccessfulWrite;
	UINT64 WatchedWriteOffset;
	const VOID* WatchedWriteBuffer;
	ULONG WatchedWriteLength;
} TEST_FAIL_STORE, *PTEST_FAIL_STORE;

static NTSTATUS TestFailStoreRead(
	_In_ PCdp_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PTEST_FAIL_STORE fail = (PTEST_FAIL_STORE)Store->Context;
	NTSTATUS status;

	++fail->ReadCallCount;
	if (Length == sizeof(Cdp_JOURNAL_RECORD_HEADER))
		++fail->ReadLength32Count;
	if (fail->FailNextReads > 0)
	{
		--fail->FailNextReads;
		return STATUS_IO_DEVICE_ERROR;
	}
	if (fail->MaxReadLength != 0 && Length > fail->MaxReadLength)
	{
		++fail->OversizeReadCount;
		return STATUS_IO_DEVICE_ERROR;
	}

	Store->Context = fail->OriginalContext;
	status = fail->OriginalRead(Store, Offset, Length, Buffer);
	Store->Context = fail;
	if (NT_SUCCESS(status))
	{
		if (Length > fail->LargestSuccessfulRead)
			fail->LargestSuccessfulRead = Length;
		fail->TotalReadBytes += Length;
		fail->LastReadOffset = Offset;
		fail->LastReadLength = Length;
	}
	return status;
}

static NTSTATUS TestFailStoreWrite(
	_In_ PCdp_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	PTEST_FAIL_STORE fail = (PTEST_FAIL_STORE)Store->Context;
	NTSTATUS status;

	++fail->WriteCallCount;
	if (Offset == fail->WatchedWriteOffset)
	{
		fail->WatchedWriteBuffer = Buffer;
		fail->WatchedWriteLength = Length;
	}
	if (Offset == 0 && Length == Store->SectorSize)
	{
		++fail->SuperblockWriteCount;
		if (fail->FailNextSuperblockWrites > 0)
		{
			--fail->FailNextSuperblockWrites;
			return STATUS_IO_DEVICE_ERROR;
		}
	}
	if (fail->FailNextWrites > 0)
	{
		--fail->FailNextWrites;
		return STATUS_IO_DEVICE_ERROR;
	}
	if (fail->MaxWriteLength != 0 && Length > fail->MaxWriteLength)
	{
		++fail->OversizeWriteCount;
		return STATUS_IO_DEVICE_ERROR;
	}

	Store->Context = fail->OriginalContext;
	status = fail->OriginalWrite(Store, Offset, Length, Buffer);
	Store->Context = fail;
	if (NT_SUCCESS(status) && Length > fail->LargestSuccessfulWrite)
		fail->LargestSuccessfulWrite = Length;
	return status;
}

static VOID TestFailStoreInstall(_Inout_ PCdp_STORE Store, _Out_ PTEST_FAIL_STORE Fail)
{
	Fail->OriginalContext = Store->Context;
	Fail->OriginalRead = Store->Read;
	Fail->OriginalWrite = Store->Write;
	Fail->FailNextReads = 0;
	Fail->ReadCallCount = 0;
	Fail->ReadLength32Count = 0;
	Fail->MaxReadLength = 0;
	Fail->OversizeReadCount = 0;
	Fail->LargestSuccessfulRead = 0;
	Fail->TotalReadBytes = 0;
	Fail->LastReadOffset = 0;
	Fail->LastReadLength = 0;
	Fail->FailNextWrites = 0;
	Fail->FailNextSuperblockWrites = 0;
	Fail->WriteCallCount = 0;
	Fail->SuperblockWriteCount = 0;
	Fail->MaxWriteLength = 0;
	Fail->OversizeWriteCount = 0;
	Fail->LargestSuccessfulWrite = 0;
	Fail->WatchedWriteOffset = ~(UINT64)0;
	Fail->WatchedWriteBuffer = NULL;
	Fail->WatchedWriteLength = 0;
	Store->Context = Fail;
	Store->Read = TestFailStoreRead;
	Store->Write = TestFailStoreWrite;
}

static VOID TestFailStoreRemove(_Inout_ PCdp_STORE Store, _In_ PTEST_FAIL_STORE Fail)
{
	Store->Context = Fail->OriginalContext;
	Store->Read = Fail->OriginalRead;
	Store->Write = Fail->OriginalWrite;
}

static VOID TestCtxDestroy(_Inout_ PTEST_CTX Ctx)
{
	if (Ctx->Core)
	{
		CdpCoreDestroy(Ctx->Core);
		Ctx->Core = NULL;
	}
	if (Ctx->Journal)
	{
		CdpMemStoreDestroy(Ctx->Journal);
		Ctx->Journal = NULL;
	}
	if (Ctx->Source)
	{
		CdpMemStoreDestroy(Ctx->Source);
		Ctx->Source = NULL;
	}
}

static int RunCase(const char* title, int (*fn)(void))
{
	int failed;

	printf("\n=== %s ===\n", title);
	g_caseFailed = 0;
	failed = fn();
	if (failed != g_caseFailed)
		failed = g_caseFailed;
	return failed;
}

static int TestAfterImageInitialBranchRecord(void)
{
	TEST_CTX ctx;
	PCdp_JOURNAL_SUPERBLOCK superblock;
	PCdp_JOURNAL_BRANCH_RECORD_HEADER branch;
	PUCHAR journalBytes;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 90000)),
		"setup after-image initial branch test");
	if (!ctx.Journal)
		return g_caseFailed;

	journalBytes = (PUCHAR)CdpMemStoreData(ctx.Journal);
	superblock = (PCdp_JOURNAL_SUPERBLOCK)journalBytes;
	branch = (PCdp_JOURNAL_BRANCH_RECORD_HEADER)(
		journalBytes + superblock->LastHeaderRegionOff);
	Expect((branch->Sequence & Cdp_JOURNAL_RECORD_FLAG_BRANCH) != 0 &&
		(branch->Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) == 0,
		"first header is branch marker at local index zero");
	Expect(branch->BranchNumber == 1 &&
		branch->ParentBranchNumber == 0 &&
		branch->InheritedRecordSequence == 0 &&
		branch->Reserved == 0,
		"initial branch record stores branch 1 with no parent");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestJournalPhysicalLayoutPersistence(void)
{
	PCdp_STORE store = NULL;
	Cdp_JOURNAL formatted;
	Cdp_JOURNAL mounted;
	PCdp_JOURNAL_SUPERBLOCK superblock;
	GUID sourceGuid =
		{ 0x10203040, 0x5060, 0x7080,
		  { 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x01 } };
	GUID diskGuid =
		{ 0x89ABCDEF, 0x1234, 0x5678,
		  { 0x9A, 0xBC, 0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44 } };
	const ULONG gptStyle = 1;
	const UINT64 sourceStart = 1ULL * 1024 * 1024;
	const UINT64 sourceSize = 32ULL * 1024 * 1024;
	const UINT64 journalStart = sourceStart + sourceSize;

	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &store)),
		"create journal for physical-layout persistence test");
	if (!store)
		return g_caseFailed;

	CdpJournalInitializeWithStore(
		&formatted, store, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)90500);
	CdpJournalSetPhysicalLayout(
		&formatted,
		gptStyle,
		0,
		&diskGuid,
		sourceStart,
		sourceSize,
		journalStart,
		JNL_SIZE);
	Expect(NT_SUCCESS(CdpJournalFormat(&formatted)),
		"format journal with stable physical identity");
	superblock = (PCdp_JOURNAL_SUPERBLOCK)CdpMemStoreData(store);
	Expect(superblock->Version == Cdp_JOURNAL_VERSION &&
		superblock->DiskPartitionStyle == gptStyle &&
		memcmp(&superblock->DiskGuid, &diskGuid, sizeof(GUID)) == 0 &&
		superblock->SourcePartitionStart == sourceStart &&
		superblock->SourcePartitionSize == sourceSize &&
		superblock->JournalPartitionStart == journalStart &&
		superblock->JournalPartitionSize == JNL_SIZE,
		"superblock stores complete source/journal physical layout");
	CdpJournalClose(&formatted);

	CdpJournalInitializeWithStore(
		&mounted, store, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)90600);
	Expect(NT_SUCCESS(CdpJournalMount(&mounted)),
		"remount journal with physical identity metadata");
	Expect(mounted.DiskPartitionStyle == gptStyle &&
		memcmp(&mounted.DiskGuid, &diskGuid, sizeof(GUID)) == 0 &&
		mounted.SourcePartitionStart == sourceStart &&
		mounted.SourcePartitionSize == sourceSize &&
		mounted.JournalPartitionStart == journalStart &&
		mounted.JournalPartitionSize == JNL_SIZE,
		"mount restores complete physical identity metadata");

	CdpJournalClose(&mounted);
	CdpMemStoreDestroy(store);
	return g_caseFailed;
}

static int TestAfterImageAppendDoesNotTouchSource(void)
{
	TEST_CTX ctx;
	TEST_FAIL_STORE sourceTrace;
	Cdp_JOURNAL_RECORD written;
	PCdp_JOURNAL_SUPERBLOCK superblock;
	PCdp_JOURNAL_RECORD_HEADER header;
	UCHAR sourceBefore[512];
	UCHAR afterImage[512];
	PUCHAR journalBytes;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 91000)),
		"setup single after-image append test");
	FillPattern(sourceBefore, sizeof(sourceBefore), 0x21);
	FillPattern(afterImage, sizeof(afterImage), 0xA4);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 4096, sizeof(sourceBefore), sourceBefore)),
		"seed source before after-image append");

	TestFailStoreInstall(ctx.Source, &sourceTrace);
	status = CdpCoreAppendAfterImage(
		ctx.Core, 4096, sizeof(afterImage), afterImage, &written);
	Expect(NT_SUCCESS(status), "append one after-image record");
	Expect(sourceTrace.ReadCallCount == 0 &&
		sourceTrace.WriteCallCount == 0,
		"after-image append performs no source read or source write");
	TestFailStoreRemove(ctx.Source, &sourceTrace);

	Expect(memcmp(
		(PUCHAR)CdpMemStoreData(ctx.Source) + 4096,
		sourceBefore,
		sizeof(sourceBefore)) == 0,
		"source bytes remain unchanged after journal append");
	journalBytes = (PUCHAR)CdpMemStoreData(ctx.Journal);
	superblock = (PCdp_JOURNAL_SUPERBLOCK)journalBytes;
	header = (PCdp_JOURNAL_RECORD_HEADER)(
		journalBytes + superblock->LastHeaderRegionOff +
		sizeof(Cdp_JOURNAL_RECORD_HEADER));
	Expect((header->Sequence & Cdp_JOURNAL_RECORD_FLAG_BRANCH) == 0 &&
		(header->Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) == 1 &&
		header->VolumeOffset == 4096 &&
		header->DataLength == sizeof(afterImage),
		"normal record follows initial branch marker");
	Expect(written.Sequence == 2 &&
		written.FileOffset == header->FileOffset &&
		memcmp(journalBytes + header->FileOffset,
			afterImage, sizeof(afterImage)) == 0,
		"record payload contains the application after-image");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestAfterImageJournalFailureDoesNotBypassSource(void)
{
	TEST_CTX ctx;
	TEST_FAIL_STORE sourceTrace;
	TEST_FAIL_STORE journalFail;
	UCHAR sourceBefore[512];
	UCHAR afterImage[512];
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 92000)),
		"setup after-image failure test");
	FillPattern(sourceBefore, sizeof(sourceBefore), 0x32);
	FillPattern(afterImage, sizeof(afterImage), 0xB5);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 8192, sizeof(sourceBefore), sourceBefore)),
		"seed source before failed journal append");

	TestFailStoreInstall(ctx.Source, &sourceTrace);
	TestFailStoreInstall(ctx.Journal, &journalFail);
	journalFail.FailNextWrites = 1;
	status = CdpCoreAppendAfterImage(
		ctx.Core, 8192, sizeof(afterImage), afterImage, NULL);
	Expect(status == STATUS_IO_DEVICE_ERROR,
		"after-image append reports journal write failure");
	Expect(sourceTrace.ReadCallCount == 0 &&
		sourceTrace.WriteCallCount == 0,
		"journal failure does not fall back to the source volume");
	TestFailStoreRemove(ctx.Journal, &journalFail);
	TestFailStoreRemove(ctx.Source, &sourceTrace);
	Expect(memcmp(
		(PUCHAR)CdpMemStoreData(ctx.Source) + 8192,
		sourceBefore,
		sizeof(sourceBefore)) == 0,
		"source bytes remain unchanged after journal failure");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestAfterImagePayloadZeroCopyAndFallback(void)
{
	PCdp_STORE store = NULL;
	Cdp_JOURNAL journal;
	TEST_FAIL_STORE trace;
	GUID sourceGuid = { 0 };
	PUCHAR alignedInput = NULL;
	PUCHAR journalBytes;
	UINT64 payloadOffset;
	ULONG i;
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &store)),
		"create journal for payload zero-copy test");
	if (!store)
		return g_caseFailed;
	alignedInput = (PUCHAR)_aligned_malloc(1024, SECTOR);
	Expect(alignedInput != NULL,
		"allocate sector-aligned application buffer");
	if (!alignedInput)
	{
		CdpMemStoreDestroy(store);
		return g_caseFailed;
	}
	FillPattern(alignedInput, 1024, 0x6D);
	CdpJournalInitializeWithStore(
		&journal, store, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)92400);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)),
		"format journal for payload zero-copy test");

	TestFailStoreInstall(store, &trace);
	payloadOffset = journal.PayloadRegionOff;
	trace.WatchedWriteOffset = payloadOffset;
	status = CdpJournalAppend(
		&journal, 0, SECTOR, alignedInput, NULL);
	Expect(NT_SUCCESS(status), "append aligned after-image payload");
	Expect(trace.WatchedWriteBuffer == alignedInput &&
		trace.WatchedWriteLength == SECTOR,
		"aligned payload is written directly without a copy buffer");

	payloadOffset = journal.PayloadRegionOff;
	trace.WatchedWriteOffset = payloadOffset;
	trace.WatchedWriteBuffer = NULL;
	trace.WatchedWriteLength = 0;
	status = CdpJournalAppend(
		&journal, SECTOR, SECTOR + 1, alignedInput, NULL);
	Expect(NT_SUCCESS(status), "append partial-sector after-image payload");
	Expect(trace.WatchedWriteBuffer != NULL &&
		trace.WatchedWriteBuffer != alignedInput &&
		trace.WatchedWriteLength == 2 * SECTOR,
		"partial-sector payload uses the aligned fallback buffer");
	TestFailStoreRemove(store, &trace);
	journalBytes = (PUCHAR)CdpMemStoreData(store);
	Expect(memcmp(journalBytes + payloadOffset,
		alignedInput, SECTOR + 1) == 0,
		"fallback payload preserves all valid input bytes");
	for (i = SECTOR + 1; i < 2 * SECTOR; ++i)
	{
		if (journalBytes[payloadOffset + i] != 0)
			break;
	}
	Expect(i == 2 * SECTOR,
		"fallback payload zero-fills the final partial sector");

	CdpJournalClose(&journal);
	_aligned_free(alignedInput);
	CdpMemStoreDestroy(store);
	return g_caseFailed;
}

static int TestRecordHeaderWriteReusesSectorCache(void)
{
	PCdp_STORE store = NULL;
	Cdp_JOURNAL formatted;
	Cdp_JOURNAL mounted;
	TEST_FAIL_STORE trace;
	GUID sourceGuid = { 0 };
	UCHAR payload[512];
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &store)),
		"create journal for record-header cache test");
	if (!store)
		return g_caseFailed;
	CdpJournalInitializeWithStore(
		&formatted, store, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)92500);
	Expect(NT_SUCCESS(CdpJournalFormat(&formatted)),
		"format journal for record-header cache test");
	CdpJournalClose(&formatted);

	CdpJournalInitializeWithStore(
		&mounted, store, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)92600);
	Expect(NT_SUCCESS(CdpJournalMount(&mounted)),
		"mount journal with an empty record-header write cache");
	FillPattern(payload, sizeof(payload), 0x4D);
	TestFailStoreInstall(store, &trace);
	status = CdpJournalAppend(
		&mounted, 0, sizeof(payload), payload, NULL);
	Expect(NT_SUCCESS(status), "append first cached-header record");
	status = CdpJournalAppend(
		&mounted, sizeof(payload), sizeof(payload), payload, NULL);
	Expect(NT_SUCCESS(status), "append second cached-header record");
	Expect(trace.ReadCallCount == 1 && trace.LastReadLength == SECTOR,
		"two headers in one sector require only one sector read");
	TestFailStoreRemove(store, &trace);

	CdpJournalClose(&mounted);
	CdpMemStoreDestroy(store);
	return g_caseFailed;
}

static int TestAfterImageBranchNumbersIncrease(void)
{
	PCdp_STORE store = NULL;
	Cdp_JOURNAL journal;
	GUID sourceGuid = { 0 };
	PCdp_JOURNAL_BRANCH_RECORD_HEADER branch;
	UINT64 parentRegion;
	PUCHAR bytes;
	UCHAR payload[512];
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &store)),
		"create journal for branch increment test");
	if (!store)
		return g_caseFailed;
	CdpJournalInitializeWithStore(
		&journal, store, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)93000);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)),
		"format journal with branch 1");
	FillPattern(payload, sizeof(payload), 0xC6);
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 0, sizeof(payload), payload, NULL)),
		"append parent branch data record");
	parentRegion = journal.LastHeaderRegionOff;
	status = CdpJournalAppendBranch(&journal, 2, 1, 2);
	Expect(NT_SUCCESS(status), "append monotonically increasing branch 2");
	Expect(journal.CurrentBranchNumber == 2 &&
		journal.HighestBranchNumber == 2 &&
		journal.LastHeaderRegionOff != parentRegion &&
		journal.CurrentHeaderRegionStartSequence == 3,
		"new branch starts a new region without resetting global Sequence");

	bytes = (PUCHAR)CdpMemStoreData(store);
	branch = (PCdp_JOURNAL_BRANCH_RECORD_HEADER)(
		bytes + journal.LastHeaderRegionOff);
	Expect((branch->Sequence & Cdp_JOURNAL_RECORD_FLAG_BRANCH) != 0 &&
		(branch->Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) == 0 &&
		branch->BranchNumber == 2 &&
		branch->ParentBranchNumber == 1 &&
		branch->InheritedRecordSequence == 2 &&
		branch->Reserved == 0,
		"branch 2 marker stores parent and inherited record sequence");
	ExpectStatus(
		CdpJournalAppendBranch(&journal, 4, 2, 2),
		STATUS_INVALID_PARAMETER,
		"branch numbers cannot skip the next value");
	ExpectStatus(
		CdpJournalAppendBranch(&journal, 2, 1, 2),
		STATUS_INVALID_PARAMETER,
		"branch numbers cannot be reused");

	CdpJournalClose(&journal);
	CdpMemStoreDestroy(store);
	return g_caseFailed;
}

static int TestAfterImageMixedReadAndLatestOverlap(void)
{
	TEST_CTX ctx;
	UCHAR source[2048];
	UCHAR first[1024];
	UCHAR latest[512];
	UCHAR expected[2048];
	UCHAR output[2048];
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 94000)),
		"setup mixed after-image read test");
	FillPattern(source, sizeof(source), 0x10);
	RtlFillMemory(first, sizeof(first), 0xA1);
	RtlFillMemory(latest, sizeof(latest), 0xB2);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(source), source)),
		"seed source for mixed read");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 512, sizeof(first), first, NULL)),
		"append first current-branch range");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 1024, sizeof(latest), latest, NULL)),
		"append latest overlapping current-branch range");

	RtlCopyMemory(expected, source, sizeof(expected));
	RtlCopyMemory(expected + 512, first, sizeof(first));
	RtlCopyMemory(expected + 1024, latest, sizeof(latest));
	RtlZeroMemory(output, sizeof(output));
	status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"read combines source gaps with latest journal coverage");

	CdpCoreDestroy(ctx.Core);
	ctx.Core = NULL;
	status = CdpCoreCreate(ctx.Source, ctx.Journal, &ctx.Core);
	Expect(NT_SUCCESS(status), "recreate core for meta-tree mount scan");
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(ctx.Core);
	Expect(NT_SUCCESS(status), "mount rebuilds current-branch meta tree");
	RtlZeroMemory(output, sizeof(output));
	if (NT_SUCCESS(status))
		status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"remounted meta tree returns the same mixed view");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestAfterImageAllOverlapShapes(void)
{
	TEST_CTX ctx;
	Cdp_JOURNAL_RECORD latestRecord;
	UCHAR source[16384];
	UCHAR expected[16384];
	UCHAR output[16384];
	UCHAR oldLeft[1024];
	UCHAR newLeft[1024];
	UCHAR oldRight[1024];
	UCHAR newRight[1024];
	UCHAR oldContained[512];
	UCHAR newContains[1536];
	UCHAR oldSplit[1536];
	UCHAR newMiddle[512];
	Cdp_CORE_READ_COVERAGE coverage = Cdp_CORE_READ_COVERAGE_NONE;
	UINT64 sourceOffset = 0;
	ULONG sourceLength = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 94500)),
		"setup all-overlap-shapes test");
	FillPattern(source, sizeof(source), 0x09);
	RtlCopyMemory(expected, source, sizeof(expected));
	RtlFillMemory(oldLeft, sizeof(oldLeft), 0x11);
	RtlFillMemory(newLeft, sizeof(newLeft), 0x21);
	RtlFillMemory(oldRight, sizeof(oldRight), 0x32);
	RtlFillMemory(newRight, sizeof(newRight), 0x42);
	RtlFillMemory(oldContained, sizeof(oldContained), 0x53);
	RtlFillMemory(newContains, sizeof(newContains), 0x63);
	RtlFillMemory(oldSplit, sizeof(oldSplit), 0x74);
	RtlFillMemory(newMiddle, sizeof(newMiddle), 0x84);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(source), source)),
		"seed source for all overlap shapes");

	// Left overlap: newer [0, 1024) replaces the left side of older
	// [512, 1536), leaving only the older right fragment.
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 512, sizeof(oldLeft), oldLeft, NULL)),
		"append old range for left overlap");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(newLeft), newLeft, NULL)),
		"append newer range overlapping from the left");
	RtlCopyMemory(expected + 512, oldLeft, sizeof(oldLeft));
	RtlCopyMemory(expected, newLeft, sizeof(newLeft));

	// Right overlap: newer [4608, 5632) replaces the right side of older
	// [4096, 5120), leaving only the older left fragment.
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 4096, sizeof(oldRight), oldRight, NULL)),
		"append old range for right overlap");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 4608, sizeof(newRight), newRight, NULL)),
		"append newer range overlapping from the right");
	RtlCopyMemory(expected + 4096, oldRight, sizeof(oldRight));
	RtlCopyMemory(expected + 4608, newRight, sizeof(newRight));

	// Contains: newer [8192, 9728) completely covers older [8704, 9216).
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 8704, sizeof(oldContained), oldContained, NULL)),
		"append old range that will be fully contained");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 8192, sizeof(newContains), newContains, NULL)),
		"append newer range containing the old range");
	RtlCopyMemory(expected + 8704, oldContained, sizeof(oldContained));
	RtlCopyMemory(expected + 8192, newContains, sizeof(newContains));

	// Middle overlap: newer [12800, 13312) splits older
	// [12288, 13824) into independently readable left and right fragments.
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 12288, sizeof(oldSplit), oldSplit, NULL)),
		"append old range for middle split");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 12800, sizeof(newMiddle), newMiddle, &latestRecord)),
		"append newer middle range that splits the old range");
	RtlCopyMemory(expected + 12288, oldSplit, sizeof(oldSplit));
	RtlCopyMemory(expected + 12800, newMiddle, sizeof(newMiddle));

	status = CdpCoreQueryCurrentReadCoverage(
		ctx.Core, 2048, 512, &coverage, &sourceOffset, &sourceLength);
	Expect(NT_SUCCESS(status) &&
		coverage == Cdp_CORE_READ_COVERAGE_NONE &&
		sourceOffset == 2048 && sourceLength == 512,
		"pure memory coverage query reports a MetaTree gap");
	status = CdpCoreQueryCurrentReadCoverage(
		ctx.Core, 1535, 2, &coverage, &sourceOffset, &sourceLength);
	Expect(NT_SUCCESS(status) &&
		coverage == Cdp_CORE_READ_COVERAGE_PARTIAL &&
		sourceOffset == 1536 && sourceLength == 1,
		"pure memory coverage query detects a one-byte overlap");
	status = CdpCoreQueryCurrentReadCoverage(
		ctx.Core, 0, 1536, &coverage, &sourceOffset, &sourceLength);
	Expect(NT_SUCCESS(status) &&
		coverage == Cdp_CORE_READ_COVERAGE_FULL && sourceLength == 0,
		"full MetaTree coverage requires no source read");
	status = CdpCoreQueryCurrentReadCoverage(
		ctx.Core, 0, 2048, &coverage, &sourceOffset, &sourceLength);
	Expect(NT_SUCCESS(status) &&
		coverage == Cdp_CORE_READ_COVERAGE_PARTIAL &&
		sourceOffset == 1536 && sourceLength == 512,
		"left-side coverage trims the beginning of the source read");
	status = CdpCoreQueryCurrentReadCoverage(
		ctx.Core, 3584, 2048, &coverage, &sourceOffset, &sourceLength);
	Expect(NT_SUCCESS(status) &&
		coverage == Cdp_CORE_READ_COVERAGE_PARTIAL &&
		sourceOffset == 3584 && sourceLength == 512,
		"right-side coverage trims the end of the source read");
	status = CdpCoreQueryCurrentReadCoverage(
		ctx.Core, 0, 14336, &coverage, &sourceOffset, &sourceLength);
	Expect(NT_SUCCESS(status) &&
		coverage == Cdp_CORE_READ_COVERAGE_PARTIAL &&
		sourceOffset == 0 && sourceLength == 14336,
		"multiple source holes use one full original-range source read");

	RtlZeroMemory(output, sizeof(output));
	status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"live MetaTree read resolves all four overlap shapes and source gaps");

	/* Disk Upper now obtains the baseline from the original lower read and
	 * asks Core to replace only journal-covered bytes. */
	RtlCopyMemory(output, source, sizeof(output));
	status = CdpCoreOverlayCurrentRead(
		ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"overlay-only read preserves source gaps and replaces MetaTree hits");

	// Each read crosses a fragment boundary, so correctness cannot be achieved
	// by merely selecting one covering node for the request.
	RtlZeroMemory(output, 2048);
	status = CdpCoreRead(ctx.Core, 768, 512, output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected + 768, 512) == 0,
		"read across left-overlap new-to-old boundary");
	status = CdpCoreRead(ctx.Core, 4352, 512, output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected + 4352, 512) == 0,
		"read across right-overlap old-to-new boundary");
	status = CdpCoreRead(ctx.Core, 7936, 2048, output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected + 7936, 2048) == 0,
		"read across both edges of a containing record");
	status = CdpCoreRead(ctx.Core, 12544, 1024, output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected + 12544, 1024) == 0,
		"read across old-left, new-middle and old-right fragments");

	status = CdpCorePreviewBegin(
		ctx.Core, latestRecord.WallClock100ns);
	Expect(NT_SUCCESS(status),
		"build PreviewTree containing all overlap shapes");
	RtlZeroMemory(output, sizeof(output));
	if (NT_SUCCESS(status))
		status = CdpCorePreviewRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"PreviewTree read resolves all four overlap shapes");
	Expect(NT_SUCCESS(CdpCorePreviewEnd(ctx.Core)),
		"end all-overlap-shapes preview");

	status = CdpCoreRecoveryBegin(
		ctx.Core, latestRecord.WallClock100ns);
	Expect(NT_SUCCESS(status),
		"recovery rebuilds MetaTree containing all overlap shapes");
	RtlZeroMemory(output, sizeof(output));
	if (NT_SUCCESS(status))
		status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"recovery replacement MetaTree preserves all overlap results");

	CdpCoreDestroy(ctx.Core);
	ctx.Core = NULL;
	status = CdpCoreCreate(ctx.Source, ctx.Journal, &ctx.Core);
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(ctx.Core);
	Expect(NT_SUCCESS(status),
		"remount rebuilds MetaTree after all overlap shapes");
	RtlZeroMemory(output, sizeof(output));
	if (NT_SUCCESS(status))
		status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"remounted MetaTree preserves all overlap results");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

#define TEST_TREE_MODEL_SIZE 8192UL

typedef struct _TEST_TREE_STATS
{
	ULONG Count;
	LONG Height;
	UINT64 MaxEnd;
	UINT64 MinValidSequence;
} TEST_TREE_STATS, *PTEST_TREE_STATS;

static ULONG TestRandomNext(_Inout_ PULONG State)
{
	ULONG value = *State;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*State = value;
	return value;
}

static BOOLEAN TestValidateTreeNode(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_Inout_ PUINT64 PreviousEnd,
	_Inout_ PBOOLEAN HavePrevious,
	_Out_ PTEST_TREE_STATS Stats)
{
	TEST_TREE_STATS left;
	TEST_TREE_STATS right;
	UINT64 expectedMaxEnd;
	UINT64 expectedMinSequence;
	LONG expectedHeight;

	RtlZeroMemory(Stats, sizeof(*Stats));
	Stats->MinValidSequence = MAXUINT64;
	if (!Node)
		return TRUE;

	if (!TestValidateTreeNode(
		Node->Left, PreviousEnd, HavePrevious, &left))
	{
		return FALSE;
	}
	if (Node->Start >= Node->End ||
		Node->End - Node->Start != Node->DataLength ||
		(*HavePrevious && Node->Start < *PreviousEnd))
	{
		return FALSE;
	}
	*PreviousEnd = Node->End;
	*HavePrevious = TRUE;
	if (!TestValidateTreeNode(
		Node->Right, PreviousEnd, HavePrevious, &right))
	{
		return FALSE;
	}

	expectedHeight = 1 +
		(left.Height > right.Height ? left.Height : right.Height);
	expectedMaxEnd = Node->End;
	if (left.MaxEnd > expectedMaxEnd)
		expectedMaxEnd = left.MaxEnd;
	if (right.MaxEnd > expectedMaxEnd)
		expectedMaxEnd = right.MaxEnd;
	expectedMinSequence = Node->Invalid ? MAXUINT64 : Node->Sequence;
	if (left.MinValidSequence < expectedMinSequence)
		expectedMinSequence = left.MinValidSequence;
	if (right.MinValidSequence < expectedMinSequence)
		expectedMinSequence = right.MinValidSequence;
	if (Node->Height != expectedHeight ||
		Node->MaxEnd != expectedMaxEnd ||
		Node->MinValidSequence != expectedMinSequence ||
		left.Height - right.Height > 1 ||
		right.Height - left.Height > 1)
	{
		return FALSE;
	}

	Stats->Count = left.Count + right.Count + 1;
	Stats->Height = expectedHeight;
	Stats->MaxEnd = expectedMaxEnd;
	Stats->MinValidSequence = expectedMinSequence;
	return TRUE;
}

static PCdp_PREVIEW_TREE_NODE TestTreeFindByte(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 Offset)
{
	while (Node)
	{
		if (Offset < Node->Start)
			Node = Node->Left;
		else if (Offset >= Node->End)
			Node = Node->Right;
		else
			return Node;
	}
	return NULL;
}

static BOOLEAN TestValidateTreeAgainstModel(
	_In_ PCdp_PREVIEW_TREE Tree,
	_In_reads_(ModelSize) const UCHAR* Covered,
	_In_reads_(ModelSize) const UINT64* Sequences,
	_In_reads_(ModelSize) const UINT64* PayloadOffsets,
	_In_ ULONG ModelSize)
{
	TEST_TREE_STATS stats;
	UINT64 previousEnd = 0;
	BOOLEAN havePrevious = FALSE;
	ULONG i;

	if (!TestValidateTreeNode(
		Tree->Root, &previousEnd, &havePrevious, &stats) ||
		stats.Count != Tree->NodeCount)
	{
		return FALSE;
	}

	for (i = 0; i < ModelSize; ++i)
	{
		PCdp_PREVIEW_TREE_NODE node =
			TestTreeFindByte(Tree->Root, i);
		if (!Covered[i])
		{
			if (node)
				return FALSE;
			continue;
		}
		if (!node || node->Invalid ||
			node->Sequence != Sequences[i] ||
			node->FileOffset + (i - node->Start) != PayloadOffsets[i])
		{
			return FALSE;
		}
	}
	return TRUE;
}

static NTSTATUS TestOverlayTreeAndModel(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_Inout_updates_(ModelSize) PUCHAR Covered,
	_Inout_updates_(ModelSize) PUINT64 Sequences,
	_Inout_updates_(ModelSize) PUINT64 PayloadOffsets,
	_In_ ULONG ModelSize,
	_In_ ULONG Start,
	_In_ ULONG Length,
	_In_ UINT64 Sequence,
	_In_ UINT64 FileOffset)
{
	Cdp_JOURNAL_RECORD record;
	NTSTATUS status;
	ULONG i;

	if (Length == 0 || Start > ModelSize || Length > ModelSize - Start)
		return STATUS_INVALID_PARAMETER;
	RtlZeroMemory(&record, sizeof(record));
	record.VolumeOffset = Start;
	record.DataLength = Length;
	record.Sequence = Sequence;
	record.FileOffset = FileOffset;
	record.WallClock100ns = Sequence;
	status = CdpPreviewTreeOverlayLatest(Tree, &record);
	if (!NT_SUCCESS(status))
		return status;
	for (i = 0; i < Length; ++i)
	{
		Covered[Start + i] = TRUE;
		Sequences[Start + i] = Sequence;
		PayloadOffsets[Start + i] = FileOffset + i;
	}
	return STATUS_SUCCESS;
}

static int TestMetaTreeCompoundOverlapModel(void)
{
	static const struct
	{
		ULONG Start;
		ULONG Length;
	} operations[] =
	{
		{ 512, 3072 },
		{ 1024, 1024 },
		{ 1536, 1536 },
		{ 768, 512 },
		{ 768, 256 },
		{ 3072, 512 },
		{ 1792, 768 },
		{ 0, 512 },
		{ 3584, 512 },
		{ 256, 3584 },
		{ 4096, 2048 },
		{ 5632, 1536 },
		{ 4864, 1024 },
		{ 0, TEST_TREE_MODEL_SIZE }
	};
	Cdp_PREVIEW_TREE tree;
	UCHAR covered[TEST_TREE_MODEL_SIZE];
	UINT64 sequences[TEST_TREE_MODEL_SIZE];
	UINT64 payloadOffsets[TEST_TREE_MODEL_SIZE];
	BOOLEAN valid = TRUE;
	ULONG i;

	CdpPreviewTreeInitialize(&tree);
	RtlZeroMemory(covered, sizeof(covered));
	RtlZeroMemory(sequences, sizeof(sequences));
	RtlZeroMemory(payloadOffsets, sizeof(payloadOffsets));
	for (i = 0; i < ARRAYSIZE(operations); ++i)
	{
		UINT64 sequence = i + 1;
		UINT64 fileOffset = 0x100000000ULL + sequence * 0x10000ULL;

		if (!NT_SUCCESS(TestOverlayTreeAndModel(
			&tree, covered, sequences, payloadOffsets,
			ARRAYSIZE(covered), operations[i].Start,
			operations[i].Length, sequence, fileOffset)) ||
			!TestValidateTreeAgainstModel(
				&tree, covered, sequences, payloadOffsets,
				ARRAYSIZE(covered)))
		{
			valid = FALSE;
			break;
		}
	}
	Expect(valid,
		"compound overlays preserve interval, payload-offset and AVL invariants");
	CdpPreviewTreeFree(&tree);
	return g_caseFailed;
}

static int TestMetaTreeRandomizedReferenceModel(void)
{
	Cdp_PREVIEW_TREE tree;
	UCHAR covered[TEST_TREE_MODEL_SIZE];
	UINT64 sequences[TEST_TREE_MODEL_SIZE];
	UINT64 payloadOffsets[TEST_TREE_MODEL_SIZE];
	ULONG randomState = 0x4d595df4UL;
	BOOLEAN valid = TRUE;
	ULONG iteration;

	CdpPreviewTreeInitialize(&tree);
	RtlZeroMemory(covered, sizeof(covered));
	RtlZeroMemory(sequences, sizeof(sequences));
	RtlZeroMemory(payloadOffsets, sizeof(payloadOffsets));
	for (iteration = 0; iteration < 6000; ++iteration)
	{
		ULONG start = TestRandomNext(&randomState) % TEST_TREE_MODEL_SIZE;
		ULONG maxLength = TEST_TREE_MODEL_SIZE - start;
		ULONG length = 1 + TestRandomNext(&randomState) %
			(maxLength < 1536 ? maxLength : 1536);
		UINT64 sequence = iteration + 1;
		UINT64 fileOffset = 0x200000000ULL + sequence * 0x2000ULL;

		if (!NT_SUCCESS(TestOverlayTreeAndModel(
			&tree, covered, sequences, payloadOffsets,
			ARRAYSIZE(covered), start, length, sequence, fileOffset)))
		{
			valid = FALSE;
			break;
		}
		if ((iteration % 20) == 0 &&
			!TestValidateTreeAgainstModel(
				&tree, covered, sequences, payloadOffsets,
				ARRAYSIZE(covered)))
		{
			valid = FALSE;
			break;
		}
	}
	if (valid)
	{
		valid = TestValidateTreeAgainstModel(
			&tree, covered, sequences, payloadOffsets,
			ARRAYSIZE(covered));
	}
	Expect(valid,
		"6000 randomized overlays match byte reference model and tree invariants");
	CdpPreviewTreeFree(&tree);
	return g_caseFailed;
}

static int TestMetaTreeRandomizedWriteReadRemount(void)
{
	TEST_CTX ctx;
	UCHAR source[SRC_SIZE];
	UCHAR expected[SRC_SIZE];
	UCHAR output[SRC_SIZE];
	UCHAR payload[4096];
	ULONG randomState = 0x9e3779b9UL;
	BOOLEAN valid = TRUE;
	const char* failStage = NULL;
	ULONG failIteration = 0;
	NTSTATUS status;
	ULONG iteration;

	status = TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 130000);
	Expect(NT_SUCCESS(status), "setup randomized write/read/remount test");
	if (!NT_SUCCESS(status))
		return g_caseFailed;
	FillPattern(source, sizeof(source), 0x37);
	RtlCopyMemory(expected, source, sizeof(expected));
	status = ctx.Source->Write(ctx.Source, 0, sizeof(source), source);
	if (!NT_SUCCESS(status))
	{
		valid = FALSE;
		failStage = "source seed";
	}

	for (iteration = 0; valid && iteration < 1000; ++iteration)
	{
		ULONG start = (TestRandomNext(&randomState) %
			((ULONG)SRC_SIZE / SECTOR)) * SECTOR;
		ULONG maxSectors = ((ULONG)SRC_SIZE - start) / SECTOR;
		ULONG length = (1 + TestRandomNext(&randomState) %
			(maxSectors < ARRAYSIZE(payload) / SECTOR ?
				maxSectors : ARRAYSIZE(payload) / SECTOR)) * SECTOR;
		ULONG i;

		for (i = 0; i < length; ++i)
			payload[i] = (UCHAR)(iteration * 29 + i * 17);
		status = CdpCoreAppendAfterImage(
			ctx.Core, start, length, payload, NULL);
		if (!NT_SUCCESS(status))
		{
			valid = FALSE;
			failStage = "append";
			failIteration = iteration;
			break;
		}
		RtlCopyMemory(expected + start, payload, length);

		if ((iteration % 25) == 24)
		{
			ULONG readStart = (TestRandomNext(&randomState) %
				((ULONG)SRC_SIZE / SECTOR)) * SECTOR;
			ULONG readLength = (1 + TestRandomNext(&randomState) %
				(((ULONG)SRC_SIZE - readStart) / SECTOR)) * SECTOR;

			status = CdpCoreRead(
				ctx.Core, readStart, readLength, output);
			if (!NT_SUCCESS(status) ||
				memcmp(output, expected + readStart, readLength) != 0)
			{
				valid = FALSE;
				failStage = "live partial read";
				failIteration = iteration;
				break;
			}
		}
		if ((iteration % 100) == 99)
		{
			CdpCoreDestroy(ctx.Core);
			ctx.Core = NULL;
			status = CdpCoreCreate(ctx.Source, ctx.Journal, &ctx.Core);
			if (NT_SUCCESS(status))
				status = CdpCoreMountJournal(ctx.Core);
			if (NT_SUCCESS(status))
				status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
			if (!NT_SUCCESS(status) ||
				memcmp(output, expected, sizeof(output)) != 0)
			{
				valid = FALSE;
				failStage = "remount full read";
				failIteration = iteration;
				break;
			}
		}
	}
	if (!valid)
	{
		printf("MetaTree randomized failure: stage=%s iteration=%lu status=0x%08lx\n",
			failStage ? failStage : "unknown", failIteration, (ULONG)status);
	}
	Expect(valid,
		"1000 writes stay correct during live reads and ten MetaTree rebuilds");
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestAfterImageBranchAncestryRead(void)
{
	PCdp_STORE sourceStore = NULL;
	PCdp_STORE journalStore = NULL;
	PCdp_CORE core = NULL;
	Cdp_JOURNAL journal;
	GUID sourceGuid = { 0 };
	UCHAR source[1536];
	UCHAR branch1Kept[512];
	UCHAR branch1Excluded[512];
	UCHAR sibling[512];
	UCHAR current[512];
	UCHAR expected[1536];
	UCHAR output[1536];
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(SRC_SIZE, SECTOR, &sourceStore)),
		"create source for branch ancestry read");
	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &journalStore)),
		"create journal for branch ancestry read");
	if (!sourceStore || !journalStore)
		goto cleanup;
	FillPattern(source, sizeof(source), 0x20);
	RtlFillMemory(branch1Kept, sizeof(branch1Kept), 0x31);
	RtlFillMemory(branch1Excluded, sizeof(branch1Excluded), 0x42);
	RtlFillMemory(sibling, sizeof(sibling), 0x53);
	RtlFillMemory(current, sizeof(current), 0x64);
	Expect(NT_SUCCESS(sourceStore->Write(
		sourceStore, 0, sizeof(source), source)),
		"seed branch ancestry source");

	CdpJournalInitializeWithStore(
		&journal, journalStore, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)95000);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)), "format branch journal");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 0, sizeof(branch1Kept), branch1Kept, NULL)),
		"append inherited branch-1 record sequence 2");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, sizeof(branch1Excluded), branch1Excluded, NULL)),
		"append non-inherited branch-1 record sequence 3");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(&journal, 2, 1, 2)),
		"create sibling branch 2 from sequence 2");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, sizeof(sibling), sibling, NULL)),
		"append sibling-only record");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(&journal, 3, 1, 2)),
		"create current branch 3 from branch 1 sequence 2");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 1024, sizeof(current), current, NULL)),
		"append current-branch record");
	CdpJournalClose(&journal);

	status = CdpCoreCreate(sourceStore, journalStore, &core);
	Expect(NT_SUCCESS(status), "create core for current branch view");
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(core);
	Expect(NT_SUCCESS(status), "mount follows current branch ancestry");
	RtlCopyMemory(expected, branch1Kept, sizeof(branch1Kept));
	RtlCopyMemory(expected + 512, source + 512, 512);
	RtlCopyMemory(expected + 1024, current, sizeof(current));
	RtlZeroMemory(output, sizeof(output));
	if (NT_SUCCESS(status))
		status = CdpCoreRead(core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"current view keeps inherited data and discards excluded/sibling records");

cleanup:
	if (core)
		CdpCoreDestroy(core);
	if (journalStore)
		CdpMemStoreDestroy(journalStore);
	if (sourceStore)
		CdpMemStoreDestroy(sourceStore);
	return g_caseFailed;
}

static int TestAfterImageCompactsOnlyCurrentBranch(void)
{
	PCdp_STORE sourceStore = NULL;
	PCdp_STORE journalStore = NULL;
	PCdp_CORE core = NULL;
	Cdp_JOURNAL journal;
	GUID sourceGuid = { 0 };
	UCHAR source[1536];
	UCHAR inherited[512];
	UCHAR excluded[512];
	UCHAR sibling[512];
	UCHAR current[512];
	PUCHAR filler = NULL;
	UCHAR output[1536];
	UCHAR expected[1536];
	UCHAR fillerOut[512];
	UINT64 oldRegion;
	UINT64 firstSequence;
	UINT64 endSequence;
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(SRC_SIZE * 32, SECTOR, &sourceStore)),
		"create source for compaction test");
	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &journalStore)),
		"create journal for compaction test");
	if (!sourceStore || !journalStore)
		goto cleanup;
	filler = (PUCHAR)malloc(1024 * 1024);
	if (!filler)
		goto cleanup;
	FillPattern(source, sizeof(source), 0x70);
	RtlFillMemory(inherited, sizeof(inherited), 0x81);
	RtlFillMemory(excluded, sizeof(excluded), 0x92);
	RtlFillMemory(sibling, sizeof(sibling), 0xA3);
	RtlFillMemory(current, sizeof(current), 0xB4);
	RtlFillMemory(filler, 1024 * 1024, 0xC5);
	Expect(NT_SUCCESS(sourceStore->Write(
		sourceStore, 0, sizeof(source), source)),
		"seed compaction source");

	CdpJournalInitializeWithStore(
		&journal, journalStore, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)96000);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)),
		"format compaction journal");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 0, sizeof(inherited), inherited, NULL)),
		"append inherited parent value");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, sizeof(excluded), excluded, NULL)),
		"append excluded parent value");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(&journal, 2, 1, 2)),
		"create sibling branch for compaction");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, sizeof(sibling), sibling, NULL)),
		"append sibling value that must be discarded");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(&journal, 3, 1, 2)),
		"create current branch for compaction");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 1024, sizeof(current), current, NULL)),
		"append current value in oldest region");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 4096, 1024 * 1024, filler, NULL)),
		"rotate to a newer header region");
	Expect(NT_SUCCESS(CdpJournalGetOldestCompactableRegion(
		&journal, &oldRegion, &firstSequence, &endSequence)),
		"oldest region is compactable");
	CdpJournalClose(&journal);

	status = CdpCoreCreate(sourceStore, journalStore, &core);
	Expect(NT_SUCCESS(status), "create compaction core");
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(core);
	Expect(NT_SUCCESS(status), "mount current branch before compaction");
	if (NT_SUCCESS(status))
		status = CdpCoreCompactOldestRegion(core);
	Expect(NT_SUCCESS(status), "materialize and delete oldest region");
	Expect(memcmp(CdpMemStoreData(sourceStore), inherited, 512) == 0 &&
		memcmp((PUCHAR)CdpMemStoreData(sourceStore) + 512,
			source + 512, 512) == 0 &&
		memcmp((PUCHAR)CdpMemStoreData(sourceStore) + 1024,
			source + 1024, 512) == 0,
		"compaction materializes only live values from the deleted region");

	RtlCopyMemory(expected, inherited, 512);
	RtlCopyMemory(expected + 512, source + 512, 512);
	RtlCopyMemory(expected + 1024, current, 512);
	RtlZeroMemory(output, sizeof(output));
	if (NT_SUCCESS(status))
		status = CdpCoreRead(core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) && memcmp(output, expected, sizeof(output)) == 0,
		"read falls back to materialized source values after tree removal");

	CdpCoreDestroy(core);
	core = NULL;
	status = CdpCoreCreate(sourceStore, journalStore, &core);
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(core);
	Expect(NT_SUCCESS(status),
		"remount succeeds after branch markers in old region were reclaimed");
	RtlZeroMemory(fillerOut, sizeof(fillerOut));
	if (NT_SUCCESS(status))
		status = CdpCoreRead(core, 4096, sizeof(fillerOut), fillerOut);
	Expect(NT_SUCCESS(status) &&
		memcmp(fillerOut, filler, sizeof(fillerOut)) == 0,
		"remounted MetaTree retains newer-region journal data");

cleanup:
	if (core)
		CdpCoreDestroy(core);
	if (filler)
		free(filler);
	if (journalStore)
		CdpMemStoreDestroy(journalStore);
	if (sourceStore)
		CdpMemStoreDestroy(sourceStore);
	return g_caseFailed;
}

static int TestAfterImageCompactionPrunesOnlyAtInheritancePoint(void)
{
	PCdp_STORE sourceStore = NULL;
	PCdp_STORE journalStore = NULL;
	PCdp_CORE core = NULL;
	Cdp_JOURNAL journal;
	GUID sourceGuid = { 0 };
	Cdp_JOURNAL_RECORD aRecord;
	Cdp_JOURNAL_RECORD bRecord;
	Cdp_JOURNAL_RECORD xRecord;
	Cdp_JOURNAL_RECORD headers[16];
	PUCHAR b = NULL;
	UCHAR a[512];
	UCHAR c[512];
	UCHAR x[512];
	UCHAR sibling[512];
	UCHAR z[512];
	UCHAR y[512];
	UCHAR postPrune[512];
	Cdp_JOURNAL_RECORD postPruneRecord;
	UINT64 total = 0;
	UINT64 generation = 0;
	UINT64 partitionBytes = 0;
	UINT64 metadataBytes = 0;
	UINT64 payloadBytesUsed = 0;
	UINT64 payloadBytesFree = 0;
	UINT64 usageRecords = 0;
	ULONG returned = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(
		4ULL * 1024 * 1024, SECTOR, &sourceStore)),
		"create source for inheritance-point compaction test");
	Expect(NT_SUCCESS(CdpMemStoreCreate(
		16ULL * 1024 * 1024, SECTOR, &journalStore)),
		"create journal for inheritance-point compaction test");
	if (!sourceStore || !journalStore)
		goto cleanup;
	b = (PUCHAR)malloc(2 * 1024 * 1024);
	if (!b)
		goto cleanup;
	RtlFillMemory(a, sizeof(a), 0x11);
	RtlFillMemory(b, 2 * 1024 * 1024, 0x22);
	RtlFillMemory(c, sizeof(c), 0x33);
	RtlFillMemory(x, sizeof(x), 0x44);
	RtlFillMemory(sibling, sizeof(sibling), 0x55);
	RtlFillMemory(z, sizeof(z), 0x66);
	RtlFillMemory(y, sizeof(y), 0x77);
	RtlFillMemory(postPrune, sizeof(postPrune), 0x88);

	CdpJournalInitializeWithStore(
		&journal, journalStore, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)140000);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)),
		"format inheritance-point journal");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 0, sizeof(a), a, &aRecord)),
		"append branch-1 record a in oldest region");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 4096, 2 * 1024 * 1024, b, &bRecord)),
		"rotate and append branch-1 inheritance record b");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 2ULL * 1024 * 1024, sizeof(c), c, NULL)),
		"append branch-1 suffix c after inheritance point");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 2, 1, bRecord.Sequence + 1)),
		"create branch 2 from c");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 2ULL * 1024 * 1024 + 512, sizeof(x), x, &xRecord)),
		"append branch-2 data");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 3, 1, bRecord.Sequence)),
		"create valid sibling branch 3 from retained record b");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 2ULL * 1024 * 1024 + 1024,
		sizeof(sibling), sibling, NULL)),
		"append valid sibling branch-3 data");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 4, 2, xRecord.Sequence)),
		"create recursive child branch 4 from branch 2");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 2ULL * 1024 * 1024 + 1536, sizeof(z), z, NULL)),
		"append branch-4 descendant data");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 5, 1, bRecord.Sequence)),
		"create latest branch 5 from b");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 3ULL * 1024 * 1024, sizeof(y), y, NULL)),
		"append latest-branch data");
	Expect(aRecord.Sequence == 2 && bRecord.Sequence == 4 &&
		journal.NextSequence == 15,
		"global Record Sequence is unique and monotonic across branches");
	CdpJournalClose(&journal);

	status = CdpCoreCreate(sourceStore, journalStore, &core);
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(core);
	Expect(NT_SUCCESS(status), "mount inheritance-point compaction core");
	if (!NT_SUCCESS(status))
		goto cleanup;

	status = CdpCoreCompactOldestRegion(core);
	Expect(NT_SUCCESS(status),
		"compact region without an inheritance point");
	if (NT_SUCCESS(status))
		status = CdpCoreQueryRecordHeaders(
			core, 0, 0, headers, RTL_NUMBER_OF(headers),
			&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 12 && returned == 12,
		"no-point compaction keeps unreachable sibling subtree for now");

	returned = 0;
	status = CdpCoreCompactOldestRegion(core);
	Expect(NT_SUCCESS(status),
		"compact region containing the latest-branch inheritance point");
	if (NT_SUCCESS(status))
		status = CdpCoreQueryRecordHeaders(
			core, 0, 0, headers, RTL_NUMBER_OF(headers),
			&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 10 && returned == 10 &&
		headers[0].Sequence == 5 && headers[1].Sequence == 6 &&
		headers[2].Sequence == 7 && headers[3].Sequence == 8 &&
		headers[4].Sequence == 9 && headers[5].Sequence == 10 &&
		headers[6].Sequence == 11 && headers[7].Sequence == 12 &&
		headers[8].Sequence == 13 && headers[9].Sequence == 14,
		"compaction retains branch tails until their own inherited baseline is reclaimed");

	/* The branch-2 tail is still reconstructible here: its inheritance record
	 * is in a later RR.  A current-branch fork does not make it garbage. */
	status = CdpCoreQueryJournalUsage(
		core, &partitionBytes, &metadataBytes, &payloadBytesUsed,
		&payloadBytesFree, &usageRecords);
	Expect(NT_SUCCESS(status) && usageRecords == 10,
		"valid branch records remain counted until their inherited baseline is reclaimed");
	Expect(NT_SUCCESS(CdpCoreCompactOldestRegion(core)),
		"merge reaches the branch-2 inheritance RR");
	returned = 0;
	status = CdpCoreQueryRecordHeaders(
		core, 0, 0, headers, RTL_NUMBER_OF(headers),
		&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 4 && returned == 4 &&
		headers[0].Sequence == 9 && headers[1].Sequence == 10 &&
		headers[2].Sequence == 13 && headers[3].Sequence == 14,
		"branch becomes reclaimable only when its own inherited baseline is merged");
	status = CdpCoreQueryJournalUsage(
		core, &partitionBytes, &metadataBytes, &payloadBytesUsed,
		&payloadBytesFree, &usageRecords);
	Expect(NT_SUCCESS(status) && usageRecords == 4,
		"invalid branch subtree is reclaimed after its baseline disappears");

	CdpCoreDestroy(core);
	core = NULL;
	status = CdpCoreCreate(sourceStore, journalStore, &core);
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(core);
	Expect(NT_SUCCESS(status),
		"remount accepts tombstoned global-sequence slots");
	if (NT_SUCCESS(status))
	{
		returned = 0;
		status = CdpCoreQueryRecordHeaders(
			core, 0, 0, headers, RTL_NUMBER_OF(headers),
			&total, &generation, &returned);
		Expect(NT_SUCCESS(status) && total == 4 && returned == 4 &&
			headers[0].Sequence == 9 && headers[1].Sequence == 10 &&
			headers[2].Sequence == 13 && headers[3].Sequence == 14,
			"remount retains valid branches and does not resurrect pruned branches");
		status = CdpCoreAppendAfterImage(
			core, 3ULL * 1024 * 1024 + 512,
			sizeof(postPrune), postPrune, &postPruneRecord);
		Expect(NT_SUCCESS(status) && postPruneRecord.Sequence == 15,
			"new append does not reuse tombstoned global Sequences");
	}

cleanup:
	if (core)
		CdpCoreDestroy(core);
	if (b)
		free(b);
	if (journalStore)
		CdpMemStoreDestroy(journalStore);
	if (sourceStore)
		CdpMemStoreDestroy(sourceStore);
	return g_caseFailed;
}

static int TestAfterImageCompactionFailureKeepsRegion(void)
{
	TEST_CTX ctx;
	TEST_FAIL_STORE sourceFail;
	UCHAR baseline[512];
	UCHAR live[512];
	PUCHAR filler = NULL;
	UCHAR output[512];
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, 2ULL * 1024 * 1024, JNL_SIZE, 97000)),
		"setup compaction failure test");
	filler = (PUCHAR)malloc(1024 * 1024);
	if (!filler)
	{
		TestCtxDestroy(&ctx);
		return g_caseFailed;
	}
	FillPattern(baseline, sizeof(baseline), 0x11);
	RtlFillMemory(live, sizeof(live), 0xD6);
	RtlFillMemory(filler, 1024 * 1024, 0xE7);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(baseline), baseline)),
		"seed source before failed compaction");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(live), live, NULL)),
		"append live value in oldest region");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 4096, 1024 * 1024, filler, NULL)),
		"create newer region before failed compaction");
	TestFailStoreInstall(ctx.Source, &sourceFail);
	sourceFail.FailNextWrites = 1;
	status = CdpCoreCompactOldestRegion(ctx.Core);
	Expect(status == STATUS_IO_DEVICE_ERROR,
		"source write failure aborts compaction");
	TestFailStoreRemove(ctx.Source, &sourceFail);
	RtlZeroMemory(output, sizeof(output));
	status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) && memcmp(output, live, sizeof(output)) == 0,
		"failed compaction keeps MetaTree journal coverage");
	Expect(NT_SUCCESS(CdpCoreCompactOldestRegion(ctx.Core)),
		"failed attempt kept the region for a successful retry");

	free(filler);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestAfterImageCompactionMaterializesRegionLatest(void)
{
	TEST_CTX ctx;
	UCHAR baseline[1024];
	UCHAR oldRegionLatest[1024];
	UCHAR newerOverlay[512];
	UCHAR expected[1024];
	UCHAR output[1024];
	PUCHAR filler = NULL;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, 2ULL * 1024 * 1024, JNL_SIZE, 98000)),
		"setup region-latest materialization test");
	filler = (PUCHAR)malloc(1024 * 1024);
	if (!filler)
	{
		TestCtxDestroy(&ctx);
		return g_caseFailed;
	}
	RtlFillMemory(baseline, sizeof(baseline), 0x12);
	RtlFillMemory(oldRegionLatest, sizeof(oldRegionLatest), 0xA8);
	RtlFillMemory(newerOverlay, sizeof(newerOverlay), 0xB9);
	RtlFillMemory(filler, 1024 * 1024, 0xCA);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(baseline), baseline)),
		"seed source for region-latest materialization");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(oldRegionLatest), oldRegionLatest, NULL)),
		"append full old-region latest record");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 4096, 1024 * 1024, filler, NULL)),
		"rotate old record into a compactable region");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 512, sizeof(newerOverlay), newerOverlay, NULL)),
		"append newer-region partial overlay");

	status = CdpCoreCompactOldestRegion(ctx.Core);
	Expect(NT_SUCCESS(status), "compact partially overlaid old region");
	Expect(memcmp(CdpMemStoreData(ctx.Source),
		oldRegionLatest, sizeof(oldRegionLatest)) == 0,
		"source receives the complete latest value inside deleted region");
	RtlCopyMemory(expected, oldRegionLatest, sizeof(expected));
	RtlCopyMemory(expected + 512, newerOverlay, sizeof(newerOverlay));
	RtlZeroMemory(output, sizeof(output));
	status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) && memcmp(output, expected, sizeof(output)) == 0,
		"MetaTree keeps newer intersection over materialized source baseline");

	free(filler);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestAfterImageBranchInfoTree(void)
{
	PCdp_STORE store = NULL;
	Cdp_JOURNAL journal;
	Cdp_JOURNAL remounted;
	Cdp_JOURNAL_RECORD parentRecord;
	Cdp_JOURNAL_RECORD childRecord;
	GUID sourceGuid = { 0 };
	UCHAR value[512];
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &store)),
		"create journal for branch-info tree test");
	if (!store)
		return g_caseFailed;
	RtlFillMemory(value, sizeof(value), 0x5A);
	CdpJournalInitializeWithStore(
		&journal, store, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)99000);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)),
		"format journal and create branch-info root");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 0, sizeof(value), value, &parentRecord)),
		"advance branch-1 end record");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 2, 1, parentRecord.Sequence)),
		"create child branch and update branch-info tree");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, sizeof(value), value, &childRecord)),
		"advance latest branch end record");
	Expect(journal.BranchTree.Count == 2 &&
		journal.BranchTree.Root != NULL &&
		journal.BranchTree.Root->BranchNumber == 1 &&
		journal.BranchTree.Root->EndRecord.Sequence == parentRecord.Sequence &&
		!journal.BranchTree.Root->Latest &&
		journal.BranchTree.Latest != NULL &&
		journal.BranchTree.Latest->BranchNumber == 2 &&
		journal.BranchTree.Latest->Parent == journal.BranchTree.Root &&
		journal.BranchTree.Latest->InheritedRecordSequence ==
			parentRecord.Sequence &&
		journal.BranchTree.Latest->EndRecord.Sequence == childRecord.Sequence &&
		journal.BranchTree.Latest->Latest,
		"dynamic branch-info tree records ancestry, endpoints and latest flag");
	CdpJournalClose(&journal);

	CdpJournalInitializeWithStore(
		&remounted, store, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)100000);
	status = CdpJournalMount(&remounted);
	Expect(NT_SUCCESS(status) && remounted.BranchTree.Count == 2 &&
		remounted.BranchTree.Root != NULL &&
		remounted.BranchTree.Latest != NULL &&
		remounted.BranchTree.Latest->Parent == remounted.BranchTree.Root &&
		remounted.BranchTree.Latest->EndRecord.Sequence == childRecord.Sequence,
		"mount scan rebuilds the same branch-info topology");
	CdpJournalClose(&remounted);
	CdpMemStoreDestroy(store);
	return g_caseFailed;
}

static int TestAfterImagePreviewBranchPath(void)
{
	PCdp_STORE sourceStore = NULL;
	PCdp_STORE journalStore = NULL;
	PCdp_CORE core = NULL;
	Cdp_JOURNAL journal;
	Cdp_JOURNAL_RECORD inheritedRecord;
	Cdp_JOURNAL_RECORD excludedRecord;
	Cdp_JOURNAL_RECORD siblingRecord;
	Cdp_JOURNAL_RECORD currentRecord;
	GUID sourceGuid = { 0 };
	UCHAR source[1536];
	UCHAR inherited[512];
	UCHAR excluded[512];
	UCHAR sibling[512];
	UCHAR current[512];
	UCHAR expected[1536];
	UCHAR output[1536];
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(SRC_SIZE, SECTOR, &sourceStore)),
		"create preview source store");
	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &journalStore)),
		"create preview journal store");
	if (!sourceStore || !journalStore)
		goto cleanup;
	RtlFillMemory(source, sizeof(source), 0x10);
	RtlFillMemory(inherited, sizeof(inherited), 0x21);
	RtlFillMemory(excluded, sizeof(excluded), 0x32);
	RtlFillMemory(sibling, sizeof(sibling), 0x43);
	RtlFillMemory(current, sizeof(current), 0x54);
	Expect(NT_SUCCESS(sourceStore->Write(
		sourceStore, 0, sizeof(source), source)),
		"seed source fallback for branch preview");

	CdpJournalInitializeWithStore(
		&journal, journalStore, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)100000);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)), "format preview journal");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 0, 512, inherited, &inheritedRecord)),
		"append inherited parent value");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, 512, excluded, &excludedRecord)),
		"append parent value excluded by inheritance point");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 2, 1, inheritedRecord.Sequence)),
		"create historical sibling branch");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, 512, sibling, &siblingRecord)),
		"append sibling branch value");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 3, 1, inheritedRecord.Sequence)),
		"create latest branch from the same parent point");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 1024, 512, current, &currentRecord)),
		"append latest-branch value");
	CdpJournalClose(&journal);

	status = CdpCoreCreate(sourceStore, journalStore, &core);
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(core);
	Expect(NT_SUCCESS(status), "mount branch-aware preview core");
	if (!NT_SUCCESS(status))
		goto cleanup;

	status = CdpCorePreviewBegin(core, siblingRecord.WallClock100ns);
	Expect(NT_SUCCESS(status), "select branch 2 from target time");
	RtlCopyMemory(expected, inherited, 512);
	RtlCopyMemory(expected + 512, sibling, 512);
	RtlCopyMemory(expected + 1024, source + 1024, 512);
	RtlZeroMemory(output, sizeof(output));
	if (NT_SUCCESS(status))
		status = CdpCorePreviewRead(core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) && memcmp(output, expected, sizeof(output)) == 0,
		"preview recursively includes parent inheritance and excludes other branches");
	Expect(NT_SUCCESS(CdpCorePreviewEnd(core)), "end sibling preview");

	status = CdpCorePreviewBegin(core, currentRecord.WallClock100ns);
	Expect(NT_SUCCESS(status), "select latest branch from target time");
	RtlCopyMemory(expected, inherited, 512);
	RtlCopyMemory(expected + 512, source + 512, 512);
	RtlCopyMemory(expected + 1024, current, 512);
	RtlZeroMemory(output, sizeof(output));
	if (NT_SUCCESS(status))
		status = CdpCorePreviewRead(core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) && memcmp(output, expected, sizeof(output)) == 0,
		"latest preview follows its own recursive ancestry path");
	Expect(NT_SUCCESS(CdpCorePreviewEnd(core)), "end latest preview");

cleanup:
	if (core)
		CdpCoreDestroy(core);
	if (journalStore)
		CdpMemStoreDestroy(journalStore);
	if (sourceStore)
		CdpMemStoreDestroy(sourceStore);
	UNREFERENCED_PARAMETER(excludedRecord);
	return g_caseFailed;
}

static int TestAfterImagePreviewMergeCoordination(void)
{
	TEST_CTX ctx;
	UCHAR baseline[512];
	UCHAR oldValue[512];
	UCHAR output[512];
	PUCHAR filler = NULL;
	Cdp_JOURNAL_RECORD oldRecord;
	Cdp_JOURNAL_RECORD newerRecord;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, 2ULL * 1024 * 1024, JNL_SIZE, 110000)),
		"setup preview/merge coordination test");
	filler = (PUCHAR)malloc(1024 * 1024);
	if (!filler)
	{
		TestCtxDestroy(&ctx);
		return g_caseFailed;
	}
	RtlFillMemory(baseline, sizeof(baseline), 0x61);
	RtlFillMemory(oldValue, sizeof(oldValue), 0x72);
	RtlFillMemory(filler, 1024 * 1024, 0x83);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(baseline), baseline)),
		"seed preview/merge source");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(oldValue), oldValue, &oldRecord)),
		"append preview value in oldest region");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 4096, 1024 * 1024, filler, &newerRecord)),
		"rotate preview target into a newer region");

	Expect(NT_SUCCESS(CdpCoreSetMergeActive(ctx.Core, TRUE)),
		"mark merge thread active");
	ExpectStatus(CdpCorePreviewBegin(ctx.Core, newerRecord.WallClock100ns),
		STATUS_DEVICE_BUSY,
		"preview begin is rejected while merge is running");
	Expect(NT_SUCCESS(CdpCoreSetMergeActive(ctx.Core, FALSE)),
		"clear merge-thread gate");

	Expect(NT_SUCCESS(CdpCorePreviewBegin(
		ctx.Core, newerRecord.WallClock100ns)),
		"begin preview whose target is in the retained newer region");
	Expect(NT_SUCCESS(CdpCoreSetMergeActive(ctx.Core, TRUE)),
		"start merge while preview is active");
	status = CdpCoreCompactOldestRegion(ctx.Core);
	Expect(NT_SUCCESS(status) &&
		CdpCoreGetPhase(ctx.Core) == Cdp_CORE_PHASE_PREVIEW &&
		!CdpCoreConsumePreviewStoppedByMerge(ctx.Core),
		"merge keeps preview active before reaching its target record");
	RtlZeroMemory(output, sizeof(output));
	status = CdpCorePreviewRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) && memcmp(output, oldValue, sizeof(output)) == 0,
		"merge rebases reclaimed PreviewTree coverage onto source data");
	Expect(NT_SUCCESS(CdpCoreSetMergeActive(ctx.Core, FALSE)),
		"finish merge before ending preview");
	Expect(NT_SUCCESS(CdpCorePreviewEnd(ctx.Core)),
		"end preview after non-target compaction");

	free(filler);
	TestCtxDestroy(&ctx);

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, 2ULL * 1024 * 1024, JNL_SIZE, 120000)),
		"setup target-region preview stop test");
	filler = (PUCHAR)malloc(1024 * 1024);
	if (!filler)
	{
		TestCtxDestroy(&ctx);
		return g_caseFailed;
	}
	RtlFillMemory(oldValue, sizeof(oldValue), 0x94);
	RtlFillMemory(filler, 1024 * 1024, 0xA5);
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(oldValue), oldValue, &oldRecord)),
		"append target record in oldest region");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 4096, 1024 * 1024, filler, NULL)),
		"create a newer region for target-stop compaction");
	Expect(NT_SUCCESS(CdpCorePreviewBegin(
		ctx.Core, oldRecord.WallClock100ns)),
		"begin preview anchored in oldest region");
	Expect(NT_SUCCESS(CdpCoreSetMergeActive(ctx.Core, TRUE)),
		"start target-region merge");
	status = CdpCoreCompactOldestRegion(ctx.Core);
	Expect(NT_SUCCESS(status) &&
		CdpCoreGetPhase(ctx.Core) == Cdp_CORE_PHASE_GENERAL &&
		CdpCoreConsumePreviewStoppedByMerge(ctx.Core),
		"merge stops preview when deleting its target-record region");
	ExpectStatus(CdpCorePreviewRead(ctx.Core, 0, sizeof(output), output),
		STATUS_INVALID_DEVICE_STATE,
		"stopped preview can no longer serve reads");
	Expect(NT_SUCCESS(CdpCoreSetMergeActive(ctx.Core, FALSE)),
		"finish target-region merge");

	free(filler);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestAfterImageRecoveryBranchSwitch(void)
{
	TEST_CTX ctx;
	TEST_FAIL_STORE sourceTrace;
	Cdp_JOURNAL_RECORD firstRecord;
	Cdp_JOURNAL_RECORD latestRecord;
	Cdp_JOURNAL_RECORD postRecoveryRecord;
	Cdp_JOURNAL_RECORD headers[8];
	UCHAR source[512];
	UCHAR first[512];
	UCHAR latest[512];
	UCHAR postRecovery[512];
	UCHAR output[512];
	UINT64 total = 0;
	UINT64 generation = 0;
	ULONG returned = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 130000)),
		"setup after-image recovery branch-switch test");
	RtlFillMemory(source, sizeof(source), 0x11);
	RtlFillMemory(first, sizeof(first), 0x22);
	RtlFillMemory(latest, sizeof(latest), 0x33);
	RtlFillMemory(postRecovery, sizeof(postRecovery), 0x44);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(source), source)),
		"seed recovery source fallback");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(first), first, &firstRecord)),
		"append recovery target record");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(latest), latest, &latestRecord)),
		"append value newer than recovery target");
	Expect(NT_SUCCESS(CdpCoreSetMergeActive(ctx.Core, TRUE)),
		"mark merge active before direct core recovery");
	ExpectStatus(CdpCoreRecoveryBegin(
		ctx.Core, firstRecord.WallClock100ns),
		STATUS_DEVICE_BUSY,
		"core recovery refuses to race an unstopped merge owner");
	Expect(NT_SUCCESS(CdpCoreSetMergeActive(ctx.Core, FALSE)),
		"clear merge owner as the driver stop/join path would do");

	TestFailStoreInstall(ctx.Source, &sourceTrace);
	status = CdpCoreRecoveryBegin(ctx.Core, firstRecord.WallClock100ns);
	Expect(NT_SUCCESS(status) &&
		CdpCoreGetPhase(ctx.Core) == Cdp_CORE_PHASE_GENERAL,
		"recovery creates and publishes a new branch in one begin operation");
	Expect(sourceTrace.ReadCallCount == 0 && sourceTrace.WriteCallCount == 0,
		"recovery branch switch performs no source read or writeback");
	TestFailStoreRemove(ctx.Source, &sourceTrace);
	RtlZeroMemory(output, sizeof(output));
	status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) && memcmp(output, first, sizeof(output)) == 0,
		"replacement MetaTree exposes the requested historical value");
	Expect(NT_SUCCESS(CdpCoreRecoveryCommit(ctx.Core)),
		"recovery commit is an idempotent no-writeback acknowledgement");

	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(postRecovery), postRecovery,
		&postRecoveryRecord)),
		"new writes continue on the recovery-created branch");
	RtlZeroMemory(output, sizeof(output));
	status = CdpCoreRead(ctx.Core, 0, sizeof(output), output);
	Expect(NT_SUCCESS(status) &&
		memcmp(output, postRecovery, sizeof(output)) == 0,
		"post-recovery write updates the new branch MetaTree");

	status = CdpCoreQueryRecordHeaders(
		ctx.Core, 0, 0, headers, RTL_NUMBER_OF(headers),
		&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 5 && returned == 5 &&
		headers[3].Flags == Cdp_JOURNAL_RECORD_FLAG_BRANCH &&
		headers[4].Sequence == postRecoveryRecord.Sequence,
		"journal contains the recovery branch marker followed by new-branch data");

	// A later Recovery fails after its branch marker was appended. The marker
	// must be removed and the already-published MetaTree must remain untouched.
	CdpCoreTestSetRecoveryBuildFailure(STATUS_IO_DEVICE_ERROR);
	status = CdpCoreRecoveryBegin(ctx.Core, firstRecord.WallClock100ns);
	Expect(status == STATUS_IO_DEVICE_ERROR,
		"recovery reports replacement-tree scan failure");
	RtlZeroMemory(output, sizeof(output));
	Expect(CdpCoreGetPhase(ctx.Core) == Cdp_CORE_PHASE_GENERAL &&
		NT_SUCCESS(CdpCoreRead(ctx.Core, 0, sizeof(output), output)) &&
		memcmp(output, postRecovery, sizeof(output)) == 0,
		"failed recovery preserves the old MetaTree and returns to General");
	returned = 0;
	status = CdpCoreQueryRecordHeaders(
		ctx.Core, 0, 0, headers, RTL_NUMBER_OF(headers),
		&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 5 && returned == 5,
		"failed recovery removes its newly created branch record");

	UNREFERENCED_PARAMETER(latestRecord);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestDeferredRebootRecoveryBranch(void)
{
	TEST_CTX ctx;
	TEST_FAIL_STORE journalFail;
	Cdp_JOURNAL_RECORD targetRecord;
	Cdp_JOURNAL_RECORD writtenRecord;
	Cdp_JOURNAL_RECORD headers[8];
	UCHAR source[512];
	UCHAR target[512];
	UCHAR latest[512];
	UCHAR firstWrite[512];
	UCHAR output[512];
	UINT64 total = 0;
	UINT64 generation = 0;
	ULONG returned = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 160000)),
		"setup deferred reboot recovery test");
	RtlFillMemory(source, sizeof(source), 0x10);
	RtlFillMemory(target, sizeof(target), 0x20);
	RtlFillMemory(latest, sizeof(latest), 0x30);
	RtlFillMemory(firstWrite, sizeof(firstWrite), 0x40);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(source), source)),
		"seed deferred recovery source");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(target), target, &targetRecord)),
		"append deferred recovery target value");
	CdpCoreSetTime100ns(ctx.Core, 170000);
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(latest), latest, NULL)),
		"append value newer than deferred target");

	status = CdpCorePrepareRebootRecovery(
		ctx.Core, targetRecord.WallClock100ns);
	Expect(NT_SUCCESS(status) &&
		CdpCoreHasPendingRecoveryBranch(ctx.Core),
		"boot recovery publishes target view without creating its branch");
	RtlZeroMemory(output, sizeof(output));
	Expect(NT_SUCCESS(CdpCoreRead(
		ctx.Core, 0, sizeof(output), output)) &&
		memcmp(output, target, sizeof(output)) == 0,
		"deferred recovery reads expose the historical target immediately");
	status = CdpCoreQueryRecordHeaders(
		ctx.Core, 0, 0, headers, RTL_NUMBER_OF(headers),
		&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 3 && returned == 3,
		"boot preparation performs no journal write");
	ExpectStatus(CdpCoreSetMergeActive(ctx.Core, TRUE),
		STATUS_DEVICE_BUSY,
		"merge stays disabled while the recovery branch is pending");

	TestFailStoreInstall(ctx.Journal, &journalFail);
	journalFail.FailNextWrites = 32;
	status = CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(firstWrite), firstWrite, NULL);
	Expect(!NT_SUCCESS(status) &&
		CdpCoreHasPendingRecoveryBranch(ctx.Core),
		"failed first-write branch persistence keeps the branch pending");
	TestFailStoreRemove(ctx.Journal, &journalFail);

	CdpCoreSetTime100ns(ctx.Core, 180000);
	status = CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(firstWrite), firstWrite, &writtenRecord);
	Expect(NT_SUCCESS(status) &&
		!CdpCoreHasPendingRecoveryBranch(ctx.Core),
		"first application write materializes the pending branch");
	returned = 0;
	status = CdpCoreQueryRecordHeaders(
		ctx.Core, 0, 0, headers, RTL_NUMBER_OF(headers),
		&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 5 && returned == 5 &&
		headers[3].Flags == Cdp_JOURNAL_RECORD_FLAG_BRANCH &&
		headers[4].Sequence == writtenRecord.Sequence,
		"pending branch marker is committed immediately before first payload");
	RtlZeroMemory(output, sizeof(output));
	Expect(NT_SUCCESS(CdpCoreRead(
		ctx.Core, 0, sizeof(output), output)) &&
		memcmp(output, firstWrite, sizeof(output)) == 0,
		"first write updates the recovered MetaTree on the new branch");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestPersistentRestorePoint(void)
{
	TEST_CTX ctx;
	TEST_DRAIN_WRITER writer;
	PCdp_JOURNAL_SUPERBLOCK superblock;
	Cdp_JOURNAL_RECORD targetRecord;
	Cdp_JOURNAL_RECORD records[8];
	UCHAR baseline[512];
	UCHAR target[512];
	UCHAR latest[512];
	UCHAR fresh[512];
	UCHAR output[512];
	UINT64 effective = 0;
	UINT64 targetSequence = 0;
	UINT64 writtenBytes = 0;
	UINT64 total = 0;
	UINT64 generation = 0;
	ULONG writtenRanges = 0;
	ULONG returned = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 250000)),
		"setup persistent restore-point test");
	if (!ctx.Core)
		return g_caseFailed;
	RtlFillMemory(baseline, sizeof(baseline), 0x11);
	RtlFillMemory(target, sizeof(target), 0x22);
	RtlFillMemory(latest, sizeof(latest), 0x33);
	RtlFillMemory(fresh, sizeof(fresh), 0x44);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(baseline), baseline)),
		"seed source before restore-point history");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(target), target, &targetRecord)),
		"append restore-point target value");
	CdpCoreSetTime100ns(ctx.Core, 260000);
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(latest), latest, NULL)),
		"append value newer than restore point");

	RtlZeroMemory(&writer, sizeof(writer));
	writer.Store = ctx.Source;
	status = CdpCoreMaterializeTimeWithWriter(
		ctx.Core, targetRecord.WallClock100ns,
		TestDrainAbsoluteWriter, &writer,
		&effective, &targetSequence, &writtenRanges, &writtenBytes);
	RtlZeroMemory(output, sizeof(output));
	Expect(NT_SUCCESS(status) && effective == targetRecord.WallClock100ns &&
		targetSequence == targetRecord.Sequence && writtenRanges == 1 &&
		writtenBytes == sizeof(target) &&
		NT_SUCCESS(ctx.Source->Read(ctx.Source, 0, sizeof(output), output)) &&
		memcmp(output, target, sizeof(output)) == 0,
		"setting point materializes exact target view into source");
	Expect(NT_SUCCESS(CdpCoreSetRestorePointMarker(
		ctx.Core, targetRecord.WallClock100ns)),
		"persist restore-point marker in superblock");
	superblock = (PCdp_JOURNAL_SUPERBLOCK)CdpMemStoreData(ctx.Journal);
	Expect((superblock->Flags & Cdp_JOURNAL_FLAG_RESTORE_POINT_SET) != 0 &&
		superblock->RestorePointTime100ns == targetRecord.WallClock100ns,
		"superblock contains persistent restore-point time");

	Expect(NT_SUCCESS(CdpCorePrepareRebootRecovery(
		ctx.Core, targetRecord.WallClock100ns)),
		"prepare higher-priority recovery view");
	ExpectStatus(CdpCorePreparePersistentRestoreBoot(ctx.Core),
		STATUS_INVALID_DEVICE_STATE,
		"recovery preparation prevents restore-point boot handling");

	CdpCoreDestroy(ctx.Core);
	ctx.Core = NULL;
	status = CdpCoreCreate(ctx.Source, ctx.Journal, &ctx.Core);
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(ctx.Core);
	Expect(NT_SUCCESS(status), "remount persistent restore-point journal");
	Expect(NT_SUCCESS(CdpCorePreparePersistentRestoreBoot(ctx.Core)),
		"boot publishes empty MetaTree without writing journal");
	status = CdpCoreQueryRecordHeaders(
		ctx.Core, 0, 0, records, RTL_NUMBER_OF(records),
		&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 3,
		"restore boot preparation leaves old history physically untouched");
	RtlZeroMemory(output, sizeof(output));
	Expect(NT_SUCCESS(CdpCoreRead(
		ctx.Core, 0, sizeof(output), output)) &&
		memcmp(output, target, sizeof(output)) == 0,
		"empty boot MetaTree exposes materialized restore baseline");

	CdpCoreSetTime100ns(ctx.Core, 270000);
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(fresh), fresh, NULL)),
		"first new write resets history and starts fresh branch");
	returned = 0;
	status = CdpCoreQueryRecordHeaders(
		ctx.Core, 0, 0, records, RTL_NUMBER_OF(records),
		&total, &generation, &returned);
	Expect(NT_SUCCESS(status) && total == 2 && returned == 2 &&
		records[0].Flags == Cdp_JOURNAL_RECORD_FLAG_BRANCH &&
		records[1].Flags == 0,
		"old history is replaced by root marker and first fresh record");
	superblock = (PCdp_JOURNAL_SUPERBLOCK)CdpMemStoreData(ctx.Journal);
	Expect((superblock->Flags & Cdp_JOURNAL_FLAG_RESTORE_POINT_SET) != 0,
		"history reset preserves restore-point marker");
	Expect(NT_SUCCESS(CdpCoreClearRestorePointMarker(ctx.Core)),
		"explicit delete clears persistent restore point");
	Expect((superblock->Flags & Cdp_JOURNAL_FLAG_RESTORE_POINT_SET) == 0 &&
		superblock->RestorePointTime100ns == 0,
		"deleted restore point is absent from superblock");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestRestorePointCheckpointMerge(void)
{
	TEST_CTX ctx;
	Cdp_JOURNAL_RECORD oldestRecord;
	PUCHAR filler = NULL;
	PUCHAR sourceBefore = NULL;
	PUCHAR output = NULL;
	UCHAR oldestValue[4096];
	UINT64 oldestTime = 0;
	UINT64 newestTime = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, 3ULL * 1024 * 1024, JNL_SIZE, 330000)),
		"setup restore-point checkpoint merge test");
	if (!ctx.Core)
		return g_caseFailed;
	filler = (PUCHAR)malloc(1024 * 1024);
	sourceBefore = (PUCHAR)malloc(4096);
	output = (PUCHAR)malloc(4096);
	if (!filler || !sourceBefore || !output)
		goto cleanup;
	FillPattern(oldestValue, sizeof(oldestValue), 0x31);
	RtlFillMemory(filler, 1024 * 1024, 0x72);
	RtlFillMemory(sourceBefore, 4096, 0x19);
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, 4096, sourceBefore)),
		"seed source baseline before checkpoint merge");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(oldestValue), oldestValue, &oldestRecord)),
		"append effective data to oldest RR");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 8192, 1024 * 1024, filler, NULL)),
		"rotate a newer active RR before checkpoint merge");
	Expect(NT_SUCCESS(CdpCoreSetRestorePointMarker(
		ctx.Core, oldestRecord.WallClock100ns)),
		"enable persistent restore point for checkpoint strategy");

	status = CdpCoreCheckpointOldestRegion(ctx.Core);
	Expect(NT_SUCCESS(status),
		"checkpoint merge processes exactly one oldest complete RR");
	RtlZeroMemory(output, 4096);
	Expect(NT_SUCCESS(ctx.Source->Read(ctx.Source, 0, 4096, output)) &&
		memcmp(output, sourceBefore, 4096) == 0,
		"checkpoint merge never writes the protected source baseline");
	RtlZeroMemory(output, 4096);
	Expect(NT_SUCCESS(CdpCoreRead(ctx.Core, 0, 4096, output)) &&
		memcmp(output, oldestValue, 4096) == 0,
		"MetaTree payload is remapped to checkpoint data before RR reclaim");
	ExpectStatus(CdpCoreCheckpointOldestRegion(ctx.Core), STATUS_NOT_FOUND,
		"one merge does not treat the active RR as another candidate");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 8192 + 1024 * 1024, 1024 * 1024, filler, NULL)),
		"rotate another active RR after the first checkpoint merge");
	Expect(NT_SUCCESS(CdpCoreCheckpointOldestRegion(ctx.Core)),
		"second merge relocates checkpoints stored inside the reclaimed RR span");
	RtlZeroMemory(output, 4096);
	Expect(NT_SUCCESS(CdpCoreRead(ctx.Core, 0, 4096, output)) &&
		memcmp(output, oldestValue, 4096) == 0,
		"multi-RR checkpoint relocation preserves the earlier checkpoint view");

	status = CdpCoreQueryTimeRange(ctx.Core, &oldestTime, &newestTime);
	if (NT_SUCCESS(status))
		status = CdpCorePreviewBegin(ctx.Core, newestTime);
	RtlZeroMemory(output, 4096);
	Expect(NT_SUCCESS(status) &&
		NT_SUCCESS(CdpCorePreviewRead(ctx.Core, 0, 4096, output)) &&
		memcmp(output, oldestValue, 4096) == 0,
		"preview uses checkpoint baseline after the original RR is reclaimed");
	if (NT_SUCCESS(status))
		Expect(NT_SUCCESS(CdpCorePreviewEnd(ctx.Core)),
			"end checkpoint-backed preview");
	Expect(NT_SUCCESS(CdpCoreClearRestorePointMarker(ctx.Core)),
		"deleting restore point materializes and clears runtime checkpoints");
	RtlZeroMemory(output, 4096);
	Expect(NT_SUCCESS(ctx.Source->Read(ctx.Source, 0, 4096, output)) &&
		memcmp(output, oldestValue, 4096) == 0,
		"restore-point deletion preserves checkpoint baseline on source");

cleanup:
	if (output)
		free(output);
	if (sourceBefore)
		free(sourceBefore);
	if (filler)
		free(filler);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestRuntimeCheckpointReuseOrder(void)
{
	PCdp_STORE journalStore = NULL;
	Cdp_JOURNAL journal;
	GUID sourceGuid = { 0 };
	PCdp_CHECKPOINT_REMAP remaps = NULL;
	ULONG remapCount = 0;
	UCHAR first[4096];
	UCHAR wider[8192];
	UCHAR overwrite[4096];
	UCHAR cross[4096];
	UCHAR output[4096];
	Cdp_RUNTIME_CHECKPOINT_TREE_INFO checkpointInfos[4];
	Cdp_RUNTIME_CHECKPOINT_RECORD_TREE_INFO checkpointRecords[4];
	UINT64 firstCheckpointOffset = 0;
	UINT64 secondCheckpointOffset = 0;
	UINT64 payloadCursorAfterTwo = 0;
	UINT64 checkpointId = 0;
	UINT64 firstCheckpointId = 0;
	UINT64 secondCheckpointId = 0;
	UINT64 totalItems = 0;
	UINT64 queryGeneration = 0;
	ULONG returnedItems = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(
		JNL_SIZE, SECTOR, &journalStore)),
		"create journal for runtime checkpoint reuse test");
	if (!journalStore)
		return g_caseFailed;
	CdpJournalInitializeWithStore(
		&journal, journalStore, &sourceGuid,
		TestPointerTime100ns, (PVOID)(ULONG_PTR)350000);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)),
		"format runtime checkpoint reuse journal");
	Expect(NT_SUCCESS(CdpJournalSetRestorePoint(&journal, 350000)),
		"enable restore point before runtime checkpoint allocation");
	RtlFillMemory(first, sizeof(first), 0x11);
	RtlFillMemory(wider, sizeof(wider), 0x22);
	RtlFillMemory(overwrite, sizeof(overwrite), 0x33);
	RtlFillMemory(cross, sizeof(cross), 0x44);

	status = CdpJournalMergeIntoRuntimeCheckpoints(
		&journal, 0x100000, 1, 2, &checkpointId,
		0, sizeof(first), first, &remaps, &remapCount);
	if (NT_SUCCESS(status) && remapCount == 1)
	{
		firstCheckpointOffset = remaps[0].FileOffset;
		firstCheckpointId = checkpointId;
	}
	Expect(NT_SUCCESS(status) && remapCount == 1 &&
		journal.CheckpointCount == 1,
		"first range creates one runtime checkpoint");
	CdpJournalFreeCheckpointRemaps(remaps);
	remaps = NULL;

	checkpointId = 0;
	status = CdpJournalMergeIntoRuntimeCheckpoints(
		&journal, 0x200000, 2, 3, &checkpointId,
		0, sizeof(wider), wider, &remaps, &remapCount);
	if (NT_SUCCESS(status) && remapCount == 2)
	{
		secondCheckpointOffset = remaps[1].FileOffset;
		secondCheckpointId = checkpointId;
	}
	payloadCursorAfterTwo = journal.PayloadRegionOff;
	Expect(NT_SUCCESS(status) && remapCount == 2 &&
		journal.CheckpointCount == 2 &&
		remaps[0].FileOffset == firstCheckpointOffset &&
		remaps[1].VolumeOffset == 4096,
		"wider range reuses the first checkpoint before allocating its remainder");
	CdpJournalFreeCheckpointRemaps(remaps);
	remaps = NULL;

	checkpointId = 0;
	status = CdpJournalMergeIntoRuntimeCheckpoints(
		&journal, 0x300000, 3, 4, &checkpointId,
		0, sizeof(overwrite), overwrite, &remaps, &remapCount);
	Expect(NT_SUCCESS(status) && remapCount == 1 &&
		journal.CheckpointCount == 2 &&
		journal.PayloadRegionOff == payloadCursorAfterTwo &&
		remaps[0].FileOffset == firstCheckpointOffset,
		"fully consumed data stops immediately without allocating another checkpoint");
	CdpJournalFreeCheckpointRemaps(remaps);
	remaps = NULL;

	checkpointId = 0;
	status = CdpJournalMergeIntoRuntimeCheckpoints(
		&journal, 0x400000, 4, 5, &checkpointId,
		2048, sizeof(cross), cross, &remaps, &remapCount);
	Expect(NT_SUCCESS(status) && remapCount == 2 &&
		journal.CheckpointCount == 2 &&
		journal.PayloadRegionOff == payloadCursorAfterTwo &&
		remaps[0].FileOffset == firstCheckpointOffset + 2048 &&
		remaps[1].FileOffset == secondCheckpointOffset,
		"one range checks checkpoints in order and consumes overlaps from both");
	CdpJournalFreeCheckpointRemaps(remaps);
	remaps = NULL;
	RtlZeroMemory(output, sizeof(output));
	Expect(NT_SUCCESS(CdpJournalReadPayload(
		&journal, firstCheckpointOffset + 2048, sizeof(output), output)) &&
		memcmp(output, cross, sizeof(output)) == 0,
		"reused checkpoint payload contains the final cross-checkpoint data");

	RtlZeroMemory(checkpointInfos, sizeof(checkpointInfos));
	status = CdpJournalQueryRuntimeCheckpointInfos(
		&journal, 0, 0, checkpointInfos, RTL_NUMBER_OF(checkpointInfos),
		&totalItems, &queryGeneration, &returnedItems);
	Expect(NT_SUCCESS(status) && totalItems == 2 && returnedItems == 2 &&
		checkpointInfos[0].CheckpointId == firstCheckpointId &&
		checkpointInfos[0].SourceRegionOffset == 0x100000 &&
		checkpointInfos[0].SourceFirstSequence == 1 &&
		checkpointInfos[0].SourceEndSequence == 2 &&
		checkpointInfos[0].RecordCount == 1 &&
		checkpointInfos[1].CheckpointId == secondCheckpointId &&
		checkpointInfos[1].RecordCount == 1 &&
		journal.CheckpointRecordCount == 2,
		"checkpoint summary query groups records by their source RR merge");

	RtlZeroMemory(checkpointRecords, sizeof(checkpointRecords));
	status = CdpJournalQueryRuntimeCheckpointRecords(
		&journal, secondCheckpointId, 0, queryGeneration,
		checkpointRecords, RTL_NUMBER_OF(checkpointRecords),
		&totalItems, &queryGeneration, &returnedItems);
	Expect(NT_SUCCESS(status) && totalItems == 1 && returnedItems == 1 &&
		checkpointRecords[0].CheckpointId == secondCheckpointId &&
		checkpointRecords[0].RecordIndex == 0 &&
		checkpointRecords[0].VolumeOffset == 4096 &&
		checkpointRecords[0].FileOffset == secondCheckpointOffset &&
		checkpointRecords[0].DataLength == 4096 &&
		checkpointRecords[0].AllocatedLength == 4096,
		"checkpoint record query returns every extent in the selected checkpoint");
	ExpectStatus(CdpJournalQueryRuntimeCheckpointRecords(
		&journal, secondCheckpointId, 0, queryGeneration + 1,
		checkpointRecords, RTL_NUMBER_OF(checkpointRecords),
		&totalItems, &queryGeneration, &returnedItems), STATUS_RETRY,
		"checkpoint record pagination rejects a changed generation");

	CdpJournalClose(&journal);
	CdpMemStoreDestroy(journalStore);
	return g_caseFailed;
}

static int TestAutoDiscoverySkipsRestorePointRecords(void)
{
	TEST_CTX ctx;
	Cdp_JOURNAL autoJournal;
	Cdp_JOURNAL_RECORD record;
	GUID zeroGuid = { 0 };
	UCHAR oldValue[512];
	UCHAR freshValue[512];
	UINT64 total = 0;
	UINT64 generation = 0;
	ULONG returned = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 280000)),
		"setup auto-discovery restore-point scan test");
	if (!ctx.Core)
		return g_caseFailed;
	RtlFillMemory(oldValue, sizeof(oldValue), 0x61);
	RtlFillMemory(freshValue, sizeof(freshValue), 0x62);
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(oldValue), oldValue, NULL)),
		"append history hidden by auto restore boot");
	Expect(NT_SUCCESS(CdpCoreSetRestorePointMarker(ctx.Core, 280000)),
		"persist marker for auto-discovery mount");

	CdpCoreDestroy(ctx.Core);
	ctx.Core = NULL;
	CdpJournalInitializeWithStore(
		&autoJournal, ctx.Journal, &zeroGuid, NULL, NULL);
	status = CdpJournalMountForAutoDiscovery(&autoJournal);
	Expect(NT_SUCCESS(status) && autoJournal.Mounted &&
		autoJournal.RestorePointSet && autoJournal.HistoryScanSkipped &&
		autoJournal.TotalRecords == 0 &&
		autoJournal.HeaderScanBuffer == NULL,
		"auto discovery detects restore point without allocating or scanning Record headers");
	if (NT_SUCCESS(status))
	{
		Expect(NT_SUCCESS(CdpJournalResetHistoryPreserveRestorePoint(
			&autoJournal)),
			"materialize deferred reset after scan-free auto discovery");
		Expect(NT_SUCCESS(CdpJournalAppend(
			&autoJournal, 0, sizeof(freshValue), freshValue, NULL)),
			"first write resets skipped history before append");
		status = CdpJournalQueryRecordHeaders(
			&autoJournal, 0, 0, &record, 1,
			&total, &generation, &returned);
		Expect(NT_SUCCESS(status) && !autoJournal.HistoryScanSkipped &&
			total == 2 && returned == 1 &&
			record.Flags == Cdp_JOURNAL_RECORD_FLAG_BRANCH,
			"scan-free mount transitions to fresh root history");
		CdpJournalClose(&autoJournal);
	}
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestGracefulDisableDrainsMetaTree(void)
{
	TEST_CTX ctx;
	TEST_DRAIN_WRITER writer;
	UCHAR journalA[512];
	UCHAR journalB[512];
	UCHAR applicationWrite[512];
	UCHAR expected[1024];
	UCHAR output[1024];
	BOOLEAN complete = FALSE;
	ULONG iterations = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(TestCtxCreate(
		&ctx, SRC_SIZE, JNL_SIZE, 180000)),
		"setup graceful-disable drain test");
	if (!ctx.Core)
		return g_caseFailed;
	RtlFillMemory(journalA, sizeof(journalA), 0x31);
	RtlFillMemory(journalB, sizeof(journalB), 0x42);
	RtlFillMemory(applicationWrite, sizeof(applicationWrite), 0x53);
	RtlZeroMemory(&writer, sizeof(writer));
	writer.Store = ctx.Source;

	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 0, sizeof(journalA), journalA, NULL)),
		"append first protected value before graceful disable");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		ctx.Core, 512, sizeof(journalB), journalB, NULL)),
		"append second protected value before graceful disable");

	/* Model the DRAINING write worker: source write completes first while
	 * HistoryMutex excludes reads/backfill, then its MetaTree range is punched. */
	Expect(NT_SUCCESS(ctx.Source->Write(
		ctx.Source, 0, sizeof(applicationWrite), applicationWrite)),
		"draining application write commits directly to source");
	Expect(NT_SUCCESS(CdpCorePunchMetaRange(
		ctx.Core, 0, sizeof(applicationWrite))),
		"draining application write holes matching MetaTree coverage");
	RtlCopyMemory(expected, applicationWrite, sizeof(applicationWrite));
	RtlCopyMemory(expected + 512, journalB, sizeof(journalB));
	RtlZeroMemory(output, sizeof(output));
	Expect(NT_SUCCESS(CdpCoreRead(ctx.Core, 0, sizeof(output), output)) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"reads combine punched source bytes with undrained journal bytes");

	writer.FailNext = TRUE;
	status = CdpCoreDrainOneMetaRangeWithWriter(
		ctx.Core, TestDrainAbsoluteWriter, &writer,
		&complete, NULL, NULL);
	Expect(status == STATUS_IO_DEVICE_ERROR && !complete &&
		writer.Calls == 1 && writer.LastAbsoluteOffset == 512 &&
		writer.LastLength == sizeof(journalB),
		"failed disk writer receives exact absolute MetaTree range");
	RtlZeroMemory(output, sizeof(output));
	Expect(NT_SUCCESS(CdpCoreRead(ctx.Core, 0, sizeof(output), output)) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"failed disk backfill does not punch MetaTree coverage");

	while (!complete && iterations++ < 16)
	{
		status = CdpCoreDrainOneMetaRangeWithWriter(
			ctx.Core, TestDrainAbsoluteWriter, &writer,
			&complete, NULL, NULL);
		if (!NT_SUCCESS(status))
			break;
	}
	Expect(NT_SUCCESS(status) && complete,
		"graceful disable drains all remaining MetaTree coverage");
	Expect(memcmp(CdpMemStoreData(ctx.Source), expected, sizeof(expected)) == 0,
		"drain materializes the final current view into source");
	RtlZeroMemory(output, sizeof(output));
	Expect(NT_SUCCESS(CdpCoreRead(ctx.Core, 0, sizeof(output), output)) &&
		memcmp(output, expected, sizeof(output)) == 0,
		"empty MetaTree reads the fully materialized source view");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static VOID RunSiblingInheritanceRetentionCase(_In_ BOOLEAN SharedPoint)
{
	PCdp_STORE sourceStore = NULL;
	PCdp_STORE journalStore = NULL;
	PCdp_CORE core = NULL;
	Cdp_JOURNAL journal;
	GUID sourceGuid = { 0 };
	Cdp_JOURNAL_RECORD earlyRecord;
	Cdp_JOURNAL_RECORD laterRecord;
	Cdp_JOURNAL_RECORD headers[8];
	UCHAR early[512];
	UCHAR later[512];
	UCHAR sibling[512];
	UCHAR current[512];
	UINT64 total = 0;
	UINT64 generation = 0;
	UINT64 partitionBytes = 0;
	UINT64 metadataBytes = 0;
	UINT64 payloadBytesUsed = 0;
	UINT64 payloadBytesFree = 0;
	UINT64 usageRecords = 0;
	ULONG returned = 0;
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(SRC_SIZE, SECTOR, &sourceStore)),
		SharedPoint ? "create source for shared-point sibling case" :
			"create source for earlier-point sibling case");
	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &journalStore)),
		SharedPoint ? "create journal for shared-point sibling case" :
			"create journal for earlier-point sibling case");
	if (!sourceStore || !journalStore)
		goto cleanup;
	RtlFillMemory(early, sizeof(early), 0x21);
	RtlFillMemory(later, sizeof(later), 0x32);
	RtlFillMemory(sibling, sizeof(sibling), 0x43);
	RtlFillMemory(current, sizeof(current), 0x54);
	CdpJournalInitializeWithStore(
		&journal, journalStore, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)(SharedPoint ? 191000 : 190000));
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)),
		"format sibling inheritance journal");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 0, sizeof(early), early, &earlyRecord)),
		"append earlier inheritance candidate in root RR");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, sizeof(later), later, &laterRecord)),
		"append later inheritance candidate in root RR");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 2, 1, earlyRecord.Sequence)),
		"create historical sibling from earlier point");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 1024, sizeof(sibling), sibling, NULL)),
		"append historical sibling data");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 3, 1,
		SharedPoint ? earlyRecord.Sequence : laterRecord.Sequence)),
		SharedPoint ? "create latest branch from the exact shared point" :
			"create latest branch from the later point");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 1536, sizeof(current), current, NULL)),
		"append latest-branch data");
	CdpJournalClose(&journal);

	status = CdpCoreCreate(sourceStore, journalStore, &core);
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(core);
	Expect(NT_SUCCESS(status), "mount sibling inheritance case");
	if (!NT_SUCCESS(status))
		goto cleanup;
	status = CdpCoreCompactOldestRegion(core);
	Expect(NT_SUCCESS(status), "compact RR containing sibling inheritance points");
	if (NT_SUCCESS(status))
		status = CdpCoreQueryRecordHeaders(
			core, 0, 0, headers, RTL_NUMBER_OF(headers),
			&total, &generation, &returned);
	if (SharedPoint)
	{
		Expect(NT_SUCCESS(status) && total == 4 && returned == 4 &&
			headers[0].Sequence == 4 && headers[1].Sequence == 5 &&
			headers[2].Sequence == 6 && headers[3].Sequence == 7,
			"off-path sibling remains valid when latest path shares its exact inheritance point");
	}
	else
	{
		Expect(NT_SUCCESS(status) && total == 2 && returned == 2 &&
			headers[0].Sequence == 6 && headers[1].Sequence == 7,
			"earlier-point sibling becomes invalid when source advances to a later baseline");
	}
	status = CdpCoreQueryJournalUsage(
		core, &partitionBytes, &metadataBytes, &payloadBytesUsed,
		&payloadBytesFree, &usageRecords);
	Expect(NT_SUCCESS(status) && metadataBytes == SECTOR +
		(SharedPoint ? 2ULL : 1ULL) * Cdp_JOURNAL_HEADER_REGION_SIZE,
		SharedPoint ? "shared-point sibling RR blocks contiguous physical reclaim" :
			"invalid earlier-point sibling RR is reclaimed immediately");

cleanup:
	if (core)
		CdpCoreDestroy(core);
	if (journalStore)
		CdpMemStoreDestroy(journalStore);
	if (sourceStore)
		CdpMemStoreDestroy(sourceStore);
}

static int TestSiblingInheritancePointRetention(void)
{
	RunSiblingInheritanceRetentionCase(FALSE);
	RunSiblingInheritanceRetentionCase(TRUE);
	return g_caseFailed;
}

static int TestAncestorBranchTailRetention(void)
{
	PCdp_STORE sourceStore = NULL;
	PCdp_STORE journalStore = NULL;
	PCdp_CORE core = NULL;
	Cdp_JOURNAL journal;
	GUID sourceGuid = { 0 };
	Cdp_JOURNAL_RECORD rootRecord;
	Cdp_JOURNAL_RECORD branch2ForkRecord;
	Cdp_JOURNAL_RECORD branch2TailRecord;
	Cdp_JOURNAL_RECORD headers[16];
	UCHAR root[512];
	UCHAR branch2Fork[512];
	UCHAR branch2Tail[512];
	UCHAR branch3[512];
	UINT64 total = 0;
	UINT64 generation = 0;
	ULONG returned = 0;
	ULONG index;
	BOOLEAN tailRetained = FALSE;
	NTSTATUS status;

	Expect(NT_SUCCESS(CdpMemStoreCreate(SRC_SIZE, SECTOR, &sourceStore)),
		"create source for ancestor-branch tail retention");
	Expect(NT_SUCCESS(CdpMemStoreCreate(JNL_SIZE, SECTOR, &journalStore)),
		"create journal for ancestor-branch tail retention");
	if (!sourceStore || !journalStore)
		goto cleanup;
	RtlFillMemory(root, sizeof(root), 0x11);
	RtlFillMemory(branch2Fork, sizeof(branch2Fork), 0x22);
	RtlFillMemory(branch2Tail, sizeof(branch2Tail), 0x33);
	RtlFillMemory(branch3, sizeof(branch3), 0x44);
	CdpJournalInitializeWithStore(
		&journal, journalStore, &sourceGuid, TestPointerTime100ns,
		(PVOID)(ULONG_PTR)192000);
	Expect(NT_SUCCESS(CdpJournalFormat(&journal)),
		"format ancestor-branch tail retention journal");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 0, sizeof(root), root, &rootRecord)),
		"append root record before Branch 2 inheritance");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 2, 1, rootRecord.Sequence)),
		"create Branch 2 from retained root inheritance point");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 512, sizeof(branch2Fork), branch2Fork,
		&branch2ForkRecord)),
		"append Branch 2 point inherited by Branch 3");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 1024, sizeof(branch2Tail), branch2Tail,
		&branch2TailRecord)),
		"append valid Branch 2 tail after Branch 3 fork point");
	Expect(NT_SUCCESS(CdpJournalAppendBranch(
		&journal, 3, 2, branch2ForkRecord.Sequence)),
		"create current Branch 3 from Branch 2");
	Expect(NT_SUCCESS(CdpJournalAppend(
		&journal, 1536, sizeof(branch3), branch3, NULL)),
		"append current Branch 3 data");
	CdpJournalClose(&journal);

	status = CdpCoreCreate(sourceStore, journalStore, &core);
	if (NT_SUCCESS(status))
		status = CdpCoreMountJournal(core);
	Expect(NT_SUCCESS(status), "mount ancestor-branch tail retention case");
	if (!NT_SUCCESS(status))
		goto cleanup;
	Expect(NT_SUCCESS(CdpCoreCompactOldestRegion(core)),
		"compact root RR containing Branch 2 inheritance point");
	status = CdpCoreQueryRecordHeaders(
		core, 0, 0, headers, RTL_NUMBER_OF(headers),
		&total, &generation, &returned);
	for (index = 0; NT_SUCCESS(status) && index < returned; ++index)
	{
		if (headers[index].Sequence == branch2TailRecord.Sequence)
		{
			tailRetained = TRUE;
			break;
		}
	}
	Expect(NT_SUCCESS(status) && tailRetained,
		"valid Branch 2 tail survives compaction after current Branch 3 forks earlier");

cleanup:
	if (core)
		CdpCoreDestroy(core);
	if (journalStore)
		CdpMemStoreDestroy(journalStore);
	if (sourceStore)
		CdpMemStoreDestroy(sourceStore);
	return g_caseFailed;
}

static int TestMultipleProtectedPartitionIsolation(void)
{
	TEST_CTX first;
	TEST_CTX second;
	UCHAR firstSource[2048];
	UCHAR secondSource[2048];
	UCHAR firstWrite[512];
	UCHAR secondWrite[512];
	UCHAR firstRead[2048];
	UCHAR secondRead[2048];
	UCHAR firstExpected[2048];
	UCHAR secondExpected[2048];
	NTSTATUS firstStatus;
	NTSTATUS secondStatus;

	RtlZeroMemory(&first, sizeof(first));
	RtlZeroMemory(&second, sizeof(second));
	firstStatus = TestCtxCreate(&first, SRC_SIZE, JNL_SIZE, 200000);
	secondStatus = TestCtxCreate(&second, SRC_SIZE, JNL_SIZE, 300000);
	Expect(NT_SUCCESS(firstStatus) && NT_SUCCESS(secondStatus),
		"create two independent protected partition contexts");
	if (!NT_SUCCESS(firstStatus) || !NT_SUCCESS(secondStatus))
	{
		TestCtxDestroy(&first);
		TestCtxDestroy(&second);
		return g_caseFailed;
	}

	FillPattern(firstSource, sizeof(firstSource), 0x10);
	FillPattern(secondSource, sizeof(secondSource), 0x70);
	RtlFillMemory(firstWrite, sizeof(firstWrite), 0xA1);
	RtlFillMemory(secondWrite, sizeof(secondWrite), 0xB2);
	Expect(NT_SUCCESS(first.Source->Write(
		first.Source, 0, sizeof(firstSource), firstSource)) &&
		NT_SUCCESS(second.Source->Write(
			second.Source, 0, sizeof(secondSource), secondSource)),
		"seed distinct source baselines for both partitions");
	Expect(NT_SUCCESS(CdpCoreAppendAfterImage(
		first.Core, 512, sizeof(firstWrite), firstWrite, NULL)) &&
		NT_SUCCESS(CdpCoreAppendAfterImage(
			second.Core, 512, sizeof(secondWrite), secondWrite, NULL)),
		"append the same logical range to two independent journals");

	RtlCopyMemory(firstExpected, firstSource, sizeof(firstExpected));
	RtlCopyMemory(secondExpected, secondSource, sizeof(secondExpected));
	RtlCopyMemory(firstExpected + 512, firstWrite, sizeof(firstWrite));
	RtlCopyMemory(secondExpected + 512, secondWrite, sizeof(secondWrite));
	firstStatus = CdpCoreRead(first.Core, 0, sizeof(firstRead), firstRead);
	secondStatus = CdpCoreRead(second.Core, 0, sizeof(secondRead), secondRead);
	Expect(NT_SUCCESS(firstStatus) && NT_SUCCESS(secondStatus) &&
		memcmp(firstRead, firstExpected, sizeof(firstRead)) == 0 &&
		memcmp(secondRead, secondExpected, sizeof(secondRead)) == 0,
		"each partition read resolves only its own MetaTree and journal");
	Expect(memcmp(firstRead + 512, secondRead + 512, sizeof(firstWrite)) != 0,
		"overlapping logical offsets do not cross between protected partitions");

	TestCtxDestroy(&first);
	TestCtxDestroy(&second);
	return g_caseFailed;
}

int main(void)
{
	int failed = 0;
	setvbuf(stdout, NULL, _IONBF, 0);

	failed += RunCase("After-image initial branch record",
		TestAfterImageInitialBranchRecord);
	failed += RunCase("Journal physical-layout persistence",
		TestJournalPhysicalLayoutPersistence);
	failed += RunCase("After-image append does not touch source",
		TestAfterImageAppendDoesNotTouchSource);
	failed += RunCase("After-image journal failure does not bypass source",
		TestAfterImageJournalFailureDoesNotBypassSource);
	failed += RunCase("After-image payload zero-copy and fallback",
		TestAfterImagePayloadZeroCopyAndFallback);
	failed += RunCase("Record-header sector write cache",
		TestRecordHeaderWriteReusesSectorCache);
	failed += RunCase("After-image branch numbers increase",
		TestAfterImageBranchNumbersIncrease);
	failed += RunCase("After-image mixed read and latest overlap",
		TestAfterImageMixedReadAndLatestOverlap);
	failed += RunCase("After-image all overlap shapes",
		TestAfterImageAllOverlapShapes);
	failed += RunCase("MetaTree compound overlap model",
		TestMetaTreeCompoundOverlapModel);
	failed += RunCase("MetaTree randomized reference model",
		TestMetaTreeRandomizedReferenceModel);
	failed += RunCase("MetaTree randomized write/read/remount",
		TestMetaTreeRandomizedWriteReadRemount);
	failed += RunCase("After-image branch ancestry read",
		TestAfterImageBranchAncestryRead);
	failed += RunCase("After-image compacts current branch only",
		TestAfterImageCompactsOnlyCurrentBranch);
	failed += RunCase("After-image inheritance-point reachability pruning",
		TestAfterImageCompactionPrunesOnlyAtInheritancePoint);
	failed += RunCase("After-image compaction failure keeps region",
		TestAfterImageCompactionFailureKeepsRegion);
	failed += RunCase("After-image compaction materializes region latest",
		TestAfterImageCompactionMaterializesRegionLatest);
	failed += RunCase("After-image branch information tree",
		TestAfterImageBranchInfoTree);
	failed += RunCase("After-image preview branch path",
		TestAfterImagePreviewBranchPath);
	failed += RunCase("After-image preview/merge coordination",
		TestAfterImagePreviewMergeCoordination);
	failed += RunCase("After-image recovery branch switch",
		TestAfterImageRecoveryBranchSwitch);
	failed += RunCase("Deferred reboot recovery branch",
		TestDeferredRebootRecoveryBranch);
	failed += RunCase("Persistent restore point",
		TestPersistentRestorePoint);
	failed += RunCase("Restore-point checkpoint merge",
		TestRestorePointCheckpointMerge);
	failed += RunCase("Runtime checkpoint reuse order",
		TestRuntimeCheckpointReuseOrder);
	failed += RunCase("Auto discovery skips restore-point records",
		TestAutoDiscoverySkipsRestorePointRecords);
	failed += RunCase("Graceful disable drains MetaTree",
		TestGracefulDisableDrainsMetaTree);
	failed += RunCase("Sibling inheritance point retention",
		TestSiblingInheritancePointRetention);
	failed += RunCase("Ancestor branch tail retention",
		TestAncestorBranchTailRetention);
	failed += RunCase("Multiple protected partition isolation",
		TestMultipleProtectedPartitionIsolation);

	printf("\n%s (%d failures)\n", failed ? "FAILED" : "ALL PASSED", failed);
	return failed ? 1 : 0;
}
