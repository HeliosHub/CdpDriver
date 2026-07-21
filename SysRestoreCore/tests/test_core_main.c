#include "qh_core.h"

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

#define SRC_SIZE   (64ULL * 1024)
#define JNL_SIZE   (8ULL * 1024 * 1024)
#define SMALL_JNL_SIZE (3ULL * 1024 * 1024)
#define TINY_JNL_SIZE  (2098688ULL + 16384ULL)
#define SECTOR     512

typedef struct _TEST_CTX
{
	PQH_STORE Source;
	PQH_STORE Journal;
	PQH_CORE Core;
} TEST_CTX, *PTEST_CTX;

static void FillPattern(PUCHAR buf, ULONG len, UCHAR seed)
{
	ULONG i;
	for (i = 0; i < len; ++i)
		buf[i] = (UCHAR)(seed + i);
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
	st = QhMemStoreCreate(SourceSize, SectorSize, &Ctx->Source);
	if (!NT_SUCCESS(st))
		return st;
	st = QhMemStoreCreate(JournalSize, SectorSize, &Ctx->Journal);
	if (!NT_SUCCESS(st))
	{
		QhMemStoreDestroy(Ctx->Source);
		Ctx->Source = NULL;
		return st;
	}
	st = QhCoreCreate(Ctx->Source, Ctx->Journal, &Ctx->Core);
	if (!NT_SUCCESS(st))
	{
		QhMemStoreDestroy(Ctx->Journal);
		QhMemStoreDestroy(Ctx->Source);
		Ctx->Journal = NULL;
		Ctx->Source = NULL;
		return st;
	}
	QhCoreSetTime100ns(Ctx->Core, InitialTime100ns);
	st = QhCoreFormatJournal(Ctx->Core);
	if (!NT_SUCCESS(st))
	{
		QhCoreDestroy(Ctx->Core);
		QhMemStoreDestroy(Ctx->Journal);
		QhMemStoreDestroy(Ctx->Source);
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
	QH_STORE_READ OriginalRead;
	QH_STORE_WRITE OriginalWrite;
	LONG FailNextReads;
	ULONG ReadCallCount;
	LONG FailNextWrites;
	ULONG MaxWriteLength;
	ULONG OversizeWriteCount;
	ULONG LargestSuccessfulWrite;
} TEST_FAIL_STORE, *PTEST_FAIL_STORE;

static NTSTATUS TestFailStoreRead(
	_In_ PQH_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PTEST_FAIL_STORE fail = (PTEST_FAIL_STORE)Store->Context;
	NTSTATUS status;

	++fail->ReadCallCount;
	if (fail->FailNextReads > 0)
	{
		--fail->FailNextReads;
		return STATUS_IO_DEVICE_ERROR;
	}

	Store->Context = fail->OriginalContext;
	status = fail->OriginalRead(Store, Offset, Length, Buffer);
	Store->Context = fail;
	return status;
}

static NTSTATUS TestFailStoreWrite(
	_In_ PQH_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	PTEST_FAIL_STORE fail = (PTEST_FAIL_STORE)Store->Context;
	NTSTATUS status;

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

static VOID TestFailStoreInstall(_Inout_ PQH_STORE Store, _Out_ PTEST_FAIL_STORE Fail)
{
	Fail->OriginalContext = Store->Context;
	Fail->OriginalRead = Store->Read;
	Fail->OriginalWrite = Store->Write;
	Fail->FailNextReads = 0;
	Fail->ReadCallCount = 0;
	Fail->FailNextWrites = 0;
	Fail->MaxWriteLength = 0;
	Fail->OversizeWriteCount = 0;
	Fail->LargestSuccessfulWrite = 0;
	Store->Context = Fail;
	Store->Read = TestFailStoreRead;
	Store->Write = TestFailStoreWrite;
}

static VOID TestFailStoreRemove(_Inout_ PQH_STORE Store, _In_ PTEST_FAIL_STORE Fail)
{
	Store->Context = Fail->OriginalContext;
	Store->Read = Fail->OriginalRead;
	Store->Write = Fail->OriginalWrite;
}

static VOID TestCtxDestroy(_Inout_ PTEST_CTX Ctx)
{
	if (Ctx->Core)
	{
		QhCoreDestroy(Ctx->Core);
		Ctx->Core = NULL;
	}
	if (Ctx->Journal)
	{
		QhMemStoreDestroy(Ctx->Journal);
		Ctx->Journal = NULL;
	}
	if (Ctx->Source)
	{
		QhMemStoreDestroy(Ctx->Source);
		Ctx->Source = NULL;
	}
}

static NTSTATUS TestRecoveryOneShot(
	_Inout_ PQH_CORE Core,
	_In_ UINT64 TargetTime100ns)
{
	NTSTATUS status = QhCoreRecoveryBegin(Core, TargetTime100ns);
	if (!NT_SUCCESS(status))
		return status;
	return QhCoreRecoveryCommit(Core);
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

/* --- existing end-to-end --- */

static int TestCowPreviewRecovery(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR b[512];
	UCHAR c[512];
	UCHAR out[512];
	UINT64 t0, t1, t2;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 1000)),
		"setup core");

	FillPattern(a, sizeof(a), 0x10);
	FillPattern(b, sizeof(b), 0x20);
	FillPattern(c, sizeof(c), 0x30);

	st = ctx.Source->Write(ctx.Source, 0, sizeof(a), a);
	Expect(NT_SUCCESS(st), "seed source with A");

	t0 = QhCoreGetTime100ns(ctx.Core);
	st = QhCoreWrite(ctx.Core, 0, sizeof(b), b);
	Expect(NT_SUCCESS(st), "COW write B over A");
	t1 = QhCoreGetTime100ns(ctx.Core);

	st = QhCoreWrite(ctx.Core, 0, sizeof(c), c);
	Expect(NT_SUCCESS(st), "COW write C over B");
	t2 = QhCoreGetTime100ns(ctx.Core);
	Expect(t2 > t1 && t1 > t0, "time advances on COW");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, c, sizeof(c)) == 0,
		"Normal read sees C (live)");

	st = QhCorePreviewBegin(ctx.Core, t0);
	Expect(NT_SUCCESS(st), "PreviewBegin at t0");
	Expect(QhCoreGetPhase(ctx.Core) == QH_CORE_PHASE_PREVIEW, "phase Preview");
	Expect(QhCoreGetTargetTime100ns(ctx.Core) == t0, "target time saved");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, a, sizeof(a)) == 0,
		"Preview at t0 includes t0 record and sees A");

	st = QhCorePreviewEnd(ctx.Core);
	Expect(NT_SUCCESS(st), "PreviewEnd");
	Expect(QhCoreGetPhase(ctx.Core) == QH_CORE_PHASE_NORMAL, "phase Normal");

	st = TestRecoveryOneShot(ctx.Core, t0);
	Expect(NT_SUCCESS(st), "RecoveryBegin at t0");
	Expect(QhCoreGetPhase(ctx.Core) == QH_CORE_PHASE_NORMAL,
		"one-shot Recovery returns to Normal");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, a, sizeof(a)) == 0,
		"Recovery at t0 includes t0 record and reads A");

	Expect(memcmp(QhMemStoreData(ctx.Source), a, sizeof(a)) == 0,
		"Recovery at t0 physically writes back t0 before-image A");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestRecoveryInvalidate(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR b[512];
	UCHAR n[512];
	UCHAR out[512];
	UINT64 t1;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 5000)), "setup");

	FillPattern(a, sizeof(a), 0x41);
	FillPattern(b, sizeof(b), 0x42);
	FillPattern(n, sizeof(n), 0x4E);
	ctx.Source->Write(ctx.Source, 0, sizeof(a), a);
	QhCoreWrite(ctx.Core, 0, sizeof(b), b);
	t1 = QhCoreGetTime100ns(ctx.Core);

	st = TestRecoveryOneShot(ctx.Core, t1 - 1);
	Expect(NT_SUCCESS(st), "RecoveryBegin before B");

	st = QhCoreWrite(ctx.Core, 0, sizeof(n), n);
	Expect(NT_SUCCESS(st), "write after one-shot recovery");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, n, sizeof(n)) == 0,
		"read after Recovery sees new write");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- driver-style capture append (journal only, no source write) --- */

