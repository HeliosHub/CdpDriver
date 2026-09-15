#pragma once

/*
 * 授权代码页完整性（LICENSE_DESIGN.md §9 O-16 / O-17）。
 *
 * CdpLicenseProtectCapture   : DriverEntry 对已加载映像的 .licprot/.licpr 做 CRC32C 快照
 * CdpLicenseProtectVerify    : 还原 XOR 分片中的期望 CRC，与现场重算比较
 * CdpLicenseProtectComputeCrc: 仅重算现场 CRC，供 Gate.c 镜像副本使用
 * CdpLicenseProtectInvalidate: 卸载时作废快照
 *
 * 使用 CRC32C 而非 BCrypt SHA256：Boot-start 的 DriverEntry 阶段 CNG 可能尚未可用。
 * 校验失败只拒绝回滚/重做，不 KeBugCheck。
 */

#ifdef CDP_LICENSE
#ifdef CDP_LICENSE_OBFUSCATE

NTSTATUS CdpLicenseProtectCapture(_In_opt_ PVOID ImageBase);
NTSTATUS CdpLicenseProtectVerify(VOID);
ULONG CdpLicenseProtectComputeCrc(VOID);
VOID CdpLicenseProtectInvalidate(VOID);

#endif /* CDP_LICENSE_OBFUSCATE */
#endif /* CDP_LICENSE */
