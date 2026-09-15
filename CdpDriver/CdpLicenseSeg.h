#pragma once

/*
 * 把「须保护的授权编译单元」链进独立 PE 节，供 O-16 完整性哈希覆盖。
 *
 * 用法：各 CdpLicense*.c / CdpLocalSeal.c 在普通 include 之后、可执行代码之前
 *       #include "CdpLicenseSeg.h"，文件末尾再 #include "CdpLicenseSegEnd.h"。
 *
 * 节名必须 ≤8 字节（PE 限制）：
 *   .licprot  代码，ER = 可执行 + 只读（禁止 W，避免 HVCI 反感的 RWX）
 *   .licpr    只读常量（公钥分片、PRODUCT_MAGIC 等）
 *
 * #pragma comment(linker, "/SECTION:...") 保证链接器按上述属性创建节，
 * 即使某个翻译单元暂时没有输入也不会默认为可写。
 *
 * 禁止把本头包含进 CdpIrpDispatchs.c / CdpJournal.c / cdp_core.c / Driver.c，
 * 否则 COW 热路径会被算进完整性范围，且补丁 COW 会误杀授权校验。
 */
#if defined(CDP_LICENSE) && defined(CDP_LICENSE_OBFUSCATE)
#pragma code_seg(".licprot")
#pragma const_seg(".licpr")
#pragma comment(linker, "/SECTION:.licprot,ER")
#pragma comment(linker, "/SECTION:.licpr,R")
#endif