static int TestCaptureAppendPath(void)
{
	TEST_CTX ctx;
	UCHAR seed[512];
	UCHAR data[512];
	UCHAR out[512];
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 2000)), "setup");

	FillPattern(seed, sizeof(seed), 0x01);
	FillPattern(data, sizeof(data), 0x55);
	ctx.Source->Write(ctx.Source, 0, sizeof(seed), seed);

	st = QhCoreCaptureAppend(ctx.Core, 0, sizeof(data), NULL);
	Expect(NT_SUCCESS(st), "CaptureAppend journals before-image");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, seed, sizeof(seed)) == 0,
		"Normal read still sees pre-append live data");

	st = ctx.Source->Write(ctx.Source, 0, sizeof(data), data);
	Expect(NT_SUCCESS(st), "caller applies write separately");

	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, data, sizeof(data)) == 0,
		"live data updated after caller write");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- mixed journal + live gap fill in Preview --- */

static int TestMixedReadPartialCoverage(void)
{
	TEST_CTX ctx;
	UCHAR whole[1024];
	UCHAR half[512];
	UCHAR out[1024];
	UINT64 tAfterHalf;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 3000)), "setup");

	FillPattern(whole, sizeof(whole), 0xAA);
	FillPattern(half, sizeof(half), 0xBB);
	ctx.Source->Write(ctx.Source, 0, sizeof(whole), whole);

	tAfterHalf = QhCoreGetTime100ns(ctx.Core);
	st = QhCoreWrite(ctx.Core, 0, sizeof(half), half);
	Expect(NT_SUCCESS(st), "COW first half only");

	st = QhCorePreviewBegin(ctx.Core, tAfterHalf);
	Expect(NT_SUCCESS(st), "PreviewBegin");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st), "Preview mixed read ok");
	Expect(memcmp(out, whole, sizeof(half)) == 0,
		"covered prefix uses target-time record before-image (A)");
	Expect(memcmp(out + sizeof(half), whole + sizeof(half),
			sizeof(whole) - sizeof(half)) == 0,
		"uncovered suffix from live (A)");

	QhCorePreviewEnd(ctx.Core);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- multi-offset + overlapping dedup (earliest sequence wins) --- */

static int TestMultiOffsetAndDedup(void)
{
	TEST_CTX ctx;
	UCHAR seed0[512];
	UCHAR seed1[512];
	UCHAR cow0[512];
	UCHAR cow1[512];
	UCHAR out[512];
	UINT64 tAfterFirstCow;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 4000)), "setup");

	FillPattern(seed0, sizeof(seed0), 0xA0);
	FillPattern(seed1, sizeof(seed1), 0xA1);
	FillPattern(cow0, sizeof(cow0), 0xB0);
	FillPattern(cow1, sizeof(cow1), 0xB1);
	ctx.Source->Write(ctx.Source, 0, sizeof(seed0), seed0);
	ctx.Source->Write(ctx.Source, 512, sizeof(seed1), seed1);

	QhCoreWrite(ctx.Core, 0, sizeof(cow0), cow0);
	tAfterFirstCow = QhCoreGetTime100ns(ctx.Core);
	QhCoreWrite(ctx.Core, 512, sizeof(cow1), cow1);

	/* Target = first record WallClock (tAfterFirstCow - 1): Oldest <= T < Newest */
	st = QhCorePreviewBegin(ctx.Core, tAfterFirstCow - 1);
	Expect(NT_SUCCESS(st), "PreviewBegin after two COWs at first record time");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, seed0, sizeof(seed0)) == 0,
		"offset 0 includes oldest target-time COW record");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 512, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, seed1, sizeof(seed1)) == 0,
		"offset 512 preview sees before-image of second COW (seed1)");

	QhCorePreviewEnd(ctx.Core);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- query time range + mount persistence --- */

static int TestQueryTimeRangeAndMount(void)
{
	TEST_CTX ctx;
	PQH_CORE core2 = NULL;
	UINT64 oldest = 0;
	UINT64 newest = 0;
	UINT64 ticked;
	UCHAR buf[512];
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 6000)), "setup");

	FillPattern(buf, sizeof(buf), 0x71);
	ctx.Source->Write(ctx.Source, 0, sizeof(buf), buf);
	QhCoreWrite(ctx.Core, 0, sizeof(buf), buf);

	st = QhCoreQueryTimeRange(ctx.Core, &oldest, &newest);
	Expect(NT_SUCCESS(st), "QueryTimeRange after COW");
	Expect(oldest <= newest, "oldest <= newest");
	Expect(newest >= 6000, "newest reflects writes");

	ticked = QhCoreTick(ctx.Core, 42);
	Expect(ticked == QhCoreGetTime100ns(ctx.Core), "QhCoreTick");

	QhCoreDestroy(ctx.Core);
	ctx.Core = NULL;

	st = QhCoreCreate(ctx.Source, ctx.Journal, &core2);
	Expect(NT_SUCCESS(st), "recreate core on same stores");
	st = QhCoreMountJournal(core2);
	Expect(NT_SUCCESS(st), "MountJournal on formatted partition");

	st = QhCoreQueryTimeRange(core2, &oldest, &newest);
	Expect(NT_SUCCESS(st), "QueryTimeRange after mount");
	Expect(newest >= oldest, "newest >= oldest after mount");
	Expect(oldest >= 6000, "oldest time persisted across mount");

	QhCoreDestroy(core2);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- invalid parameters and phase guards --- */

static int TestErrorsAndPhaseGuards(void)
{
	TEST_CTX ctx;
	UCHAR buf[512];
	PQH_CORE bogus = NULL;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 7000)), "setup");

	Expect(QhCoreCreate(NULL, ctx.Journal, &bogus) ==
			STATUS_INVALID_PARAMETER,
		"Create rejects null source");
	Expect(QhCoreRead(NULL, 0, sizeof(buf), buf) ==
			STATUS_INVALID_PARAMETER,
		"Read rejects null core");
	Expect(QhCoreWrite(ctx.Core, 0, 0, buf) ==
			STATUS_INVALID_PARAMETER,
		"Write rejects zero length");
	Expect(QhCoreCaptureAppend(ctx.Core, 0, 0, NULL) ==
			STATUS_INVALID_PARAMETER,
		"CaptureAppend rejects zero length");

	st = QhCorePreviewBegin(ctx.Core, 7000);
	Expect(NT_SUCCESS(st), "PreviewBegin ok");
	Expect(QhCorePreviewBegin(ctx.Core, 7000) ==
			STATUS_INVALID_DEVICE_STATE,
		"PreviewBegin rejected while already Preview");
	Expect(TestRecoveryOneShot(ctx.Core, 7000) ==
			STATUS_INVALID_DEVICE_STATE,
		"RecoveryBegin rejected while Preview");

	QhCorePreviewEnd(ctx.Core);

	st = TestRecoveryOneShot(ctx.Core, 7000);
	Expect(NT_SUCCESS(st), "RecoveryBegin ok");
	Expect(QhCoreGetPhase(ctx.Core) == QH_CORE_PHASE_NORMAL,
		"Recovery completes in Normal phase");
	Expect(NT_SUCCESS(QhCorePreviewBegin(ctx.Core, 7000)),
		"PreviewBegin allowed after one-shot Recovery");
	Expect(NT_SUCCESS(QhCorePreviewEnd(ctx.Core)),
		"PreviewEnd succeeds after one-shot Recovery");
	Expect(NT_SUCCESS(TestRecoveryOneShot(ctx.Core, 7000)),
		"another one-shot Recovery is allowed");

	Expect(QhCorePreviewEnd(ctx.Core) == STATUS_INVALID_DEVICE_STATE,
		"PreviewEnd rejected while Normal");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- preview write after session established (PreviewTree live update) --- */

