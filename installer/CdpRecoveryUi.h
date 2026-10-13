#pragma once

#include "CdpSecurePrompt.h"

/*
 * 忘记口令：向 CdpDriver 要二维码，导入凭据后在同一句柄上重设口令。
 * 成功时 NewPassword 是用户刚设置的新口令，调用方用它继续维护授权。
 */
BOOL CdpRunPasswordRecovery(_Out_ CDP_SECURE_PASSWORD *NewPassword);
