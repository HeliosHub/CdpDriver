/*
 * 授权节完整性快照与对照（LICENSE_DESIGN.md §9 O-16 / O-17）。
 *
 * 思路：
 *   1) 授权代码/常量被编译进独立节 .licprot（可执行）与 .licpr（只读常量）。
 *   2) DriverEntry 在映像已完成重定位后，对这两节做 CRC32C，得到「加载后快照」。
 *   3) 快照不把 CRC 明文放在一处：拆成两套 XOR 分片（Xa/Xb 与 Ya/Yb），
 *      校验时先还原 mix 常量、再还原两份期望 CRC，必须一致，再与现场重算比较。
 *   4) 此后每次回滚闸门 / Arm / Commit / 小时定时器再走一遍对照。
 *
 * 能防：加载之后用调试器 patch .licprot/.licpr。
 * 不能防：加载前改磁盘上的 .sys（靠 Authenticode / EV 签名）。
 * CRC32C 不是密码学哈希，只提高改「期望值」的成本；失败只拒绝回滚，不蓝屏。
 *
 * 启动阶段可能还没有 CNG，因此这里不用 BCrypt SHA256。
 */

#ifdef CDP_LICENSE
#ifdef CDP_LICENSE_OBFUSCATE

#include "CdpLicenseDefs.h"
#include "CdpLocalSeal.h"
#include "CdpLicenseProtect.h"
#include "CdpLicenseSeg.h" /* 本文件自身也在被哈希的 .licprot 内 */

#ifndef IMAGE_DOS_SIGNATURE
#define IMAGE_DOS_SIGNATURE 0x5A4D     /* DOS MZ */
#endif
#ifndef IMAGE_NT_SIGNATURE
#define IMAGE_NT_SIGNATURE 0x00004550  /* PE\0\0 */
#endif
#ifndef IMAGE_SIZEOF_SHORT_NAME
#define IMAGE_SIZEOF_SHORT_NAME 8      /* PE 节名固定 8 字节，不足补 0 */
#endif

/* 单节上限：授权代码远小于此；用来挡住损坏的 VirtualSize 把哈希拖进无关页 */
#define Cdp_LICPROT_MAX_SECTION  (2u * 1024u * 1024u)
#define Cdp_LICPROT_MIX1         0x6C696370UL /* ASCII 'licp'，参与分片 XOR */
#define Cdp_LICPROT_MIX2         0xC0DEC0DEUL /* 第二份混合常数，与 MIX1 交叉存放 */

/*
 * 下面只解析完整性哈希需要的 PE 字段，避免依赖 ntimage.h 在启动驱动里的差异。
 * pack(1) 必须与磁盘/映像中的 PE 布局一致，否则 e_lfanew / OptionalHeader 大小会读偏。
 */
#pragma pack(push, 1)
typedef struct _Cdp_IMAGE_DOS_HEADER
{
	USHORT e_magic;
	USHORT e_cblp;
	USHORT e_cp;
	USHORT e_crlc;
	USHORT e_cparhdr;
	USHORT e_minalloc;
	USHORT e_maxalloc;
	USHORT e_ss;
	USHORT e_sp;
	USHORT e_csum;
	USHORT e_ip;
	USHORT e_cs;
	USHORT e_lfarlc;
	USHORT e_ovno;
	USHORT e_res[4];
	USHORT e_oemid;
	USHORT e_oeminfo;
	USHORT e_res2[10];
	LONG e_lfanew;                 /* PE 头相对 ImageBase 的偏移 */
} Cdp_IMAGE_DOS_HEADER;

typedef struct _Cdp_IMAGE_FILE_HEADER
{
	USHORT Machine;
	USHORT NumberOfSections;
	ULONG TimeDateStamp;
	ULONG PointerToSymbolTable;
	ULONG NumberOfSymbols;
	USHORT SizeOfOptionalHeader;   /* 用来跳过 Optional Header 落到节表 */
	USHORT Characteristics;
} Cdp_IMAGE_FILE_HEADER;

typedef struct _Cdp_IMAGE_SECTION_HEADER
{
	UCHAR Name[IMAGE_SIZEOF_SHORT_NAME];
	union
	{
		ULONG PhysicalAddress;
		ULONG VirtualSize;         /* 内存中有效大小；哈希用这个而不是 SizeOfRawData */
	} Misc;
	ULONG VirtualAddress;          /* RVA，加载后地址 = ImageBase + RVA */
	ULONG SizeOfRawData;
	ULONG PointerToRawData;
	ULONG PointerToRelocations;
	ULONG PointerToLinenumbers;
	USHORT NumberOfRelocations;
	USHORT NumberOfLinenumbers;
	ULONG Characteristics;
} Cdp_IMAGE_SECTION_HEADER;

