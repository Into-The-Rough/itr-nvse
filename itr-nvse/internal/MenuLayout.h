#pragma once

#include <cstddef>

#include "common/ITypes.h"

constexpr UInt32 kTileTrait_id = 0xFAA;

enum MenuSpecialKeyboardInputCode {
	kMenuKey_Enter = -2,
	kMenuKey_UpArrow = 1,
	kMenuKey_DownArrow,
	kMenuKey_RightArrow,
	kMenuKey_LeftArrow,
	kMenuKey_Space = 9,
	kMenuKey_Tab,
	kMenuKey_ShiftEnter,
	kMenuKey_AltEnter,
	kMenuKey_ShiftLeft,
	kMenuKey_ShiftRight,
	kMenuKey_PageUp,
	kMenuKey_PageDown
};

struct TileView;

struct TileValueView {
	UInt32 id; //0x00
	TileView* parent; //0x04
	float num; //0x08
	char* str; //0x0C
	void* action; //0x10
};

template<typename Item>
struct MenuListBoxView {
	struct ListItem {
		TileView* tile;
		Item item;
		bool isFiltered;
		UInt8 pad09[3];
	};

	struct Node {
		ListItem* data;
		Node* next;
	};

	struct List {
		Node m_listHead;

		Node* Head() { return &m_listHead; }

		const Node* Head() const { return &m_listHead; }
	};

	void* vtable;
	List list;
	TileView* parentTile;
	TileView* selected;
};

struct MessageMenuView {
	void* vtable;
	UInt8 pad04[0x3C];
	MenuListBoxView<int> buttonList;
};

struct BGSSaveLoadFileEntryView {
	char* name;
	UInt8 isInitialised;
	UInt8 isCorrupt;
	UInt8 gap06[2];
	UInt32 saveNumber;
	char* pcName;
	char* pcTitle;
	char* location;
	char* time;
};

class BGSNote;
class TESObjectREFR;
class TESTopic;
class TESTopicInfo;

struct TESFormIDView {
	void* vtbl;
	UInt8 typeID;
	UInt8 pad05[7];
	UInt32 refID;
};

struct TileStringView {
	char* m_data;
	UInt16 m_dataLen;
	UInt16 m_bufLen;
};

struct TileNodeView {
	TileNodeView* next;
	TileNodeView* prev;
	void* data;
};

template<typename T>
struct TileSimpleArrayView {
	void* vtbl;
	T* data;
	UInt32 size;
	UInt32 capacity;
};

struct TileView {
	void* vtbl;
	TileNodeView* firstChild;
	UInt8 pad08[0x08];
	TileSimpleArrayView<TileValueView*> values;
	TileStringView name;
};

inline TileValueView* TileGetValue(const void* tilePtr, UInt32 traitID)
{
	auto* tile = reinterpret_cast<const TileView*>(tilePtr);
	if (!tile || !tile->values.data)
		return nullptr;

	UInt32 left = 0;
	UInt32 right = tile->values.size;
	while (left < right) {
		UInt32 mid = (left + right) >> 1;
		TileValueView* val = tile->values.data[mid];
		if (!val)
			return nullptr;
		if (val->id < traitID)
			left = mid + 1;
		else if (val->id == traitID)
			return val;
		else
			right = mid;
	}
	return nullptr;
}

struct TileMenuArrayView {
	void* vtbl;
	void** data;
	UInt16 capacity;
	UInt16 firstFreeEntry;
	UInt16 numValidEntries;
	UInt16 growSize;
};

struct TileShaderPropertyView {
	UInt8 pad00[0x91];
	UInt8 textureMorph;
};

struct MenuView {
	void* vtbl;
	void* tile;
};

struct InventoryMenuView {
	UInt8 pad00[0x84];
	UInt32 filter;
};

struct ContainerMenuView {
	UInt8 pad00[0x74];
	TESObjectREFR* containerRef;
	UInt8 pad78[0x8C - 0x78];
	UInt32 leftFilter;
	UInt32 rightFilter;
	UInt8 pad94[0x98 - 0x94];
	void* leftItems;
	UInt8 pad9C[0xF8 - 0x9C];
	void* currentItems;
};

