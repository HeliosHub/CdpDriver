/*
 * Copyright 2026 Xuhui Jiang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#define QH_CONTROL_DEVICE_NAME L"\\Device\\QHEngineControlDevice"
#define QH_CONTROL_SYSTEM_LINK_NAME L"\\DosDevices\\QHEngineControlDevice"
#else
#include <Windows.h>
#define QH_CONTROL_SYSTEM_LINK_NAME L"\\\\.\\QHEngineControlDevice"
#endif

// 保护状态文件路径（相对卷根目录）
// 文件存在 + 首字节 = 1 → 保护开启
// 文件存在 + 首字节 = 0 → 用户已关闭保护
// 文件不存在            → 此卷未配置保护
// 该文件被加入 ProtectRanges, 写入时直通真实磁盘, 不被重定向
#define QH_PROTECT_STATE_FILE_NAME L"\\_qh_protect_state.data"

// 保护状态文件大小（字节）
// 必须远大于 NTFS 驻留属性阈值（~700B）以强制非驻留存储,
// 否则数据直接放在 MFT 记录里, ProtectRanges 标记的"文件扇区"会变成空,
// 写入将走 MFT 而被本驱动重定向
#define QH_PROTECT_STATE_FILE_SIZE (1024 * 1024)

// 自定义 IOCTL 控制码
#define QH_IOCTL_TYPE 0x8000

// 查询是否有任意卷处于保护状态
// 遍历过滤设备链表，任一卷 Initialized==TRUE 即返回 TRUE
// 输入: 无
// 输出: BOOLEAN (1 字节)
#define IOCTL_QH_QUERY_PROTECT_STATUS CTL_CODE(QH_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)



