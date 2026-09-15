#pragma once

/*
 * ============================================================================
 * 授权子系统公共定义（LICENSE_DESIGN.md v3.1）
 * ============================================================================
 *
 * 编译开关：
 *   - Debug      ：不定义 CDP_LICENSE → 本文件及全部授权路径不参与编译
 *   - Debug-Lic  ：CDP_LICENSE + CDP_LICENSE_OBFUSCATE → 带授权的调试构建
 *   - Release    ：同上，生产启用授权
 *
 * CDP_LICENSE_OBFUSCATE：
 *   仅用于圈出「须 CFF/混淆后再链接」的编译单元（设计文档 §9 O-01～O-15）。
 *   宏本身不做混淆，方便后续用工具只处理这些单元。
 */

#ifdef CDP_LICENSE

#ifndef _CDP_LICENSE_DEFS_TYPES_READY_
#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif
#endif

/* ---- 容量与协议常量（与服务端 cdpserver 对齐）---- */

#define Cdp_LICENSE_FORMAT_VERSION       2u   /* 当前签发版本（含 kind） */
#define Cdp_LICENSE_FORMAT_VERSION_MIN   1u   /* 仍接受无 kind 的 v1 盘上证书 */
#define Cdp_LICENSE_FORMAT_VERSION_MAX   2u
#define Cdp_LICENSE_BLOB_MAX             2048u /* 超级块内 License 原始字节上限 */
#define Cdp_E0_SEAL_MAX                  128u  /* E0 密封后字节上限（头+密文+tag） */
#define Cdp_LICENSE_FP_BYTES             32u   /* SHA-256 设备指纹 */
#define Cdp_LICENSE_MB_UUID_CHARS        64u
#define Cdp_LICENSE_DISK_SERIAL_CHARS    128u
#define Cdp_LICENSE_ID_CHARS             64u
#define Cdp_APPLY_QR_PAYLOAD_MAX         4096u /* 申请二维码 URI 字符串 */
#define Cdp_APPLY_QR_PREFIX_MAX          256u  /* 应用层传入的扫码 URI 前缀 */
#define Cdp_APPLY_CIPHERTEXT_MAX         3072u /* 混合加密申请包 */
#define Cdp_APPLY_PLAINTEXT_MAX          1536u

/* 与服务端 apply_sessions.mode / licenses.mode 一致 */
#define Cdp_LICENSE_MODE_TIME            1u /* 时长模式 */
#define Cdp_LICENSE_MODE_COUNTER         2u /* 次数模式 */
#define Cdp_LICENSE_MODE_HYBRID          3u /* 混合 */

/* blob.kind / 申请明文 kind；与 apply_sessions.kind 一致 */
#define Cdp_LICENSE_KIND_PAID            0u
#define Cdp_LICENSE_KIND_TRIAL           1u

#ifndef Cdp_LICENSE_QUERY_REPLY_V1_BYTES
#define Cdp_LICENSE_QUERY_REPLY_V1_BYTES 40u /* QUERY_REPLY 在增加 IsTrial 之前的大小 */
#endif

/* E0 密封头 magic = 'E0S1'（小端） */
#define Cdp_E0_SEAL_MAGIC                0x31533045UL
#define Cdp_E0_SEAL_VERSION              1u
#define Cdp_LOCAL_SEAL_VERSION           1u

/*
 * 启用授权后，Journal 超级块占用固定 4KB（按扇区对齐上取整），
 * 以容纳 LicenseBlob + E0；无授权构建仍用单扇区超级块。
 */
#define Cdp_JOURNAL_SUPERBLOCK_RESERVE   4096u
#define Cdp_CAP_TOKEN_BYTES              32u /* SHA-256 摘要长度，Capability Token 用 */

/*
 * 授权失败码必须包装为 FACILITY_NTWIN32（0xC007xxxx）。
 * 若使用自定义设施 0xC0CDxxxx，I/O 管理器 RtlNtStatusToDosError 无法映射，
 * 用户态 GetLastError() 会变成 317（ERROR_MR_MID_NOT_FOUND），GUI 无法区分原因。
 * 包装后 GetLastError() 等于下列 ERROR_CDP_LICENSE_*（与 CdpIoctl.h 保持一致）。
 */