struct BarterMenuView {
	UInt8 pad00[0x80];
	TESObjectREFR* merchantRef;
	UInt8 pad84[0x9C - 0x84];
	UInt32 leftFilter;
	UInt32 rightFilter;
	UInt8 padA4[0xA8 - 0xA4];
	void* leftItems;
	UInt8 padAC[0x108 - 0xAC];
	void* currentItems;
	void* leftBarterItem;
	void* leftBarterNext;
};

struct HUDMainMenuView {
	MenuView menu;
	UInt8 pad08[0xA8 - 0x08];
	void* infoNameTile;
	UInt8 padAC[0x1B8 - 0xAC];
	TESObjectREFR* crosshairRef;
};

struct BGSNoteView {
	UInt8 pad00[0x6C];

	union {
		void* noteText;
		void* picture;
		void* voice;
	};

	void* speaker;
	UInt8 pad74[0x08];
	UInt8 noteType;
	UInt8 read;
};

struct ListBoxView {
	void* vtbl;
	void* headData;
	void* headNext;
	UInt32 unk0C;
	void* selected;
};

struct MapMenuView {
	UInt8 pad00[0x5C];
	void* dataPanelTile;
	UInt8 pad60[0x0C];
	void* tabLineTile;
	UInt8 pad70[0x10];
	UInt8 currentTab;
	UInt8 pad81[0x0F];
	BGSNote* currentNote;
	UInt32 timeNoteViewed;
	UInt8 holotapeDialogues[0x10];
	UInt8 holotapeSubtitles[0x10];
	void* currentHolotapeDialogueSound;
	UInt8 isHolotapeVoicePlaying;
	UInt8 pad0BD[3];
	float holotapeTotalTime;
	UInt32 holotapePlayStartTime;
	UInt8 pad0C8[0x98];
	ListBoxView noteList;
};

struct InterfaceManagerView {
	UInt8 pad00[0xE4];
	UInt8 msgBoxButton;
	UInt8 pad0E5[0xFC - 0xE5];
	TESObjectREFR* crosshairRef;
	UInt8 pad100[0x114 - 0x100];
	UInt32 menuStack[10];
	UInt8 pad13C[0x1DC - 0x13C];
	UInt8 vatsHighlightData[0x10];
};

struct VATSHighlightDataView {
	UInt32 mode; //0 renders the highlight target, non-zero renders the additional-ref list
	TESObjectREFR* target;
	void* targetNode;
	SInt32 refCount;
};

struct DialogMenuView {
	UInt8 pad00[0x48];
	void* currentInfo;
	UInt8 pad4C[0x70 - 0x4C];
	void* topicManager;
};

struct MenuTopicView {
	UInt8 pad00[0x18];
	TESTopicInfo* topicInfo;
	TESTopic* topic;
};

//8 byte tList node, data is a MenuTopic. iteration ends at the first null data, and a null-data node
//with a live next faults the chain free sub_83E2D0, which stops only on next and data both null
struct MenuTopicNodeView {
	void* menuTopic;
	MenuTopicNodeView* next;
};

//24 bytes. +0x04/+0x08 are the data/next of a tList of screens, +0x08 is the older-screens link
struct MenuTopicManagerView {
	MenuTopicNodeView* cursor;
	MenuTopicNodeView* topicList;
	UInt8 pad08[0x18 - 0x08];
};

enum BGSNoteType : UInt8 {
	kBGSNoteType_Sound = 0,
	kBGSNoteType_Text = 1,
	kBGSNoteType_Image = 2,
	kBGSNoteType_Voice = 3,
};