static int TestPreviewWriteDuringSession(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR b[512];
	UCHAR c[512];
	UCHAR seedExtra[512];
	UCHAR extra[512];
	UCHAR out[512];
	UINT64 tAfterB;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 8000)), "setup");

	FillPattern(a, sizeof(a), 0xA0);
	FillPattern(b, sizeof(b), 0xB0);
	FillPattern(c, sizeof(c), 0xC0);
	FillPattern(seedExtra, sizeof(seedExtra), 0x11);
	FillPattern(extra, sizeof(extra), 0xE0);
	ctx.Source->Write(ctx.Source, 0, sizeof(a), a);
	ctx.Source->Write(ctx.Source, 1024, sizeof(seedExtra), seedExtra);

	QhCoreWrite(ctx.Core, 0, sizeof(b), b);
	tAfterB = QhCoreGetTime100ns(ctx.Core);
	QhCoreWrite(ctx.Core, 0, sizeof(c), c);

	st = QhCorePreviewBegin(ctx.Core, tAfterB);
	Expect(NT_SUCCESS(st), "PreviewBegin at C record time");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, b, sizeof(b)) == 0,
		"Preview read returns before-image B (from C's journal record)");

	st = QhCoreWrite(ctx.Core, 1024, sizeof(extra), extra);
	Expect(NT_SUCCESS(st), "COW during Preview at offset 1024");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 1024, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, seedExtra, sizeof(seedExtra)) == 0,
		"Preview read at 1024 returns before-image (not post-write live)");

	QhCorePreviewEnd(ctx.Core);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- preview at seed-only (no COW yet) --- */

static int TestPreviewBeforeAnyCow(void)
{
	TEST_CTX ctx;
	UCHAR seed[512];
	UCHAR out[512];
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 9000)), "setup");

	FillPattern(seed, sizeof(seed), 0xE0);
	ctx.Source->Write(ctx.Source, 0, sizeof(seed), seed);

	st = QhCorePreviewBegin(ctx.Core, 9500);
	Expect(NT_SUCCESS(st), "PreviewBegin with empty journal tree");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, seed, sizeof(seed)) == 0,
		"Preview with no history falls back to live seed");

	QhCorePreviewEnd(ctx.Core);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- staging during Preview build (Merge into tree) --- */

static UCHAR g_stagingBuf[512];

static VOID PreviewBuildHook_InsertStaging(_Inout_ PQH_CORE Core)
{
	FillPattern(g_stagingBuf, sizeof(g_stagingBuf), 0x51);
	(void)QhCoreWrite(Core, 256, sizeof(g_stagingBuf), g_stagingBuf);
}

static int TestPreviewStagingMerge(void)
{
	TEST_CTX ctx;
	UCHAR base[512];
	UCHAR early[512];
	UCHAR beforeStaging[512];
	UCHAR out[512];
	UINT64 t0;
	NTSTATUS st;

	QhCoreTestSetPreviewBuildHook(NULL);
	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 10000)), "setup");

	FillPattern(base, sizeof(base), 0xF0);
	FillPattern(early, sizeof(early), 0xF1);
	ctx.Source->Write(ctx.Source, 0, sizeof(base), base);

	QhCoreWrite(ctx.Core, 0, sizeof(early), early);
	t0 = QhCoreGetTime100ns(ctx.Core);
	FillPattern(beforeStaging, sizeof(beforeStaging), 0xF2);
	ctx.Source->Write(ctx.Source, 256, sizeof(beforeStaging), beforeStaging);

	QhCoreTestSetPreviewBuildHook(PreviewBuildHook_InsertStaging);
	st = QhCorePreviewBegin(ctx.Core, t0);
	QhCoreTestSetPreviewBuildHook(NULL);
	Expect(NT_SUCCESS(st), "PreviewBegin with staging hook");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 256, sizeof(out), out);
	Expect(NT_SUCCESS(st) &&
			memcmp(out, beforeStaging, sizeof(beforeStaging)) == 0,
		"Preview staging merge serves before-image at offset 256");

	QhCorePreviewEnd(ctx.Core);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- staging during Recovery build (Punch History, not merge) --- */

static UCHAR g_punchBuf[512];

static VOID RecoveryBuildHook_StagingPunch(_Inout_ PQH_CORE Core)
{
	FillPattern(g_punchBuf, sizeof(g_punchBuf), 0x50);
	(void)QhCoreWrite(Core, 0, sizeof(g_punchBuf), g_punchBuf);
}

static int TestRecoveryStagingPunch(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR b[512];
	UCHAR c[512];
	UCHAR out[512];
	UINT64 t0, t1;
	NTSTATUS st;

	QhCoreTestSetRecoveryBuildHook(NULL);
	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 11000)), "setup");

	FillPattern(a, sizeof(a), 0x01);
	FillPattern(b, sizeof(b), 0x02);
	FillPattern(c, sizeof(c), 0x03);
	ctx.Source->Write(ctx.Source, 0, sizeof(a), a);

	t0 = QhCoreGetTime100ns(ctx.Core);
	QhCoreWrite(ctx.Core, 0, sizeof(b), b);
	t1 = QhCoreGetTime100ns(ctx.Core);
	QhCoreWrite(ctx.Core, 0, sizeof(c), c);

	/* Recover to t1: history should be B, staging during build punches B */
	QhCoreTestSetRecoveryBuildHook(RecoveryBuildHook_StagingPunch);
	st = TestRecoveryOneShot(ctx.Core, t1);
	QhCoreTestSetRecoveryBuildHook(NULL);
	Expect(NT_SUCCESS(st), "RecoveryBegin with staging punch hook");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, g_punchBuf, sizeof(g_punchBuf)) == 0,
		"Recovery read after punch sees staging/live (not punched-back B)");

	/* Source should have writeback of A only (B was punched), plus staging write */
	Expect(memcmp(QhMemStoreData(ctx.Source), g_punchBuf, sizeof(g_punchBuf)) == 0,
		"source reflects post-punch writeback path");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- recovery writeback: earliest sequence wins on overlap --- */

static int TestRecoveryWritebackOverlap(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR bFull[512];
	UCHAR bTail[256];
	UCHAR out[512];
	UINT64 tAfterFull;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 12000)), "setup");

	FillPattern(a, sizeof(a), 0xA5);
	FillPattern(bFull, sizeof(bFull), 0xB5);
	FillPattern(bTail, sizeof(bTail), 0xBE);
	ctx.Source->Write(ctx.Source, 0, sizeof(a), a);

	QhCoreWrite(ctx.Core, 0, sizeof(bFull), bFull);
	tAfterFull = QhCoreGetTime100ns(ctx.Core);
	QhCoreWrite(ctx.Core, 256, sizeof(bTail), bTail);

	st = TestRecoveryOneShot(ctx.Core, tAfterFull);
	Expect(NT_SUCCESS(st), "RecoveryBegin to full B time");

	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, bFull, sizeof(bFull)) == 0,
		"Recovery read full block");
	Expect(memcmp(QhMemStoreData(ctx.Source), bFull, sizeof(bFull)) == 0,
		"writeback restores full B (earlier seq on overlap)");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- unmounted journal operations fail --- */

