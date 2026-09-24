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
 * P requests IMAGE_SCN_MEM_NOT_PAGED from the linker. The verifier treats a
 * missing bit as a Release-build failure, so protected code/data remains safe
 * if a future call path reaches it above PASSIVE_LEVEL.
 *
 * #pragma comment(linker, "/SECTION:...") 保证链接器按上述属性创建节，
 * 即使某个翻译单元暂时没有输入也不会默认为可写。
 *
 * 禁止把本头包含进 CdpIrpDispatchs.c / CdpJournal.c / cdp_core.c / Driver.c，
 * 否则 COW 热路径会被算进完整性范围，且补丁 COW 会误杀授权校验。
 * （CdpLicenseCodec.c 例外：它虽被 IOCTL 路径间接调用，但承担 canonical 待验签
 *   字节与有效性判定，属于判定实现本身，必须进节。）
 *
 * ---------------------------------------------------------------------------
 * 【重要】保护边界规则（2026-09 补充，源于一次真实的边界错位）
 *
 * 只有"进节 + 被 CRC 覆盖"的代码才受 O-16 保护。曾经出现过两类错位：
 *
 *   1) 授权判定的**调用点**留在节外。CdpIrpDispatchs.c 处于 I/O 热路径，
 *      按上面的规则不能进节；但 BEGIN/COMMIT RECOVERY 的闸门调用恰好在那里，
 *      于是攻击者只要改那一处返回码判断，CdpLicenseGate / CdpLocalSeal /
 *      CdpLicenseProtect 里的全部机制（含本节的 CRC 校验）一次都不会执行。
 *      → 解法：新增 CdpLicenseGateCall.c（只做转发，进节 + 混淆），
 *        热路径文件改为调用它。**任何"判定是否放行"的代码，其调用点必须在节内。**
 *
 *   2) 参与判定的**解析/判定实现**留在节外。CdpLicenseCodec.c 定义 canonical
 *      待验签字节与"证件是否有效"，却曾不在 .licprot 内。
 *      → 解法：把 codec 整单元链入本节。
 *
 * 因此新增授权相关代码时请自问：
 *   - 它是否参与"放行 / 拒绝"的判定？         → 必须在节内
 *   - 它的调用点是否在节外的热路径文件里？     → 用节内的转发包装
 *   - 它是否引入新的 static const 数据？       → 会进入 .licpr，需复核尺寸
 * 改动混淆配置后请跑 script/verify_obfuscation.ps1（它按构建产物断言逐单元
 * 的 pass 集合、节属性与明文串，能把"少传一个 flag"变成构建失败）。
 * ---------------------------------------------------------------------------
 */
#if defined(CDP_LICENSE) && defined(CDP_LICENSE_OBFUSCATE)
#pragma code_seg(".licprot")
#pragma const_seg(".licpr")
#pragma comment(linker, "/SECTION:.licprot,ERP")
#pragma comment(linker, "/SECTION:.licpr,RP")
#endif