#ifndef ERROR_CDP_LICENSE_REQUIRED
#define ERROR_CDP_LICENSE_REQUIRED    0x0000CD01L /* 52481 未导入/未加载 */
#define ERROR_CDP_LICENSE_EXPIRED     0x0000CD02L /* 52482 已过期 */
#define ERROR_CDP_LICENSE_EXHAUSTED   0x0000CD03L /* 52483 次数用尽 */
#define ERROR_CDP_LICENSE_TAMPER      0x0000CD04L /* 52484 本地状态被篡改 */
#define ERROR_CDP_LICENSE_INVALID     0x0000CD05L /* 52485 验签或绑机失败 */
#define ERROR_CDP_LICENSE_TRIAL_USED  0x0000CD06L /* 52486 试用证已用过，拒绝再导入 */
#endif
#ifndef ERROR_CDP_LICENSE_TRIAL_USED
#define ERROR_CDP_LICENSE_TRIAL_USED  0x0000CD06L
#endif

#define CDP_NTSTATUS_FROM_WIN32(Win32Error) \
	((NTSTATUS)((((ULONG)(Win32Error)) & 0x0000FFFFUL) | 0xC0070000UL))

#define STATUS_CDP_LICENSE_REQUIRED      CDP_NTSTATUS_FROM_WIN32(ERROR_CDP_LICENSE_REQUIRED)
#define STATUS_CDP_LICENSE_EXPIRED       CDP_NTSTATUS_FROM_WIN32(ERROR_CDP_LICENSE_EXPIRED)
#define STATUS_CDP_LICENSE_EXHAUSTED     CDP_NTSTATUS_FROM_WIN32(ERROR_CDP_LICENSE_EXHAUSTED)
#define STATUS_CDP_LICENSE_TAMPER        CDP_NTSTATUS_FROM_WIN32(ERROR_CDP_LICENSE_TAMPER)
#define STATUS_CDP_LICENSE_INVALID       CDP_NTSTATUS_FROM_WIN32(ERROR_CDP_LICENSE_INVALID)
#define STATUS_CDP_LICENSE_TRIAL_USED    CDP_NTSTATUS_FROM_WIN32(ERROR_CDP_LICENSE_TRIAL_USED)

#pragma pack(push, 1)

/*
 * E0 密封前明文（设计 §6.1），固定 32 字节 → Feistel CBC 两块。
 * OPS_T / OPS_S / A_MOD 必须与 T0/C0 一同密封，禁止明文单独落盘。
 */
typedef struct _Cdp_E0_PLAINTEXT
{
	UINT64 T0_100ns;     /* 本地单调时间水位（FILETIME 100ns） */
	ULONG C0;            /* 剩余回滚/重做次数 */
	ULONG OPS_T;         /* 本授权周期内尝试总次数 */
	ULONG OPS_S;         /* 本授权周期内成功次数 */
	ULONG A_MOD;         /* 检测到的内存篡改次数（单调不减；新证导入时清零） */
	USHORT SealVersion;  /* 明文布局版本，当前 = 1 */
	USHORT Reserved;
	ULONG Crc32c;        /* 以上字段的 CRC32C（不含本字段） */
} Cdp_E0_PLAINTEXT, *PCdp_E0_PLAINTEXT;

C_ASSERT(sizeof(Cdp_E0_PLAINTEXT) == 32);

#pragma pack(pop)

/*
 * 驱动全局授权状态（单机一份，镜像到各 Journal 超级块）。
 * g_* 为运行时内存水位；闸门前须与 E0 解密出的 l_* 对照（防改内存）。
 */
