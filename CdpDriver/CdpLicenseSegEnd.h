#pragma once

/*
 * 恢复默认 code/const 节。必须在授权 .c 的末尾、#endif CDP_LICENSE 之前包含，
 * 避免后续（若有）代码被继续排进 .licprot。
 */
#if defined(CDP_LICENSE) && defined(CDP_LICENSE_OBFUSCATE)
#pragma code_seg()
#pragma const_seg()
#endif
