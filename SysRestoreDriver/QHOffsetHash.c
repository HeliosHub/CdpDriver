// QHOffsetHash.c
// 使用 Windows 内核自带的 RTL_GENERIC_TABLE (Splay树) 实现重定向映射
// 键为原始扇区索引，值为目标扇区索引

#include "QHOffsetHash.h"
#include "QHEngineDefs.h"

// ---- RTL_GENERIC_TABLE 回调函数 ----

static RTL_GENERIC_COMPARE_RESULTS NTAPI QHCompareRoutine(
	struct _RTL_GENERIC_TABLE *Table,
	PVOID FirstStruct,
	PVOID SecondStruct)
{
	PQH_REDIRECT_PAIR first = (PQH_REDIRECT_PAIR)FirstStruct;
	PQH_REDIRECT_PAIR second = (PQH_REDIRECT_PAIR)SecondStruct;

	UNREFERENCED_PARAMETER(Table);

	if (first->OrgSectorIndex < second->OrgSectorIndex)
		return GenericLessThan;
	else if (first->OrgSectorIndex > second->OrgSectorIndex)
		return GenericGreaterThan;
	else
		return GenericEqual;
}

static PVOID NTAPI QHAllocateRoutine(
	struct _RTL_GENERIC_TABLE *Table,
	CLONG ByteSize)
{
	UNREFERENCED_PARAMETER(Table);
	return qhalloc(ByteSize);
}

static VOID NTAPI QHFreeRoutine(
	struct _RTL_GENERIC_TABLE *Table,
	PVOID Buffer)
{
	UNREFERENCED_PARAMETER(Table);
	qhfree(Buffer);
}

// ---- 公共接口 ----

PQH_OFFSET_HASH QHHashCreate()
{
	PQH_OFFSET_HASH hash = (PQH_OFFSET_HASH)qhalloc(sizeof(QH_OFFSET_HASH));
	if (!hash) return NULL;

	RtlZeroMemory(hash, sizeof(QH_OFFSET_HASH));
	RtlInitializeGenericTable(&hash->Table, QHCompareRoutine, QHAllocateRoutine, QHFreeRoutine, NULL);
	return hash;
}

NTSTATUS QHHashSet(_In_ PQH_OFFSET_HASH Hash, _In_ UINT64 Key, _In_ UINT64 TargetSectorIndex)
{
	QH_REDIRECT_PAIR pair;
	pair.OrgSectorIndex = Key;
	pair.TargetSectorIndex = TargetSectorIndex;

	BOOLEAN newElement = FALSE;
	PQH_REDIRECT_PAIR result = (PQH_REDIRECT_PAIR)RtlInsertElementGenericTable(
		&Hash->Table, &pair, sizeof(QH_REDIRECT_PAIR), &newElement);

	if (!result)
		return STATUS_INSUFFICIENT_RESOURCES;

	if (!newElement)
	{
		// 键已存在，直接更新值
		result->TargetSectorIndex = TargetSectorIndex;
	}

	return STATUS_SUCCESS;
}

NTSTATUS QHHashGet(_In_ PQH_OFFSET_HASH Hash, _In_ UINT64 Key, _Out_ PUINT64 TargetSectorIndex)
{
	QH_REDIRECT_PAIR searchPair;
	searchPair.OrgSectorIndex = Key;
	searchPair.TargetSectorIndex = 0;

	PQH_REDIRECT_PAIR result = (PQH_REDIRECT_PAIR)RtlLookupElementGenericTable(&Hash->Table, &searchPair);

	if (result)
	{
		*TargetSectorIndex = result->TargetSectorIndex;
		return STATUS_SUCCESS;
	}

	return STATUS_NOT_FOUND;
}

NTSTATUS QHHashRemove(_In_ PQH_OFFSET_HASH Hash, _In_ UINT64 Key)
{
	QH_REDIRECT_PAIR searchPair;
	searchPair.OrgSectorIndex = Key;
	searchPair.TargetSectorIndex = 0;

	BOOLEAN deleted = RtlDeleteElementGenericTable(&Hash->Table, &searchPair);

	return deleted ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

VOID QHHashClear(_In_ PQH_OFFSET_HASH Hash)
{
	PVOID restartKey = NULL;
	PVOID element;

	while ((element = RtlEnumerateGenericTableWithoutSplaying(&Hash->Table, &restartKey)) != NULL)
	{
		RtlDeleteElementGenericTable(&Hash->Table, element);
		restartKey = NULL;
	}
}

VOID QHHashForeach(_In_ PQH_OFFSET_HASH Hash, _In_ QHHashForeachCallback Callback)
{
	PVOID restartKey = NULL;
	PQH_REDIRECT_PAIR element;

	while ((element = (PQH_REDIRECT_PAIR)RtlEnumerateGenericTableWithoutSplaying(&Hash->Table, &restartKey)) != NULL)
	{
		if (!Callback(element->OrgSectorIndex, element->TargetSectorIndex))
			break;
	}
}

VOID QHHashDelete(_In_ PQH_OFFSET_HASH Hash)
{
	if (!Hash) return;

	QHHashClear(Hash);
	qhfree(Hash);
}