static_assert(offsetof(TESFormIDView, typeID) == 0x04);
static_assert(offsetof(TESFormIDView, refID) == 0x0C);
static_assert(sizeof(TileStringView) == 0x08);
static_assert(offsetof(TileNodeView, next) == 0x00);
static_assert(offsetof(TileNodeView, prev) == 0x04);
static_assert(offsetof(TileNodeView, data) == 0x08);
static_assert(offsetof(TileView, firstChild) == 0x04);
static_assert(offsetof(TileView, name) == 0x20);
static_assert(sizeof(TileMenuArrayView) == 0x10);
static_assert(offsetof(TileMenuArrayView, data) == 0x04);
static_assert(offsetof(TileMenuArrayView, firstFreeEntry) == 0x0A);
static_assert(offsetof(TileShaderPropertyView, textureMorph) == 0x91);
static_assert(offsetof(MenuView, tile) == 0x04);
static_assert(offsetof(InventoryMenuView, filter) == 0x84);
static_assert(offsetof(ContainerMenuView, containerRef) == 0x74);
static_assert(offsetof(ContainerMenuView, leftFilter) == 0x8C);
static_assert(offsetof(ContainerMenuView, rightFilter) == 0x90);
static_assert(offsetof(ContainerMenuView, leftItems) == 0x98);
static_assert(offsetof(ContainerMenuView, currentItems) == 0xF8);
static_assert(offsetof(BarterMenuView, merchantRef) == 0x80);
static_assert(offsetof(BarterMenuView, leftFilter) == 0x9C);
static_assert(offsetof(BarterMenuView, rightFilter) == 0xA0);
static_assert(offsetof(BarterMenuView, leftItems) == 0xA8);
static_assert(offsetof(BarterMenuView, currentItems) == 0x108);
static_assert(offsetof(BarterMenuView, leftBarterItem) == 0x10C);
static_assert(offsetof(HUDMainMenuView, infoNameTile) == 0xA8);
static_assert(offsetof(HUDMainMenuView, crosshairRef) == 0x1B8);
static_assert(sizeof(BGSNoteView) == 0x80);
static_assert(offsetof(BGSNoteView, noteText) == 0x6C);
static_assert(offsetof(BGSNoteView, speaker) == 0x70);
static_assert(offsetof(BGSNoteView, noteType) == 0x7C);
static_assert(offsetof(BGSNoteView, read) == 0x7D);
static_assert(offsetof(ListBoxView, headData) == 0x04);
static_assert(offsetof(ListBoxView, headNext) == 0x08);
static_assert(offsetof(ListBoxView, selected) == 0x10);
static_assert(offsetof(MapMenuView, dataPanelTile) == 0x5C);
static_assert(offsetof(MapMenuView, tabLineTile) == 0x6C);
static_assert(offsetof(MapMenuView, currentTab) == 0x80);
static_assert(offsetof(MapMenuView, currentNote) == 0x90);
static_assert(offsetof(MapMenuView, holotapeDialogues) == 0x98);
static_assert(offsetof(MapMenuView, holotapeSubtitles) == 0xA8);
static_assert(offsetof(MapMenuView, currentHolotapeDialogueSound) == 0xB8);
static_assert(offsetof(MapMenuView, isHolotapeVoicePlaying) == 0xBC);
static_assert(offsetof(MapMenuView, holotapeTotalTime) == 0xC0);
static_assert(offsetof(MapMenuView, holotapePlayStartTime) == 0xC4);
static_assert(offsetof(MapMenuView, noteList) == 0x160);
static_assert(offsetof(InterfaceManagerView, msgBoxButton) == 0xE4);
static_assert(offsetof(InterfaceManagerView, crosshairRef) == 0xFC);
static_assert(offsetof(InterfaceManagerView, vatsHighlightData) == 0x1DC);
static_assert(offsetof(VATSHighlightDataView, mode) == 0x00);
static_assert(offsetof(VATSHighlightDataView, target) == 0x04);
static_assert(offsetof(VATSHighlightDataView, refCount) == 0x0C);
static_assert(offsetof(DialogMenuView, currentInfo) == 0x48);
static_assert(offsetof(DialogMenuView, topicManager) == 0x70);
static_assert(offsetof(MenuTopicView, topicInfo) == 0x18);
static_assert(offsetof(MenuTopicView, topic) == 0x1C);
static_assert(sizeof(MenuTopicNodeView) == 0x08);
static_assert(offsetof(MenuTopicManagerView, topicList) == 0x04);
static_assert(sizeof(MenuTopicManagerView) == 0x18);

inline BGSNoteView* BGSNoteAsView(BGSNote* note)
{
	return reinterpret_cast<BGSNoteView*>(note);
}

inline const BGSNoteView* BGSNoteAsView(const BGSNote* note)
{
	return reinterpret_cast<const BGSNoteView*>(note);
}

inline UInt8 BGSNoteGetType(const BGSNote* note)
{
	return note ? BGSNoteAsView(note)->noteType : 0xFF;
}

