#pragma once

#include "common/ITypes.h"

namespace OnDialogueMenuBuildHandler {
	bool Init(void* nvseInterface);

	//per-rebuild reset of synthetic-row bookkeeping, runs inside Hook_LoadTopicsList
	//every menu open and after each reply - must never touch the topic rule sets
	void ClearState();

	//flush topic hide/order rules, load-message path only (not per rebuild)
	void ClearRules();

	//records a synthetic row so the click hook can recover its exact id
	//trait 4012 only carries the discriminator bit reliably, not the low id bits
	bool RegisterSyntheticTile(void* tile, UInt32 syntheticId);
	bool HasSyntheticCapacity();

	//topic rules keyed by formID, matched against a row's TESTopic or TESTopicInfo
	//applied on the next list rebuild - return false when the bounded store is full
	bool SetTopicHidden(UInt32 formID, bool hidden);
	bool SetTopicOrder(UInt32 formID, int order);
	int ClearTopicOverrides();

	//rows the last rebuild rendered. the live listbox counter is not a substitute, ListBox::AddEntry
	//bumps it for every synthetic row a handler adds
	constexpr UInt32 kRowCountUnknown = 0xFFFFFFFF;
	UInt32 GetRenderedRowCount();
}