static int TestUnmountedJournal(void)
{
	TEST_CTX ctx;
	UCHAR buf[512];
	PQH_CORE bare = NULL;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 13000)), "setup");
	QhCoreDestroy(ctx.Core);
	ctx.Core = NULL;

	Expect(NT_SUCCESS(QhCoreCreate(ctx.Source, ctx.Journal, &bare)),
		"create without format/mount");
	Expect(QhCoreCaptureAppend(bare, 0, sizeof(buf), NULL) ==
			STATUS_DEVICE_NOT_READY,
		"CaptureAppend fails when journal not mounted");
	Expect(QhCorePreviewBegin(bare, 13000) == STATUS_DEVICE_NOT_READY,
		"PreviewBegin fails when journal not mounted");
	Expect(TestRecoveryOneShot(bare, 13000) == STATUS_DEVICE_NOT_READY,
		"RecoveryBegin fails when journal not mounted");

	QhCoreDestroy(bare);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- journal ring: drop oldest when payload ring is full --- */

static NTSTATUS TestAppendJournalRecord(
	_Inout_ PTEST_CTX Ctx,
	_In_ UINT64 Time100ns,
	_In_ UINT64 Offset,
	_In_reads_bytes_(Length) PUCHAR Data,
	_In_ ULONG Length)
{
	NTSTATUS st;

	/* Match driver/COW order: journal before-image, then live write. */
	QhCoreSetTime100ns(Ctx->Core, Time100ns);
	st = QhCoreCaptureAppend(Ctx->Core, Offset, Length, NULL);
	if (!NT_SUCCESS(st))
		return st;
	return Ctx->Source->Write(Ctx->Source, Offset, Length, Data);
}

static int TestJournalDropOldest(void)
{
	TEST_CTX ctx;
	UCHAR buf[512];
	UCHAR out[512];
	UCHAR expectLive[512];
	UCHAR expectSnapshot[512];
	UINT64 oldest = 0;
	UINT64 newest = 0;
	UINT64 firstTime = 20000;
	ULONG i;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, SMALL_JNL_SIZE, firstTime)),
		"setup small journal");

	for (i = 0; i < 2600; ++i)
	{
		FillPattern(buf, sizeof(buf), (UCHAR)(i & 0xFF));
		st = TestAppendJournalRecord(
			&ctx,
			firstTime + i,
			0,
			buf,
			sizeof(buf));
		Expect(NT_SUCCESS(st), "append under ring pressure");
		if (!NT_SUCCESS(st))
			break;
	}

	st = QhCoreQueryTimeRange(ctx.Core, &oldest, &newest);
	Expect(NT_SUCCESS(st), "QueryTimeRange after fill");
	Expect(oldest > firstTime + 100,
		"oldest record evicted (ring dropped early entries)");
	Expect(newest >= firstTime + 2599, "newest retains latest write");

	/* Core maps journal NOT_FOUND (target < oldest) to empty tree + SUCCESS. */
	st = QhCorePreviewBegin(ctx.Core, firstTime + 50);
	Expect(NT_SUCCESS(st),
		"preview before oldest evicted succeeds (empty tree fallback)");
	FillPattern(expectLive, sizeof(expectLive), (UCHAR)(2599 & 0xFF));
	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, expectLive, sizeof(out)) == 0,
		"preview before oldest reads live (evicted journal unavailable)");
	QhCorePreviewEnd(ctx.Core);

	st = QhCorePreviewBegin(ctx.Core, newest);
	Expect(NT_SUCCESS(st), "preview near newest still works");
	FillPattern(expectSnapshot, sizeof(expectSnapshot), (UCHAR)(2598 & 0xFF));
	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, expectSnapshot, sizeof(out)) == 0,
		"preview near newest reconstructs offset 0 before latest write");
	QhCorePreviewEnd(ctx.Core);

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- payload hits partition tail, wraps, continues after drop-oldest --- */

static int TestJournalPayloadTailWrap(void)
{
	TEST_CTX ctx;
	UCHAR buf[512];
	UINT64 oldestBefore = 0;
	UINT64 newestBefore = 0;
	UINT64 oldestAfter = 0;
	UINT64 newestAfter = 0;
	UINT64 firstTime = 30000;
	ULONG i;
	NTSTATUS st;
	ULONG tailFillRecords;

	/* First payload region contiguous space ~= 3MB - 2MB header - superblocks. */
	tailFillRecords = (SMALL_JNL_SIZE - (2ULL * 1024 * 1024) - 4096ULL) / 512;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, SMALL_JNL_SIZE, firstTime)),
		"setup small journal");

	for (i = 0; i < tailFillRecords; ++i)
	{
		FillPattern(buf, sizeof(buf), (UCHAR)(0x80 + (i & 0x7F)));
		st = TestAppendJournalRecord(
			&ctx,
			firstTime + i,
			0,
			buf,
			sizeof(buf));
		Expect(NT_SUCCESS(st), "fill toward partition tail");
	}

	st = QhCoreQueryTimeRange(ctx.Core, &oldestBefore, &newestBefore);
	Expect(NT_SUCCESS(st), "QueryTimeRange before tail wrap");

	/* Next append must wrap payload cursor off the end and still succeed. */
	FillPattern(buf, sizeof(buf), 0xEE);
	st = TestAppendJournalRecord(
		&ctx,
		firstTime + tailFillRecords,
		0,
		buf,
		sizeof(buf));
	Expect(NT_SUCCESS(st), "append after tail wrap");

	st = TestAppendJournalRecord(
		&ctx,
		firstTime + tailFillRecords + 1,
		512,
		buf,
		sizeof(buf));
	Expect(NT_SUCCESS(st), "append continues after wrap (drop-oldest if needed)");

	st = QhCoreQueryTimeRange(ctx.Core, &oldestAfter, &newestAfter);
	Expect(NT_SUCCESS(st), "QueryTimeRange after tail wrap");
	Expect(newestAfter >= newestBefore, "newest advances after tail wrap");
	Expect(oldestAfter >= oldestBefore,
		"oldest time non-decreasing across tail wrap");
	if (oldestBefore > firstTime)
	{
		Expect(oldestAfter > oldestBefore,
			"tail wrap triggered additional eviction");
	}

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- tiny journal: large record + remount after ring pressure --- */

static int TestJournalTinyPartitionLargeRecord(void)
{
	TEST_CTX ctx;
	UCHAR smallBuf[512];
	UCHAR largeBuf[8192];
	UCHAR outBuf[8192];
	UINT64 oldest = 0;
	UINT64 newest = 0;
	UINT64 firstTime = 40000;
	ULONG i;
	NTSTATUS st;
	PQH_CORE remounted = NULL;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, TINY_JNL_SIZE, firstTime)),
		"setup tiny journal");

	FillPattern(largeBuf, sizeof(largeBuf), 0xD0);
	st = TestAppendJournalRecord(&ctx, firstTime, 0, largeBuf, sizeof(largeBuf));
	Expect(NT_SUCCESS(st), "large append on tiny journal");

	for (i = 0; i < 24; ++i)
	{
		FillPattern(smallBuf, sizeof(smallBuf), (UCHAR)(0xA0 + i));
		st = TestAppendJournalRecord(
			&ctx,
			firstTime + 1 + i,
			512,
			smallBuf,
			sizeof(smallBuf));
		Expect(NT_SUCCESS(st), "small appends force tail wrap on tiny volume");
	}

	st = QhCoreQueryTimeRange(ctx.Core, &oldest, &newest);
	Expect(NT_SUCCESS(st), "QueryTimeRange on tiny journal");
	Expect(oldest > firstTime, "tiny journal evicted oldest large record");

	/* Overwrite live offset 0 so preview cannot mask eviction via live data. */
	FillPattern(smallBuf, sizeof(smallBuf), 0xFF);
	st = ctx.Source->Write(ctx.Source, 0, sizeof(smallBuf), smallBuf);
	Expect(NT_SUCCESS(st), "overwrite live offset 0 after eviction");

	st = QhCorePreviewBegin(ctx.Core, firstTime);
	Expect(NT_SUCCESS(st),
		"preview before evicted oldest on tiny journal (empty tree)");
	memset(outBuf, 0, sizeof(outBuf));
	st = QhCoreRead(ctx.Core, 0, sizeof(smallBuf), outBuf);
	Expect(NT_SUCCESS(st) && memcmp(outBuf, smallBuf, sizeof(smallBuf)) == 0,
		"evicted large record not reconstructable from journal");
	QhCorePreviewEnd(ctx.Core);

	st = QhCorePreviewBegin(ctx.Core, newest);
	Expect(NT_SUCCESS(st), "preview newest on tiny journal");
	memset(outBuf, 0, sizeof(outBuf));
	st = QhCoreRead(ctx.Core, 512, sizeof(smallBuf), outBuf);
	Expect(NT_SUCCESS(st), "read recent offset after tiny journal churn");
	FillPattern(smallBuf, sizeof(smallBuf), (UCHAR)(0xA0 + 22));
	Expect(memcmp(outBuf, smallBuf, sizeof(smallBuf)) == 0,
		"preview near newest shows before-image of recent 512 write");
	QhCorePreviewEnd(ctx.Core);

	QhCoreDestroy(ctx.Core);
	ctx.Core = NULL;
	st = QhCoreCreate(ctx.Source, ctx.Journal, &remounted);
	Expect(NT_SUCCESS(st), "recreate core");
	st = QhCoreMountJournal(remounted);
	Expect(NT_SUCCESS(st), "remount after ring churn");
	st = QhCoreQueryTimeRange(remounted, &oldest, &newest);
	Expect(NT_SUCCESS(st) && newest > oldest,
		"journal metadata survives remount after wrap/evict");

	QhCoreDestroy(remounted);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- empty journal QueryTimeRange --- */