inline bool BGSNoteIsNoteForm(const BGSNote* note)
{
	return note && reinterpret_cast<const TESFormIDView*>(note)->typeID == 0x31;
}

inline void BGSNoteMarkRead(BGSNote* note)
{
	if (note) BGSNoteAsView(note)->read = 1;
}

inline void* BGSNoteGetTextSource(BGSNote* note)
{
	return note ? BGSNoteAsView(note)->noteText : nullptr;
}

inline void* BGSNoteGetVoice(BGSNote* note)
{
	return note ? BGSNoteAsView(note)->voice : nullptr;
}

inline UInt32 BGSNoteGetVoiceRefID(BGSNote* note)
{
	void* voice = BGSNoteGetVoice(note);
	return voice ? reinterpret_cast<TESFormIDView*>(voice)->refID : 0;
}

inline void* BGSNoteGetSpeaker(BGSNote* note)
{
	return note ? BGSNoteAsView(note)->speaker : nullptr;
}

inline MapMenuView* MapMenuAsView(void* mapMenu)
{
	return reinterpret_cast<MapMenuView*>(mapMenu);
}

inline InterfaceManagerView* InterfaceManagerAsView(void* interfaceManager)
{
	return reinterpret_cast<InterfaceManagerView*>(interfaceManager);
}

inline TileView* TileAsView(void* tile)
{
	return reinterpret_cast<TileView*>(tile);
}

inline const char* TileGetName(void* tile)
{
	return tile ? TileAsView(tile)->name.m_data : nullptr;
}

inline TileNodeView* TileGetFirstChild(void* tile)
{
	return tile ? TileAsView(tile)->firstChild : nullptr;
}

inline bool TileShaderPropertyUsesTextureMorph(void* shader)
{
	return shader && reinterpret_cast<TileShaderPropertyView*>(shader)->textureMorph != 0;
}

inline MenuView* MenuAsView(void* menu)
{
	return reinterpret_cast<MenuView*>(menu);
}

inline UInt32 InventoryMenuGetFilter(void* menu)
{
	return menu ? reinterpret_cast<InventoryMenuView*>(menu)->filter : 0;
}

inline UInt32 ContainerMenuGetLeftFilter(void* menu)
{
	return menu ? reinterpret_cast<ContainerMenuView*>(menu)->leftFilter : 0;
}

inline UInt32 ContainerMenuGetRightFilter(void* menu)
{
	return menu ? reinterpret_cast<ContainerMenuView*>(menu)->rightFilter : 0;
}

inline void* ContainerMenuGetLeftItems(void* menu)
{
	return menu ? &reinterpret_cast<ContainerMenuView*>(menu)->leftItems : nullptr;
}

inline void* ContainerMenuGetCurrentItems(void* menu)
{
	return menu ? reinterpret_cast<ContainerMenuView*>(menu)->currentItems : nullptr;
}

inline TESObjectREFR* ContainerMenuGetContainerRef(void* menu)
{
	return menu ? reinterpret_cast<ContainerMenuView*>(menu)->containerRef : nullptr;
}

inline UInt32 ContainerMenuGetCurrentSide(void* menu)
{
	return ContainerMenuGetCurrentItems(menu) == ContainerMenuGetLeftItems(menu) ? 0 : 1;
}

inline UInt32 BarterMenuGetLeftFilter(void* menu)
{
	return menu ? reinterpret_cast<BarterMenuView*>(menu)->leftFilter : 0;
}

inline TESObjectREFR* BarterMenuGetMerchantRef(void* menu)
{
	return menu ? reinterpret_cast<BarterMenuView*>(menu)->merchantRef : nullptr;
}

inline UInt32 BarterMenuGetRightFilter(void* menu)
{
	return menu ? reinterpret_cast<BarterMenuView*>(menu)->rightFilter : 0;
}

inline void* BarterMenuGetLeftItems(void* menu)
{
	return menu ? &reinterpret_cast<BarterMenuView*>(menu)->leftItems : nullptr;
}

inline void* BarterMenuGetCurrentItems(void* menu)
{
	return menu ? reinterpret_cast<BarterMenuView*>(menu)->currentItems : nullptr;
}

inline UInt32 BarterMenuGetCurrentSide(void* menu)
{
	return BarterMenuGetCurrentItems(menu) == BarterMenuGetLeftItems(menu) ? 0 : 1;
}