/* 完整性只需 Signature + FileHeader；Optional Header 按 SizeOfOptionalHeader 跳过 */
typedef struct _Cdp_IMAGE_NT_HEADERS
{
	ULONG Signature;
	Cdp_IMAGE_FILE_HEADER FileHeader;
} Cdp_IMAGE_NT_HEADERS;
#pragma pack(pop)

static PVOID g_CdpLicImageBase = NULL; /* DriverObject->DriverStart，映射后的映像基址 */
/*
 * CRC 分片（均在普通 .data，故意不放进被哈希的 .licpr，否则写入快照会改变哈希）：
 *   Capture 时：
 *     Xa = crc ^ MIX1
 *     Xb = MIX1 ^ MIX2
 *     Ya = crc ^ MIX2
 *     Yb = MIX2 ^ MIX1
 *   Verify 时反向还原，两套期望必须相同。
 */
static ULONG g_CdpLicCrcXa = 0;
static ULONG g_CdpLicCrcXb = 0;
static ULONG g_CdpLicCrcYa = 0;
static ULONG g_CdpLicCrcYb = 0;
static volatile LONG g_CdpLicProtectReady = 0; /* 0=未捕获或已作废；1=可校验 */

/*
 * PE 节名恰好 8 字节且不以 NUL 结尾（".licprot" 正好 8 字符）。
 * 把期望名右边补 0 后做逐字节比较，避免 strcmp 在无终止符节名上越读。
 */
static BOOLEAN CdpLicenseProtectNameEq(
	_In_reads_(IMAGE_SIZEOF_SHORT_NAME) const UCHAR* Name,
	_In_ const CHAR* Expect)
{
	UCHAR expectPad[IMAGE_SIZEOF_SHORT_NAME];
	SIZE_T n;
	SIZE_T i;

	RtlZeroMemory(expectPad, sizeof(expectPad));
	n = 0;
	while (Expect[n] && n < IMAGE_SIZEOF_SHORT_NAME)
	{
		expectPad[n] = (UCHAR)Expect[n];
		++n;
	}
	for (i = 0; i < IMAGE_SIZEOF_SHORT_NAME; ++i)
	{
		if (Name[i] != expectPad[i])
			return FALSE;
	}
	return TRUE;
}

/*
 * 遍历映像节表，对 .licprot 与 .licpr 做链式 CRC32C：
 *   crc_new = CRC32C(crc_old, section_bytes)
 * 两节都找到才返回非 0；缺节、Size 异常或 DOS/PE 魔数不对一律返回 0（调用方视为失败）。
 */
static ULONG CdpLicenseProtectHashImage(_In_ PVOID ImageBase)
{
	const Cdp_IMAGE_DOS_HEADER* dos;
	const Cdp_IMAGE_NT_HEADERS* nt;
	const Cdp_IMAGE_SECTION_HEADER* sec;
	ULONG i;
	ULONG crc = 0;
	ULONG found = 0;

	if (!ImageBase)
		return 0;

	dos = (const Cdp_IMAGE_DOS_HEADER*)ImageBase;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
		return 0;

	/* NT 头 = ImageBase + e_lfanew */
	nt = (const Cdp_IMAGE_NT_HEADERS*)((PUCHAR)ImageBase + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.NumberOfSections == 0)
		return 0;

	/*
	 * 节表紧跟 Optional Header：
	 *   nt + sizeof(Signature) + sizeof(FileHeader) + SizeOfOptionalHeader
	 */
	sec = (const Cdp_IMAGE_SECTION_HEADER*)(
		(PUCHAR)nt + sizeof(ULONG) + sizeof(Cdp_IMAGE_FILE_HEADER) +
		nt->FileHeader.SizeOfOptionalHeader);

	for (i = 0; i < nt->FileHeader.NumberOfSections; ++i)
	{
		ULONG size;
		const UCHAR* start;

		if (!CdpLicenseProtectNameEq(sec[i].Name, ".licprot") &&
			!CdpLicenseProtectNameEq(sec[i].Name, ".licpr"))
		{
			continue;
		}
		size = sec[i].Misc.VirtualSize;
		if (size == 0)
			size = sec[i].SizeOfRawData;
		if (size == 0 || size > Cdp_LICPROT_MAX_SECTION)
			return 0;
		/* 加载后 VA = ImageBase + RVA；哈希内存映像（已含重定位） */
		start = (const UCHAR*)ImageBase + sec[i].VirtualAddress;
		crc = CdpLicenseCrc32c(crc, start, size);
		found += 1;
	}
	if (found == 0)
		return 0;
	return crc;
}