static int TestQueryTimeRangeEmptyJournal(void)
{
	TEST_CTX ctx;
	UINT64 oldest = 0;
	UINT64 newest = 0;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 14000)), "setup");
	st = QhCoreQueryTimeRange(ctx.Core, &oldest, &newest);
	Expect(st == STATUS_NOT_FOUND, "QueryTimeRange on empty journal");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- Recovery time boundaries (before oldest / at or after newest) --- */

static int TestRecoveryTimeBoundaries(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR b[512];
	UCHAR c[512];
	UCHAR liveBefore[512];
	UINT64 oldest = 0;
	UINT64 newest = 0;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 15000)), "setup");

	FillPattern(a, sizeof(a), 0x31);
	FillPattern(b, sizeof(b), 0x32);
	FillPattern(c, sizeof(c), 0x33);
	ctx.Source->Write(ctx.Source, 0, sizeof(a), a);
	QhCoreWrite(ctx.Core, 0, sizeof(b), b);
	QhCoreWrite(ctx.Core, 0, sizeof(c), c);

	st = QhCoreQueryTimeRange(ctx.Core, &oldest, &newest);
	Expect(NT_SUCCESS(st), "QueryTimeRange after COWs");
	memcpy(liveBefore, QhMemStoreData(ctx.Source), sizeof(liveBefore));

	st = TestRecoveryOneShot(ctx.Core, oldest - 1);
	Expect(NT_SUCCESS(st), "RecoveryBegin before oldest succeeds (empty history)");
	Expect(memcmp(QhMemStoreData(ctx.Source), liveBefore, sizeof(liveBefore)) == 0,
		"Recovery before oldest leaves live source unchanged");

	st = TestRecoveryOneShot(ctx.Core, oldest);
	Expect(NT_SUCCESS(st), "RecoveryBegin at oldest includes oldest record");
	Expect(memcmp(QhMemStoreData(ctx.Source), a, sizeof(a)) == 0,
		"Recovery at oldest restores oldest record before-image");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- Preview target at/after newest falls back to live --- */

static int TestPreviewTargetAtOrAfterNewest(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR b[512];
	UCHAR c[512];
	UCHAR out[512];
	UINT64 oldest = 0;
	UINT64 newest = 0;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 16000)), "setup");

	FillPattern(a, sizeof(a), 0x41);
	FillPattern(b, sizeof(b), 0x42);
	FillPattern(c, sizeof(c), 0x43);
	ctx.Source->Write(ctx.Source, 0, sizeof(a), a);
	QhCoreWrite(ctx.Core, 0, sizeof(b), b);
	QhCoreWrite(ctx.Core, 0, sizeof(c), c);

	st = QhCoreQueryTimeRange(ctx.Core, &oldest, &newest);
	Expect(NT_SUCCESS(st), "QueryTimeRange newest");

	st = QhCorePreviewBegin(ctx.Core, newest);
	Expect(NT_SUCCESS(st), "PreviewBegin at newest");
	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, b, sizeof(b)) == 0,
		"Preview at newest includes newest record and reads B");
	QhCorePreviewEnd(ctx.Core);

	st = QhCorePreviewBegin(ctx.Core, newest + 500);
	Expect(NT_SUCCESS(st), "PreviewBegin after newest");
	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, c, sizeof(c)) == 0,
		"Preview after newest reads live C");
	QhCorePreviewEnd(ctx.Core);

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- append rejects record larger than journal maximum --- */

static int TestJournalAppendTooLarge(void)
{
	TEST_CTX ctx;
	PUCHAR big = NULL;
	ULONG overSize = QH_JOURNAL_MAX_RECORD_DATA + SECTOR;
	NTSTATUS st;

	Expect(NT_SUCCESS(
			TestCtxCreate(
				&ctx,
				(2ULL * 1024 * 1024) + SECTOR,
				JNL_SIZE,
				17000)),
		"setup large source");
	big = (PUCHAR)malloc(overSize);
	Expect(big != NULL, "allocate oversize buffer");
	if (!big)
	{
		TestCtxDestroy(&ctx);
		return g_caseFailed;
	}
	memset(big, 0xAB, overSize);

	st = QhCoreCaptureAppend(ctx.Core, 0, overSize, NULL);
	Expect(st == STATUS_INVALID_PARAMETER,
		"CaptureAppend rejects length above journal maximum");

	st = QhCoreWrite(ctx.Core, 0, overSize, big);
	Expect(st == STATUS_INVALID_PARAMETER,
		"Write rejects length above journal maximum");

	free(big);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- mount unformatted journal partition --- */

static int TestMountUnformattedJournal(void)
{
	PQH_STORE source = NULL;
	PQH_STORE journal = NULL;
	PQH_CORE core = NULL;
	NTSTATUS st;

	st = QhMemStoreCreate(SRC_SIZE, SECTOR, &source);
	Expect(NT_SUCCESS(st), "create source");
	st = QhMemStoreCreate(JNL_SIZE, SECTOR, &journal);
	Expect(NT_SUCCESS(st), "create journal store");
	st = QhCoreCreate(source, journal, &core);
	Expect(NT_SUCCESS(st), "create core");
	st = QhCoreMountJournal(core);
	Expect(st == STATUS_DISK_CORRUPT_ERROR,
		"Mount rejects unformatted journal partition");

	QhCoreDestroy(core);
	QhMemStoreDestroy(journal);
	QhMemStoreDestroy(source);
	return g_caseFailed;
}

/* --- WritebackActive during Recovery writeback --- */

static int g_writebackHookInvoked;

static VOID RecoveryWritebackHook(_Inout_ PQH_CORE Core)
{
	UCHAR scratch[512];

	(void)Core;
	g_writebackHookInvoked = 1;
	/* Concurrent COW during writeback must not rely on Invalid (WritebackActive). */
	FillPattern(scratch, sizeof(scratch), 0x99);
	(void)QhCoreWrite(Core, 1024, sizeof(scratch), scratch);
}

static int TestRecoveryWritebackActive(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR bFull[512];
	UCHAR bTail[256];
	UCHAR scratchLive[512];
	UINT64 tAfterFull;
	NTSTATUS st;

	QhCoreTestSetWritebackHook(NULL);
	g_writebackHookInvoked = 0;
	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 18000)), "setup");

	FillPattern(a, sizeof(a), 0xA5);
	FillPattern(bFull, sizeof(bFull), 0xB5);
	FillPattern(bTail, sizeof(bTail), 0xBE);
	ctx.Source->Write(ctx.Source, 0, sizeof(a), a);

	QhCoreWrite(ctx.Core, 0, sizeof(bFull), bFull);
	tAfterFull = QhCoreGetTime100ns(ctx.Core);
	QhCoreWrite(ctx.Core, 256, sizeof(bTail), bTail);

	QhCoreTestSetWritebackHook(RecoveryWritebackHook);
	st = TestRecoveryOneShot(ctx.Core, tAfterFull);
	QhCoreTestSetWritebackHook(NULL);
	Expect(NT_SUCCESS(st), "RecoveryBegin with writeback hook");
	Expect(g_writebackHookInvoked != 0, "writeback hook invoked during RecoveryBegin");
	Expect(memcmp(QhMemStoreData(ctx.Source), bFull, sizeof(bFull)) == 0,
		"writeback completes despite concurrent COW during writeback");
	FillPattern(scratchLive, sizeof(scratchLive), 0x99);
	Expect(memcmp((PUCHAR)QhMemStoreData(ctx.Source) + 1024,
			scratchLive,
			sizeof(scratchLive)) == 0,
		"concurrent COW during writeback applied to live source");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- format rejects partition smaller than minimum layout --- */