typedef struct _Cdp_LICENSE_STATE
{
	BOOLEAN Loaded;				/* 是否已加载E0 */
	BOOLEAN HasLicense;			/* 是否已加载License */
	BOOLEAN IsTrial;            /* blob.kind == trial（v1 无 kind 视为付费） */
	ULONG Mode;                 /* TIME / COUNTER / HYBRID */
	UINT64 T_EXP_100ns;         /* 证书结束时刻（派生 K 的输入之一） */
	UINT64 T0_Issue_100ns;      /* 证书签发起点 */
	ULONG C0_Initial;           /* 导入时的初始次数 */
	UINT64 g_T0;                /* 内存中的水位 */
	ULONG g_C0;					/* 内存中的剩余回滚/重做次数 */
	ULONG g_OPS_T;				/* 内存中的本授权周期内尝试总次数 */
	ULONG g_OPS_S;				/* 内存中的本授权周期内成功次数 */
	ULONG g_A_MOD;				/* 内存中的检测到的内存篡改次数（单调不减；新证导入时清零） */
	UCHAR DeviceFingerprint[Cdp_LICENSE_FP_BYTES];	/* 设备指纹 */
	CHAR LicenseId[Cdp_LICENSE_ID_CHARS];			/* License ID */
	CHAR MbUuid[Cdp_LICENSE_MB_UUID_CHARS];			/* MB UUID，获取方式是读取MB的GUID */
	CHAR DiskSerial[Cdp_LICENSE_DISK_SERIAL_CHARS];	/* 磁盘序列号，获取方式是读取磁盘的序列号 */
	ULONG LicenseBlobLength;							/* LicenseBlob的长度 */
	UCHAR LicenseBlob[Cdp_LICENSE_BLOB_MAX]; /* 服务端签发的 canonical 字节 */
	ULONG E0Length;								/* E0的长度 */
	UCHAR E0[Cdp_E0_SEAL_MAX];  /* 最近一次密封结果（与盘上一致的镜像） */
	UCHAR SealKey[32];          /* CdpLocalSeal 密钥 K；用毕应清零路径覆盖 */
	BOOLEAN SealKeyValid;       /* SealKey是否有效，如果为FALSE，则需要重新生成 */
	/*
	 * Capability Token（O-18）：仅内存，不写入 E0/超级块。
	 * GateBeforeOp 签发；GateArmOp 在 RecoveryBegin 前确认；AfterOpSuccess 再验后作废。
	 * PendingCapArmed=FALSE 时即使 token 字节正确也不允许 Commit 记账。
	 */
	UCHAR PendingCapToken[Cdp_CAP_TOKEN_BYTES]; /* SHA256 输出 */
	ULONG PendingCapNonce;                      /* 签发时的随机数，重算必须带上 */
	UINT64 PendingCapIssued100ns;               /* 签发时刻，绑定本次 Begin */
	BOOLEAN PendingCapValid;                    /* 已 Mint、尚未 Abort/After */
	BOOLEAN PendingCapArmed;                    /* ArmOp 已确认，允许 After 记账 */
} Cdp_LICENSE_STATE, *PCdp_LICENSE_STATE;

/*
 * 单次回滚/重做闸门使用的局部快照（从 E0 解密得到）。
 * 对照失败时改 l_A_MOD 并回写；成功路径在业务前后更新 OPS/C0。
 * Cap* 仅在 BeforeOp→ArmOp 的栈上传递，不落盘。
 */
typedef struct _Cdp_LICENSE_LOCAL_STATE
{
	UINT64 l_T0;
	ULONG l_C0;
	ULONG l_OPS_T;
	ULONG l_OPS_S;
	ULONG l_A_MOD;
	ULONG CapTokenValid;     /* 非 0 表示 CapToken/Nonce/Issued 已填充 */
	ULONG CapNonce;
	UINT64 CapIssued100ns;
	UCHAR CapToken[Cdp_CAP_TOKEN_BYTES];
} Cdp_LICENSE_LOCAL_STATE, *PCdp_LICENSE_LOCAL_STATE;


/*
 * 授权失败调试日志（仅 LIC_DEBUG 构建输出到 DbgView / WinDbg）。
 * 非 LIC_DEBUG 时宏为空操作，调用点无需再包 #ifdef。
 */
#ifdef LIC_DEBUG
#define Cdp_LIC_INFO(fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
		"[CdpLicense] INFO %s @ %s:%d: " fmt "\n", \
		__FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__)
#define Cdp_LIC_FAIL(fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
		"[CdpLicense] FAIL %s @ %s:%d: " fmt "\n", \
		__FUNCTION__, __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define Cdp_LIC_INFO(fmt, ...) ((void)0)
#define Cdp_LIC_FAIL(fmt, ...) ((void)0)
#endif

#endif /* CDP_LICENSE */

