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

#include <ntddk.h>

// 后期会实现真正的哈希树
// 我本来是拿了klib的khashl将其移植到R0, 但是不知道为什么问题频出
// 先拿RTL_GENERIC_TABLE凑合一下
// 这样也好, 也省了klib的开源协议对冲风险, 后期我自己实现一个哈希表

// 重定向映射对：原始扇区索引 → 目标扇区索引
// 使用 RTL_GENERIC_TABLE (内核自带Splay树)
typedef struct _QH_REDIRECT_PAIR
{
	UINT64 OrgSectorIndex;		// 原始扇区索引 (Splay树排序键)
	UINT64 TargetSectorIndex;	// 重定向目标扇区索引
} QH_REDIRECT_PAIR, *PQH_REDIRECT_PAIR;

// 使用 RTL_GENERIC_TABLE (内核自带Splay树)
typedef struct _QH_OFFSET_HASH
{
	RTL_GENERIC_TABLE Table;
} QH_OFFSET_HASH, *PQH_OFFSET_HASH;

typedef BOOLEAN(*QHHashForeachCallback)(UINT64 Key, UINT64 TargetSectorIndex);

PQH_OFFSET_HASH QHHashCreate();

NTSTATUS QHHashSet(_In_ PQH_OFFSET_HASH Hash, _In_ UINT64 Key, _In_ UINT64 TargetSectorIndex);
NTSTATUS QHHashGet(_In_ PQH_OFFSET_HASH Hash, _In_ UINT64 Key, _Out_ PUINT64 TargetSectorIndex);
NTSTATUS QHHashRemove(_In_ PQH_OFFSET_HASH Hash, _In_ UINT64 Key);
VOID QHHashClear(_In_ PQH_OFFSET_HASH Hash);
VOID QHHashForeach(_In_ PQH_OFFSET_HASH Hash, _In_ QHHashForeachCallback Callback);
VOID QHHashDelete(_In_ PQH_OFFSET_HASH Hash);