static int TestQueryTimeRangeWallClocks(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR b[512];
	UINT64 tFirst = 50000;
	UINT64 tSecond = 50001;
	UINT64 oldest = 0;
	UINT64 newest = 0;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, tFirst)), "setup");

	FillPattern(a, sizeof(a), 0x51);
	FillPattern(b, sizeof(b), 0x52);
	ctx.Source->Write(ctx.Source, 0, sizeof(a), a);

	st = TestAppendJournalRecord(&ctx, tFirst, 0, a, sizeof(a));
	Expect(NT_SUCCESS(st), "first journal record");
	st = TestAppendJournalRecord(&ctx, tSecond, 512, b, sizeof(b));
	Expect(NT_SUCCESS(st), "second journal record");

	st = QhCoreQueryTimeRange(ctx.Core, &oldest, &newest);
	Expect(NT_SUCCESS(st), "QueryTimeRange after two records");
	Expect(oldest == tFirst, "oldest matches first record WallClock");
	Expect(newest == tSecond, "newest matches last record WallClock");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestQueryTimeRangeAfterEviction(void)
{
	TEST_CTX ctx;
	UCHAR buf[512];
	UINT64 oldest = 0;
	UINT64 newest = 0;
	UINT64 firstTime = 51000;
	ULONG i;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, SMALL_JNL_SIZE, firstTime)),
		"setup small journal");

	for (i = 0; i < 2600; ++i)
	{
		FillPattern(buf, sizeof(buf), (UCHAR)(i & 0xFF));
		st = TestAppendJournalRecord(
			&ctx,
			firstTime + i,
			0,
			buf,
			sizeof(buf));
		Expect(NT_SUCCESS(st), "append under ring pressure");
		if (!NT_SUCCESS(st))
			break;
	}

	st = QhCoreQueryTimeRange(ctx.Core, &oldest, &newest);
	Expect(NT_SUCCESS(st), "QueryTimeRange after eviction");
	Expect(oldest > firstTime, "oldest advanced after drop-oldest");
	Expect(newest == firstTime + 2599, "newest retains latest record time");
	Expect(newest > oldest, "newest > oldest after eviction");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestQueryTimeRangeGuards(void)
{
	TEST_CTX ctx;
	PQH_CORE bare = NULL;
	UINT64 oldest = 0;
	UINT64 newest = 0;

	Expect(QhCoreQueryTimeRange(NULL, &oldest, &newest) ==
			STATUS_INVALID_PARAMETER,
		"QueryTimeRange rejects null core");

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 52000)), "setup");
	Expect(QhCoreQueryTimeRange(ctx.Core, NULL, &newest) ==
			STATUS_INVALID_PARAMETER,
		"QueryTimeRange rejects null oldest");
	Expect(QhCoreQueryTimeRange(ctx.Core, &oldest, NULL) ==
			STATUS_INVALID_PARAMETER,
		"QueryTimeRange rejects null newest");
	TestCtxDestroy(&ctx);

	Expect(NT_SUCCESS(QhMemStoreCreate(SRC_SIZE, SECTOR, &ctx.Source)),
		"create source for unmounted core");
	Expect(NT_SUCCESS(QhMemStoreCreate(JNL_SIZE, SECTOR, &ctx.Journal)),
		"create journal store");
	Expect(NT_SUCCESS(QhCoreCreate(ctx.Source, ctx.Journal, &bare)),
		"create core without mount");
	Expect(QhCoreQueryTimeRange(bare, &oldest, &newest) ==
			STATUS_DEVICE_NOT_READY,
		"QueryTimeRange fails when journal not mounted");
	QhCoreDestroy(bare);
	QhMemStoreDestroy(ctx.Journal);
	QhMemStoreDestroy(ctx.Source);

	return g_caseFailed;
}

static int TestJournalAppendMaxSize(void)
{
	TEST_CTX ctx;
	PUCHAR maxBuf = NULL;
	ULONG maxSize = QH_JOURNAL_MAX_RECORD_DATA;
	ULONG i;
	NTSTATUS st;

	Expect(NT_SUCCESS(
			TestCtxCreate(&ctx, maxSize, JNL_SIZE, 53000)),
		"setup source sized for max record");
	maxBuf = (PUCHAR)malloc(maxSize);
	Expect(maxBuf != NULL, "allocate max-size buffer");
	if (!maxBuf)
	{
		TestCtxDestroy(&ctx);
		return g_caseFailed;
	}
	for (i = 0; i < maxSize; ++i)
		maxBuf[i] = (UCHAR)(0xC0 + (i & 0x3F));

	st = QhCoreCaptureAppend(ctx.Core, 0, maxSize, NULL);
	Expect(NT_SUCCESS(st), "CaptureAppend accepts max record size");

	st = ctx.Source->Write(ctx.Source, 0, maxSize, maxBuf);
	Expect(NT_SUCCESS(st), "live write after max append");

	free(maxBuf);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestCaptureAppendSourceBounds(void)
{
	TEST_CTX ctx;
	UCHAR buf[512];
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 54000)), "setup");

	FillPattern(buf, sizeof(buf), 0x77);
	st = QhCoreCaptureAppend(ctx.Core, SRC_SIZE, sizeof(buf), NULL);
	Expect(st == STATUS_INVALID_PARAMETER,
		"CaptureAppend rejects read past source end");

	st = QhCoreCaptureAppend(ctx.Core, SRC_SIZE - 256, 512, NULL);
	Expect(st == STATUS_INVALID_PARAMETER,
		"CaptureAppend rejects partial overlap past source end");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestJournalFormatTooSmall(void)
{
	PQH_STORE source = NULL;
	PQH_STORE journal = NULL;
	PQH_CORE core = NULL;
	NTSTATUS st;

	st = QhMemStoreCreate(SRC_SIZE, SECTOR, &source);
	Expect(NT_SUCCESS(st), "create source");
	st = QhMemStoreCreate(1024ULL * 1024, SECTOR, &journal);
	Expect(NT_SUCCESS(st), "create undersized journal store");
	st = QhCoreCreate(source, journal, &core);
	Expect(NT_SUCCESS(st), "create core");
	st = QhCoreFormatJournal(core);
	Expect(st == STATUS_INVALID_PARAMETER,
		"format rejects partition below journal minimum");

	QhCoreDestroy(core);
	QhMemStoreDestroy(journal);
	QhMemStoreDestroy(source);
	return g_caseFailed;
}

/* --- header-region format write-size fallback and cache --- */