inline void* BarterMenuGetLeftBarter(void* menu)
{
	return menu ? &reinterpret_cast<BarterMenuView*>(menu)->leftBarterItem : nullptr;
}

inline HUDMainMenuView* HUDMainMenuAsView(void* hud)
{
	return reinterpret_cast<HUDMainMenuView*>(hud);
}

inline void* HUDMainMenuGetRootTile(void* hud)
{
	return hud ? HUDMainMenuAsView(hud)->menu.tile : nullptr;
}

inline TESObjectREFR* HUDMainMenuGetCrosshairRef(void* hud)
{
	return hud ? HUDMainMenuAsView(hud)->crosshairRef : nullptr;
}

inline void* HUDMainMenuGetInfoNameTile(void* hud)
{
	return hud ? HUDMainMenuAsView(hud)->infoNameTile : nullptr;
}

inline VATSHighlightDataView* InterfaceManagerGetVATSHighlightData(void* interfaceManager)
{
	return interfaceManager ? reinterpret_cast<VATSHighlightDataView*>(InterfaceManagerAsView(interfaceManager)->vatsHighlightData) : nullptr;
}

inline TESObjectREFR* InterfaceManagerGetCrosshairRef(void* interfaceManager)
{
	return interfaceManager ? InterfaceManagerAsView(interfaceManager)->crosshairRef : nullptr;
}

inline UInt32 VATSHighlightDataGetRefCount(void* vatsData)
{
	return vatsData ? reinterpret_cast<VATSHighlightDataView*>(vatsData)->refCount : 0;
}

inline bool VATSHighlightDataHasRefs(void* vatsData)
{
	auto* data = reinterpret_cast<VATSHighlightDataView*>(vatsData);
	return data && data->mode != 0 && data->refCount > 0;
}

inline void* DialogMenuGetTopicManager(void* dialogMenu)
{
	return dialogMenu ? reinterpret_cast<DialogMenuView*>(dialogMenu)->topicManager : nullptr;
}

inline void* DialogMenuGetCurrentInfo(void* dialogMenu)
{
	return dialogMenu ? reinterpret_cast<DialogMenuView*>(dialogMenu)->currentInfo : nullptr;
}

inline TESTopicInfo* MenuTopicGetTopicInfo(void* menuTopic)
{
	return menuTopic ? reinterpret_cast<MenuTopicView*>(menuTopic)->topicInfo : nullptr;
}

inline TESTopic* MenuTopicGetTopic(void* menuTopic)
{
	return menuTopic ? reinterpret_cast<MenuTopicView*>(menuTopic)->topic : nullptr;
}

//node 0 carries the greeting, both engine consumers skip exactly one node
inline MenuTopicNodeView* MenuTopicManagerGetTopicList(void* menuTopicMgr)
{
	return menuTopicMgr ? reinterpret_cast<MenuTopicManagerView*>(menuTopicMgr)->topicList : nullptr;
}

//topic ListBox at DialogMenu+0x40, item count is a UInt16 and the two bytes above it are never written
inline UInt32 DialogMenuGetTopicRowCount(void* dialogMenu)
{
	return dialogMenu ? *(UInt16*)((char*)dialogMenu + 0x40 + 0x1C) : 0;
}

inline UInt32 FormViewGetRefID(const void* form)
{
	return form ? reinterpret_cast<const TESFormIDView*>(form)->refID : 0;
}

static_assert(sizeof(TileValueView) == 0x14);
static_assert(offsetof(TileValueView, num) == 0x08);
static_assert(offsetof(TileValueView, str) == 0x0C);
static_assert(offsetof(TileView, values) == 0x10);
static_assert(offsetof(TileView, name) == 0x20);
static_assert(offsetof(InterfaceManagerView, menuStack) == 0x114);
static_assert(offsetof(InterfaceManagerView, vatsHighlightData) == 0x1DC);
static_assert(offsetof(MenuListBoxView<int>, list) == 0x04);
static_assert(offsetof(MenuListBoxView<int>, selected) == 0x10);
static_assert(offsetof(MessageMenuView, buttonList) == 0x40);
static_assert(offsetof(BGSSaveLoadFileEntryView, location) == 0x14);
