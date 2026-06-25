//Magic effect layout - alchemy/magic item effect list nodes
#pragma once

#include <cstddef>

#include "nvse/GameForms.h"
#include "nvse/GameEffects.h"
#include "internal/DialogueLayout.h" //BSSimpleListNodeView (shared list node)

struct EffectItemListView {
	void* vtbl;
	BSSimpleListNodeView<EffectItem*> effects;
	UInt32 unk0C;
};

static_assert(offsetof(AlchemyItem, effects) == 0x3C);
static_assert(offsetof(EffectItem, setting) == 0x14);
static_assert(offsetof(EffectItemListView, effects) == 0x04);

inline EffectItemList* MagicItemGetEffectList(MagicItem* magicItem)
{
	return magicItem ? &magicItem->list : nullptr;
}

inline EffectItemListView* AlchemyItemGetEffectListView(AlchemyItem* item)
{
	return item ? reinterpret_cast<EffectItemListView*>(&item->effects) : nullptr;
}