static int TestJournalFormatWriteChunkFallback(void)
{
	PQH_STORE source = NULL;
	PQH_STORE journal = NULL;
	PQH_CORE core = NULL;
	TEST_FAIL_STORE limited;
	NTSTATUS st;

	st = QhMemStoreCreate(SRC_SIZE, SECTOR, &source);
	Expect(NT_SUCCESS(st), "create source for format write fallback");
	st = QhMemStoreCreate(JNL_SIZE, SECTOR, &journal);
	Expect(NT_SUCCESS(st), "create journal for format write fallback");
	st = QhCoreCreate(source, journal, &core);
	Expect(NT_SUCCESS(st), "create core for format write fallback");
	if (!NT_SUCCESS(st))
		goto cleanup;

	TestFailStoreInstall(journal, &limited);
	limited.MaxWriteLength = 64UL * 1024UL;

	st = QhCoreFormatJournal(core);
	Expect(NT_SUCCESS(st), "format falls back to supported write size");
	Expect(limited.OversizeWriteCount == 5,
		"format probes 2MB down to 64KB");
	Expect(limited.LargestSuccessfulWrite == 64UL * 1024UL,
		"format selects 64KB as largest successful write");

	limited.OversizeWriteCount = 0;
	limited.LargestSuccessfulWrite = 0;
	st = QhCoreFormatJournal(core);
	Expect(NT_SUCCESS(st), "second format reuses cached write size");
	Expect(limited.OversizeWriteCount == 0,
		"second format does not retry oversized writes");
	Expect(limited.LargestSuccessfulWrite == 64UL * 1024UL,
		"second format continues with cached 64KB writes");

	QhCoreDestroy(core);
	core = NULL;
	TestFailStoreRemove(journal, &limited);

cleanup:
	if (core)
		QhCoreDestroy(core);
	QhMemStoreDestroy(journal);
	QhMemStoreDestroy(source);
	return g_caseFailed;
}

/* --- 4 KiB-sector journal end to end --- */

static int TestFourKiBSectorCowPreviewRecovery(void)
{
	TEST_CTX ctx;
	UCHAR a[4096];
	UCHAR b[4096];
	UCHAR c[4096];
	UCHAR out[4096];
	UINT64 t0;
	UINT64 tAfterB;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreateWithSector(
		&ctx, 128ULL * 1024, 8ULL * 1024 * 1024, 26000, 4096)),
		"setup 4KiB-sector core");
	FillPattern(a, sizeof(a), 0x11);
	FillPattern(b, sizeof(b), 0x22);
	FillPattern(c, sizeof(c), 0x33);
	Expect(NT_SUCCESS(ctx.Source->Write(ctx.Source, 0, sizeof(a), a)),
		"seed 4KiB source");
	t0 = QhCoreGetTime100ns(ctx.Core);
	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 0, sizeof(b), b)),
		"4KiB COW write B");
	tAfterB = QhCoreGetTime100ns(ctx.Core);
	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 0, sizeof(c), c)),
		"4KiB COW write C");

	Expect(NT_SUCCESS(QhCorePreviewBegin(ctx.Core, t0)),
		"4KiB PreviewBegin");
	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, a, sizeof(a)) == 0,
		"4KiB Preview includes target-time record and returns A");
	Expect(NT_SUCCESS(QhCorePreviewEnd(ctx.Core)), "4KiB PreviewEnd");

	Expect(NT_SUCCESS(TestRecoveryOneShot(ctx.Core, tAfterB)),
		"4KiB RecoveryBegin");
	Expect(memcmp(QhMemStoreData(ctx.Source), b, sizeof(b)) == 0,
		"4KiB Recovery writes back B");
	Expect(QhCoreGetPhase(ctx.Core) == QH_CORE_PHASE_NORMAL,
		"4KiB Recovery returns to Normal");
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- redundant superblock mount behavior --- */

static int TestSuperblockRedundancy(void)
{
	TEST_CTX ctx;
	PQH_CORE remounted = NULL;
	PUCHAR journalBytes;
	UCHAR a[512];
	UCHAR b[512];
	UINT64 oldest = 0;
	UINT64 newest = 0;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 27000)),
		"setup redundant superblock test");
	FillPattern(a, sizeof(a), 0x71);
	FillPattern(b, sizeof(b), 0x72);
	Expect(NT_SUCCESS(ctx.Source->Write(ctx.Source, 0, sizeof(a), a)),
		"seed source before superblock test");
	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 0, sizeof(b), b)),
		"append record before superblock test");

	journalBytes = (PUCHAR)QhMemStoreData(ctx.Journal);
	QhCoreDestroy(ctx.Core);
	ctx.Core = NULL;
	journalBytes[0] ^= 0xFF; /* corrupt primary superblock magic */
	Expect(NT_SUCCESS(QhCoreCreate(ctx.Source, ctx.Journal, &remounted)),
		"recreate after primary superblock corruption");
	st = QhCoreMountJournal(remounted);
	Expect(NT_SUCCESS(st), "mount falls back to backup superblock");
	st = QhCoreQueryTimeRange(remounted, &oldest, &newest);
	Expect(NT_SUCCESS(st) && oldest <= newest,
		"backup superblock preserves journal records");
	QhCoreDestroy(remounted);
	remounted = NULL;

	// QHJournalClose() may rewrite superblocks on successful mount.
	// Corrupt both primary+backup again to simulate "both invalid".
	journalBytes[0] = 0; /* corrupt primary superblock magic */
	journalBytes[ctx.Journal->Size - SECTOR] = 0; /* corrupt backup superblock magic */
	Expect(NT_SUCCESS(QhCoreCreate(ctx.Source, ctx.Journal, &remounted)),
		"recreate after both superblocks corrupted");
	st = QhCoreMountJournal(remounted);
	Expect(st == STATUS_DISK_CORRUPT_ERROR,
		"mount rejects journal when both superblocks are invalid");
	QhCoreDestroy(remounted);
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

/* --- journal append failure must not update the live source --- */