/* 给 Gate.c 镜像副本用：用 Capture 时保存的基址重算现场 CRC。 */
ULONG CdpLicenseProtectComputeCrc(VOID)
{
	return CdpLicenseProtectHashImage(g_CdpLicImageBase);
}

/*
 * DriverEntry 调用：记下 ImageBase，计算 CRC，拆成双份 XOR 分片。
 * 失败返回 TAMPER，调用方不得因此卸载驱动（COW 仍要工作），但之后闸门会 fail-closed。
 */
NTSTATUS CdpLicenseProtectCapture(_In_opt_ PVOID ImageBase)
{
	ULONG crc;
	ULONG mix1 = Cdp_LICPROT_MIX1;
	ULONG mix2 = Cdp_LICPROT_MIX2;

	g_CdpLicImageBase = ImageBase;
	InterlockedExchange(&g_CdpLicProtectReady, 0);
	crc = CdpLicenseProtectHashImage(ImageBase);
	if (crc == 0)
	{
		Cdp_LIC_FAIL("protect capture failed (no .licprot/.licpr or empty)");
		return STATUS_CDP_LICENSE_TAMPER;
	}

	/* 两套独立编码：改其中一套分片无法同时骗过 Verify 的双重还原 */
	g_CdpLicCrcXa = crc ^ mix1;
	g_CdpLicCrcXb = mix1 ^ mix2;
	g_CdpLicCrcYa = crc ^ mix2;
	g_CdpLicCrcYb = mix2 ^ mix1;
	InterlockedExchange(&g_CdpLicProtectReady, 1);
	return STATUS_SUCCESS;
}

/*
 * 还原分片并与现场哈希比较。
 * InterlockedCompareExchange(x,0,0) 只读当前 Ready：非 1 视为未捕获。
 */
NTSTATUS CdpLicenseProtectVerify(VOID)
{
	ULONG crc;
	ULONG expect1;
	ULONG expect2;
	ULONG mix1;
	ULONG mix2;

	if (InterlockedCompareExchange(&g_CdpLicProtectReady, 0, 0) == 0)
	{
		Cdp_LIC_FAIL("protect not captured");
		return STATUS_CDP_LICENSE_TAMPER;
	}

	/* Xb = MIX1 ^ MIX2  ⇒  mix2 = Xb ^ MIX1，必须还原回编译期 MIX2 */
	mix2 = g_CdpLicCrcXb ^ Cdp_LICPROT_MIX1;
	if (mix2 != Cdp_LICPROT_MIX2)
	{
		Cdp_LIC_FAIL("protect shard mix tamper");
		return STATUS_CDP_LICENSE_TAMPER;
	}
	/* Yb = MIX2 ^ MIX1  ⇒  mix1 = Yb ^ mix2 */
	mix1 = g_CdpLicCrcYb ^ mix2;
	if (mix1 != Cdp_LICPROT_MIX1)
	{
		Cdp_LIC_FAIL("protect shard mix2 tamper");
		return STATUS_CDP_LICENSE_TAMPER;
	}

	/* Xa = crc ^ MIX1、Ya = crc ^ MIX2  ⇒  两套期望必须相同 */
	expect1 = g_CdpLicCrcXa ^ mix1;
	expect2 = g_CdpLicCrcYa ^ mix2;
	if (expect1 != expect2)
	{
		Cdp_LIC_FAIL("protect dual-copy mismatch");
		return STATUS_CDP_LICENSE_TAMPER;
	}

	crc = CdpLicenseProtectHashImage(g_CdpLicImageBase);
	if (crc == 0 || crc != expect1)
	{
		Cdp_LIC_FAIL("protect crc mismatch got=%08X expect=%08X", crc, expect1);
		return STATUS_CDP_LICENSE_TAMPER;
	}
	return STATUS_SUCCESS;
}

/* 卸载时清快照，避免残留基址指向已卸载映像。 */
VOID CdpLicenseProtectInvalidate(VOID)
{
	InterlockedExchange(&g_CdpLicProtectReady, 0);
	g_CdpLicImageBase = NULL;
	g_CdpLicCrcXa = 0;
	g_CdpLicCrcXb = 0;
	g_CdpLicCrcYa = 0;
	g_CdpLicCrcYb = 0;
}

#include "CdpLicenseSegEnd.h" /* 恢复默认 code/const 节 */

#endif /* CDP_LICENSE_OBFUSCATE */
#endif /* CDP_LICENSE */

#if !defined(CDP_LICENSE) || !defined(CDP_LICENSE_OBFUSCATE)
/* Debug（无授权）占位，避免空翻译单元告警 */
static int CdpLicenseProtect_TU = 0;
#endif