static int TestJournalWriteFailureDoesNotWriteSource(void)
{
	TEST_CTX ctx;
	TEST_FAIL_STORE fail;
	UCHAR a[512];
	UCHAR b[512];
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 28000)),
		"setup journal write failure test");
	FillPattern(a, sizeof(a), 0x81);
	FillPattern(b, sizeof(b), 0x82);
	Expect(NT_SUCCESS(ctx.Source->Write(ctx.Source, 0, sizeof(a), a)),
		"seed source before failed COW write");

	TestFailStoreInstall(ctx.Journal, &fail);
	fail.FailNextWrites = 1;
	st = QhCoreWrite(ctx.Core, 0, sizeof(b), b);
	Expect(st == STATUS_IO_DEVICE_ERROR, "COW reports journal write failure");
	Expect(memcmp(QhMemStoreData(ctx.Source), a, sizeof(a)) == 0,
		"failed journal append leaves live source unchanged");
	TestFailStoreRemove(ctx.Journal, &fail);

	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 0, sizeof(b), b)),
		"COW succeeds after fault removal");
	Expect(memcmp(QhMemStoreData(ctx.Source), b, sizeof(b)) == 0,
		"live source updates after successful journal append");
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestPreparedRecoveryCommitCancel(void)
{
	TEST_CTX ctx;
	UCHAR a[512];
	UCHAR b[512];
	UCHAR c[512];
	UCHAR n[512];
	UCHAR x[512];
	UCHAR y[512];
	UCHAR out[512];
	UINT64 t0;
	UINT64 tx;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 29000)),
		"setup prepared recovery test");
	FillPattern(a, sizeof(a), 0x10);
	FillPattern(b, sizeof(b), 0x20);
	FillPattern(c, sizeof(c), 0x30);
	FillPattern(n, sizeof(n), 0x40);
	FillPattern(x, sizeof(x), 0x50);
	FillPattern(y, sizeof(y), 0x60);

	Expect(NT_SUCCESS(ctx.Source->Write(ctx.Source, 0, sizeof(a), a)),
		"seed prepared recovery source A");
	t0 = QhCoreGetTime100ns(ctx.Core);
	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 0, sizeof(b), b)),
		"COW write B before prepare");
	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 0, sizeof(c), c)),
		"COW write C before prepare");

	st = QhCoreRecoveryBegin(ctx.Core, t0);
	Expect(NT_SUCCESS(st), "prepare recovery without writeback");
	Expect(QhCoreGetPhase(ctx.Core) == QH_CORE_PHASE_RECOVERY,
		"prepare remains in Recovery phase");
	Expect(memcmp(QhMemStoreData(ctx.Source), c, sizeof(c)) == 0,
		"prepare leaves physical source unchanged");
	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, a, sizeof(a)) == 0,
		"prepared read serves target-time history");

	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 0, sizeof(n), n)),
		"new write is allowed while recovery is prepared");
	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, n, sizeof(n)) == 0,
		"new prepared-phase write wins over history");
	Expect(NT_SUCCESS(QhCoreRecoveryCommit(ctx.Core)),
		"commit prepared recovery");
	Expect(QhCoreGetPhase(ctx.Core) == QH_CORE_PHASE_NORMAL,
		"commit returns to Normal");
	Expect(memcmp(QhMemStoreData(ctx.Source), n, sizeof(n)) == 0,
		"commit preserves prepared-phase new write");

	Expect(NT_SUCCESS(ctx.Source->Write(ctx.Source, 512, sizeof(x), x)),
		"seed cancel range X");
	tx = QhCoreGetTime100ns(ctx.Core);
	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 512, sizeof(y), y)),
		"COW write Y before cancel test");
	Expect(NT_SUCCESS(QhCoreRecoveryBegin(ctx.Core, tx)),
		"prepare recovery for cancel");
	memset(out, 0, sizeof(out));
	Expect(NT_SUCCESS(QhCoreRead(ctx.Core, 512, sizeof(out), out)) &&
			memcmp(out, x, sizeof(x)) == 0,
		"prepared cancel view serves X");
	Expect(NT_SUCCESS(QhCoreRecoveryCancel(ctx.Core)),
		"cancel prepared recovery");
	Expect(QhCoreGetPhase(ctx.Core) == QH_CORE_PHASE_NORMAL,
		"cancel returns to Normal");
	Expect(memcmp((PUCHAR)QhMemStoreData(ctx.Source) + 512, y, sizeof(y)) == 0,
		"cancel performs no physical writeback");
	Expect(QhCoreRecoveryCommit(ctx.Core) == STATUS_INVALID_DEVICE_STATE,
		"commit requires prepared Recovery phase");

	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

static int TestFullyCoveredRecoveryReadSkipsSource(void)
{
	TEST_CTX ctx;
	TEST_FAIL_STORE sourceFail;
	UCHAR a[512];
	UCHAR b[512];
	UCHAR out[512];
	UINT64 t0;
	NTSTATUS st;

	Expect(NT_SUCCESS(TestCtxCreate(&ctx, SRC_SIZE, JNL_SIZE, 30000)),
		"setup fully covered recovery read");
	FillPattern(a, sizeof(a), 0x91);
	FillPattern(b, sizeof(b), 0x92);
	Expect(NT_SUCCESS(ctx.Source->Write(ctx.Source, 0, sizeof(a), a)),
		"seed fully covered source A");
	t0 = QhCoreGetTime100ns(ctx.Core);
	Expect(NT_SUCCESS(QhCoreWrite(ctx.Core, 0, sizeof(b), b)),
		"journal full-block before-image A");
	Expect(NT_SUCCESS(QhCoreRecoveryBegin(ctx.Core, t0)),
		"prepare fully covered recovery");

	TestFailStoreInstall(ctx.Source, &sourceFail);
	sourceFail.FailNextReads = 1;
	memset(out, 0, sizeof(out));
	st = QhCoreRead(ctx.Core, 0, sizeof(out), out);
	Expect(NT_SUCCESS(st) && memcmp(out, a, sizeof(a)) == 0,
		"fully covered recovery read uses journal only");
	Expect(sourceFail.ReadCallCount == 0 && sourceFail.FailNextReads == 1,
		"fully covered recovery read skips live source");
	TestFailStoreRemove(ctx.Source, &sourceFail);

	Expect(NT_SUCCESS(QhCoreRecoveryCancel(ctx.Core)),
		"cancel fully covered recovery");
	TestCtxDestroy(&ctx);
	return g_caseFailed;
}

int main(void)
{
	int failed = 0;

	failed += RunCase("COW / Preview / Recovery", TestCowPreviewRecovery);
	failed += RunCase("Recovery concurrent write Invalid", TestRecoveryInvalidate);
	failed += RunCase("CaptureAppend (driver path)", TestCaptureAppendPath);
	failed += RunCase("Preview mixed journal+live read", TestMixedReadPartialCoverage);
	failed += RunCase("Multi-offset and dedup", TestMultiOffsetAndDedup);
	failed += RunCase("QueryTimeRange and Mount", TestQueryTimeRangeAndMount);
	failed += RunCase("Errors and phase guards", TestErrorsAndPhaseGuards);
	failed += RunCase("Preview write during session", TestPreviewWriteDuringSession);
	failed += RunCase("Preview before any COW", TestPreviewBeforeAnyCow);
	failed += RunCase("Preview Staging merge", TestPreviewStagingMerge);
	failed += RunCase("Recovery Staging punch", TestRecoveryStagingPunch);
	failed += RunCase("Recovery writeback overlap", TestRecoveryWritebackOverlap);
	failed += RunCase("Unmounted journal guards", TestUnmountedJournal);
	failed += RunCase("QueryTimeRange empty journal", TestQueryTimeRangeEmptyJournal);
	failed += RunCase("QueryTimeRange wall clocks", TestQueryTimeRangeWallClocks);
	failed += RunCase("QueryTimeRange after eviction", TestQueryTimeRangeAfterEviction);
	failed += RunCase("QueryTimeRange guards", TestQueryTimeRangeGuards);
	failed += RunCase("Recovery time boundaries", TestRecoveryTimeBoundaries);
	failed += RunCase("Preview target at/after newest", TestPreviewTargetAtOrAfterNewest);
	failed += RunCase("Journal append too large", TestJournalAppendTooLarge);
	failed += RunCase("Journal append max size", TestJournalAppendMaxSize);
	failed += RunCase("CaptureAppend source bounds", TestCaptureAppendSourceBounds);
	failed += RunCase("Mount unformatted journal", TestMountUnformattedJournal);
	failed += RunCase("Recovery WritebackActive", TestRecoveryWritebackActive);
	failed += RunCase("Journal drop oldest (ring full)", TestJournalDropOldest);
	failed += RunCase("Journal payload tail wrap", TestJournalPayloadTailWrap);
	failed += RunCase("Journal tiny partition large record", TestJournalTinyPartitionLargeRecord);
	failed += RunCase("Journal format too small", TestJournalFormatTooSmall);
	failed += RunCase("Journal format write chunk fallback", TestJournalFormatWriteChunkFallback);
	failed += RunCase("4KiB-sector COW / Preview / Recovery", TestFourKiBSectorCowPreviewRecovery);
	failed += RunCase("Journal superblock redundancy", TestSuperblockRedundancy);
	failed += RunCase("Journal failure keeps live source unchanged", TestJournalWriteFailureDoesNotWriteSource);
	failed += RunCase("Prepared Recovery commit/cancel", TestPreparedRecoveryCommitCancel);
	failed += RunCase("Fully covered Recovery read", TestFullyCoveredRecoveryReadSkipsSource);

	printf("\n%s (%d failures)\n", failed ? "FAILED" : "ALL PASSED", failed);
	return failed ? 1 : 0;
}
