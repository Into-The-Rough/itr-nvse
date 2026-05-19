#include "InvRefData.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "nvse/CommandTable.h"
#include "nvse/GameBSExtraData.h"
#include "nvse/GameExtraData.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "nvse/PluginAPI.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern PluginHandle g_pluginHandle;
extern void Log(const char* fmt, ...);

namespace
{
	constexpr UInt32 MakeTag(char a, char b, char c, char d)
	{
		return static_cast<UInt32>(a) |
			(static_cast<UInt32>(b) << 8) |
			(static_cast<UInt32>(c) << 16) |
			(static_cast<UInt32>(d) << 24);
	}

	constexpr UInt32 kRecord_InvRefData = MakeTag('I', 'R', 'D', 'S');
	constexpr UInt32 kRecordVersion = 1;

	enum class LocatorKind : UInt8
	{
		None = 0,
		Reference = 1,
		Inventory = 2,
	};

	enum class ValueType : UInt8
	{
		None = 0,
		Number = 1,
		Form = 2,
		String = 3,
	};

	struct Locator
	{
		LocatorKind kind = LocatorKind::None;
		UInt32 ownerRefID = 0;
		UInt32 baseFormID = 0;
		UInt32 extendIndex = 0;
	};

	struct Value
	{
		ValueType type = ValueType::None;
		double number = 0.0;
		UInt32 formID = 0;
		std::string string;
	};

	using KeyMap = std::unordered_map<std::string, Value>;
	using NamespaceMap = std::unordered_map<std::string, KeyMap>;

	struct ItemState
	{
		Locator locator;
		NamespaceMap namespaces;
	};

	struct InventoryReferenceData
	{
		TESForm* type;
		ExtraContainerChanges::EntryData* entry;
		ExtraDataList* xData;
	};

	struct InventoryReferenceMini
	{
		InventoryReferenceData data;
		TESObjectREFR* containerRef;
		TESObjectREFR* tempRef;
	};

	using InventoryRefGetForID_t = void* (*)(UInt32 refID);
	using FormHeapAllocate_t = void* (__cdecl*)(UInt32 size);
	//0x411EC0 is int __thiscall ExtraDataList::CopyList(ExtraDataList* dst, BaseExtraList* src);
	//returns through eax - keep the trampoline ABI exact, this is a shared entry-point hook
	using ExtraDataListCopyList_t = int (__thiscall*)(BaseExtraList* dst, BaseExtraList* src);

	static NVSESerializationInterface* s_serialization = nullptr;
	static NVSEStringVarInterface* s_stringInterface = nullptr;
	static bool (*s_extractArgsEx)(ParamInfo*, void*, UInt32*, Script*, ScriptEventList*, ...) = nullptr;
	static InventoryRefGetForID_t s_getInventoryRefForID = nullptr;
	static auto s_formHeapAllocate = reinterpret_cast<FormHeapAllocate_t>(0x401000);
	static ExtraDataListCopyList_t s_originalCopyList = nullptr;
	static Detours::JumpDetour s_copyListDetour;

	static std::unordered_map<ExtraDataList*, ItemState> s_states;

	static bool IsEmpty(const ItemState& state)
	{
		for (const auto& nsIt : state.namespaces)
			if (!nsIt.second.empty())
				return false;
		return true;
	}

	static void MarkModified(TESForm* form, UInt32 flag)
	{
		auto* saveLoadGame = *reinterpret_cast<void**>(0x11DDF38);
		if (saveLoadGame && form)
			ThisCall<void>(0x84A690, saveLoadGame, form, flag, 0);
	}

	static bool BaseExtraListHasType(const BaseExtraList* list, UInt32 type)
	{
		if (!list)
			return false;
		const UInt32 index = type >> 3;
		const UInt8 bitMask = 1 << (type & 7);
		return (list->m_presenceBitfield[index] & bitMask) != 0;
	}

	static BSExtraData* BaseExtraListGetByType(BaseExtraList* list, UInt32 type)
	{
		if (!list || !BaseExtraListHasType(list, type))
			return nullptr;
		for (auto* extra = list->m_data; extra; extra = extra->next)
			if (extra->type == type)
				return extra;
		return nullptr;
	}

	static void BaseExtraListMarkType(BaseExtraList* list, UInt32 type, bool cleared)
	{
		if (!list)
			return;
		const UInt32 index = type >> 3;
		const UInt8 bitMask = 1 << (type & 7);
		if (cleared)
			list->m_presenceBitfield[index] &= ~bitMask;
		else
			list->m_presenceBitfield[index] |= bitMask;
	}

	static bool BaseExtraListAdd(BaseExtraList* list, BSExtraData* extra)
	{
		if (!list || !extra || BaseExtraListHasType(list, extra->type))
			return false;
		extra->next = list->m_data;
		list->m_data = extra;
		BaseExtraListMarkType(list, extra->type, false);
		return true;
	}

	static ExtraCount* CreateExtraCount(SInt16 count)
	{
		auto* extra = static_cast<ExtraCount*>(s_formHeapAllocate(sizeof(ExtraCount)));
		if (!extra)
			return nullptr;
		memset(extra, 0, sizeof(ExtraCount));
		*reinterpret_cast<UInt32*>(extra) = 0x010158D8;
		extra->type = kExtraData_Count;
		extra->count = count;
		return extra;
	}

	static ExtraContainerChanges* GetContainerChanges(TESObjectREFR* ref)
	{
		if (!ref)
			return nullptr;
		return static_cast<ExtraContainerChanges*>(
			BaseExtraListGetByType(&ref->extraDataList, kExtraData_ContainerChanges));
	}

	static ExtraDataList* CreateEmptyExtraDataList()
	{
		auto* list = static_cast<ExtraDataList*>(s_formHeapAllocate(sizeof(ExtraDataList)));
		if (!list)
			return nullptr;
		memset(list, 0, sizeof(ExtraDataList));
		*reinterpret_cast<UInt32*>(list) = 0x010143E8;
		return list;
	}

	static ExtraContainerChanges::ExtendDataList* CreateEmptyExtendDataList()
	{
		auto* list = static_cast<ExtraContainerChanges::ExtendDataList*>(
			s_formHeapAllocate(sizeof(ExtraContainerChanges::ExtendDataList)));
		if (!list)
			return nullptr;
		memset(list, 0, sizeof(ExtraContainerChanges::ExtendDataList));
		return list;
	}

	template <class TList, class TItem>
	static void AppendListItem(TList* list, TItem* item)
	{
		if (!list || !item)
			return;

		using Node = typename TList::_Node;
		Node* head = list->Head();
		if (!head->item)
		{
			head->item = item;
			head->next = nullptr;
			return;
		}

		Node* node = head;
		while (node->next)
			node = node->next;

		Node* newNode = static_cast<Node*>(s_formHeapAllocate(sizeof(Node)));
		if (!newNode)
			return;
		memset(newNode, 0, sizeof(Node));
		newNode->item = item;
		node->next = newNode;
	}

	static UInt32 GetExtendIndex(ExtraContainerChanges::ExtendDataList* list, ExtraDataList* xData)
	{
		if (!list || !xData)
			return 0xFFFFFFFF;

		UInt32 index = 0;
		for (auto* node = list->Head(); node && node->Item(); node = node->Next(), ++index)
			if (node->Item() == xData)
				return index;

		return 0xFFFFFFFF;
	}

	static ExtraContainerChanges::EntryData* FindEntryForItem(
		ExtraContainerChanges::EntryDataList* list,
		TESForm* item)
	{
		if (!list || !item)
			return nullptr;

		for (auto* node = list->Head(); node && node->Item(); node = node->Next())
		{
			auto* entry = node->Item();
			if (entry && entry->type == item)
				return entry;
		}

		return nullptr;
	}

	static ExtraDataList* GetNthExtraList(ExtraContainerChanges::ExtendDataList* list, UInt32 index)
	{
		if (!list)
			return nullptr;

		UInt32 current = 0;
		for (auto* node = list->Head(); node && node->Item(); node = node->Next(), ++current)
			if (current == index)
				return node->Item();

		return nullptr;
	}

	static ExtraDataList* ResolveLocator(const Locator& locator)
	{
		if (!locator.ownerRefID)
			return nullptr;

		auto* ownerForm = static_cast<TESForm*>(Engine::LookupFormByID(locator.ownerRefID));
		if (!ownerForm || !ownerForm->IsReference())
			return nullptr;

		auto* ref = static_cast<TESObjectREFR*>(ownerForm);
		if (locator.kind == LocatorKind::Reference)
			return &ref->extraDataList;

		if (locator.kind != LocatorKind::Inventory || !locator.baseFormID)
			return nullptr;

		auto* baseForm = static_cast<TESForm*>(Engine::LookupFormByID(locator.baseFormID));
		if (!baseForm)
			return nullptr;

		auto* changes = GetContainerChanges(ref);
		if (!changes || !changes->data || !changes->data->objList)
			return nullptr;

		auto* entry = FindEntryForItem(changes->data->objList, baseForm);
		if (!entry || !entry->extendData)
			return nullptr;

		return GetNthExtraList(entry->extendData, locator.extendIndex);
	}

	struct Target
	{
		ExtraDataList* xData = nullptr;
		Locator locator;
	};

	static InventoryReferenceMini* GetInventoryReference(TESObjectREFR* ref)
	{
		if (!ref || !s_getInventoryRefForID)
			return nullptr;
		return static_cast<InventoryReferenceMini*>(s_getInventoryRefForID(ref->refID));
	}

	static ExtraDataList* EnsureInventoryExtraData(InventoryReferenceMini* inv)
	{
		if (!inv || !inv->containerRef || !inv->data.type || !inv->data.entry)
			return nullptr;
		if (inv->data.xData)
			return inv->data.xData;

		auto* xData = CreateEmptyExtraDataList();
		if (!xData)
			return nullptr;
		BaseExtraListAdd(xData, CreateExtraCount(1));

		if (!inv->data.entry->extendData)
		{
			inv->data.entry->extendData = CreateEmptyExtendDataList();
			if (!inv->data.entry->extendData)
				return nullptr;
		}

		AppendListItem(inv->data.entry->extendData, xData);
		inv->data.xData = xData;
		MarkModified(inv->containerRef, 0x20);
		return xData;
	}

	static bool ResolveTarget(TESObjectREFR* thisObj, bool create, Target& out)
	{
		if (!thisObj)
			return false;

		if (auto* inv = GetInventoryReference(thisObj))
		{
			auto* xData = create ? EnsureInventoryExtraData(inv) : inv->data.xData;
			if (!xData || !inv->containerRef || !inv->data.type || !inv->data.entry)
				return false;

			const UInt32 index = GetExtendIndex(inv->data.entry->extendData, xData);
			if (index == 0xFFFFFFFF)
				return false;

			out.xData = xData;
			out.locator.kind = LocatorKind::Inventory;
			out.locator.ownerRefID = inv->containerRef->refID;
			out.locator.baseFormID = inv->data.type->refID;
			out.locator.extendIndex = index;
			return true;
		}

		out.xData = &thisObj->extraDataList;
		out.locator.kind = LocatorKind::Reference;
		out.locator.ownerRefID = thisObj->refID;
		out.locator.baseFormID = thisObj->baseForm ? thisObj->baseForm->refID : 0;
		out.locator.extendIndex = 0;
		return true;
	}

	static bool LocatorAcceptsTarget(const ItemState& state, const Target& target)
	{
		if (state.locator.kind == LocatorKind::None)
			return true;
		if (state.locator.kind != target.locator.kind ||
			state.locator.ownerRefID != target.locator.ownerRefID)
		{
			return false;
		}

		if (state.locator.kind == LocatorKind::Inventory &&
			(state.locator.baseFormID != target.locator.baseFormID ||
				state.locator.extendIndex != target.locator.extendIndex))
		{
			return false;
		}

		return ResolveLocator(state.locator) == target.xData;
	}

	static void MarkTargetModified(const Target& target)
	{
		if (auto* owner = static_cast<TESForm*>(Engine::LookupFormByID(target.locator.ownerRefID)))
			MarkModified(owner, target.locator.kind == LocatorKind::Inventory ? 0x20 : 0x400);
	}

	static Value* FindValue(const Target& target, const char* ns, const char* key)
	{
		if (!target.xData || !ns || !key)
			return nullptr;
		auto stateIt = s_states.find(target.xData);
		if (stateIt == s_states.end())
			return nullptr;
		if (!LocatorAcceptsTarget(stateIt->second, target))
			return nullptr;
		auto nsIt = stateIt->second.namespaces.find(ns);
		if (nsIt == stateIt->second.namespaces.end())
			return nullptr;
		auto keyIt = nsIt->second.find(key);
		return keyIt != nsIt->second.end() ? &keyIt->second : nullptr;
	}

	static void SetValue(const Target& target, const char* ns, const char* key, const Value& value)
	{
		auto& state = s_states[target.xData];
		if (!LocatorAcceptsTarget(state, target))
			state = ItemState{};
		state.locator = target.locator;
		state.namespaces[ns][key] = value;
		MarkTargetModified(target);
	}

	static bool ClearValue(const Target& target, const char* ns, const char* key)
	{
		auto stateIt = s_states.find(target.xData);
		if (stateIt == s_states.end())
			return false;
		if (!LocatorAcceptsTarget(stateIt->second, target))
			return false;

		stateIt->second.locator = target.locator;
		auto nsIt = stateIt->second.namespaces.find(ns);
		if (nsIt == stateIt->second.namespaces.end())
			return false;

		const bool removed = nsIt->second.erase(key) != 0;
		if (nsIt->second.empty())
			stateIt->second.namespaces.erase(nsIt);
		if (IsEmpty(stateIt->second))
			s_states.erase(stateIt);
		if (removed)
			MarkTargetModified(target);
		return removed;
	}

	static bool ClearNamespace(const Target& target, const char* ns)
	{
		auto stateIt = s_states.find(target.xData);
		if (stateIt == s_states.end())
			return false;
		if (!LocatorAcceptsTarget(stateIt->second, target))
			return false;

		stateIt->second.locator = target.locator;
		const bool removed = stateIt->second.namespaces.erase(ns) != 0;
		if (IsEmpty(stateIt->second))
			s_states.erase(stateIt);
		if (removed)
			MarkTargetModified(target);
		return removed;
	}

	static void CopyState(BaseExtraList* src, BaseExtraList* dst)
	{
		if (!src || !dst || src == dst)
			return;

		auto srcIt = s_states.find(static_cast<ExtraDataList*>(src));
		if (srcIt == s_states.end())
			return;
		if (s_states.find(static_cast<ExtraDataList*>(dst)) != s_states.end())
			return;

		ItemState copy = srcIt->second;
		copy.locator = Locator{};
		s_states[static_cast<ExtraDataList*>(dst)] = std::move(copy);
	}

	static int __fastcall Hook_ExtraDataListCopyList(BaseExtraList* dst, void*, BaseExtraList* src)
	{
		const int ret = s_originalCopyList(dst, src);
		CopyState(src, dst);
		return ret;
	}

	static UInt32 GetDetourSize(UInt32 address)
	{
		UInt32 offset = 0;
		while (offset < 27)
		{
			Detours::detail::DecodedInstruction instruction;
			if (!Detours::detail::DecodeInstruction(
				reinterpret_cast<const UInt8*>(address + offset),
				27 - offset,
				instruction))
			{
				return 0;
			}

			offset += instruction.length;
			if (offset >= 5)
				return offset;
		}

		return 0;
	}

	static void InstallCopyHook()
	{
		//vanilla ExtraDataList::CopyList prologue (FNV 1.4.0.525): push ebp; mov ebp,esp; sub esp,8.
		//JumpDetour only rejects an existing E9, so guard against a foreign non-E9 hook here.
		static const UInt8 kPrologue[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
		if (memcmp(reinterpret_cast<void*>(0x411EC0), kPrologue, sizeof(kPrologue)) != 0)
		{
			s_originalCopyList = reinterpret_cast<ExtraDataListCopyList_t>(0x411EC0);
			Log("InvRefData: unexpected bytes at ExtraDataList::CopyList (0x411EC0), not installing");
			return;
		}

		const UInt32 size = GetDetourSize(0x411EC0);
		UInt8* trampoline = nullptr;
		if (size && s_copyListDetour.WriteRelJump(0x411EC0, Hook_ExtraDataListCopyList, size, &trampoline))
		{
			s_originalCopyList = reinterpret_cast<ExtraDataListCopyList_t>(trampoline);
			Log("InvRefData: hooked ExtraDataList::CopyList");
		}
		else
		{
			s_originalCopyList = reinterpret_cast<ExtraDataListCopyList_t>(0x411EC0);
			Log("InvRefData: ExtraDataList::CopyList hook not installed");
		}
	}

	static void WriteString(const std::string& value)
	{
		const UInt16 len = static_cast<UInt16>(std::min<size_t>(value.size(), 0xFFFF));
		s_serialization->WriteRecordData(&len, sizeof(len));
		if (len)
			s_serialization->WriteRecordData(value.data(), len);
	}

	static void WriteValue(const Value& value)
	{
		const UInt8 type = static_cast<UInt8>(value.type);
		s_serialization->WriteRecordData(&type, sizeof(type));
		switch (value.type)
		{
			case ValueType::Number:
				s_serialization->WriteRecordData(&value.number, sizeof(value.number));
				break;
			case ValueType::Form:
				s_serialization->WriteRecordData(&value.formID, sizeof(value.formID));
				break;
			case ValueType::String:
				WriteString(value.string);
				break;
			default:
				break;
		}
	}

	static void SaveCallback(void*)
	{
		if (!s_serialization)
			return;

		std::vector<std::pair<ExtraDataList*, ItemState*>> liveStates;
		for (auto& it : s_states)
		{
			if (!it.first || IsEmpty(it.second))
				continue;

			if (it.second.locator.kind == LocatorKind::None)
				continue;

			if (ResolveLocator(it.second.locator) != it.first)
				continue;

			liveStates.push_back({ it.first, &it.second });
		}

		if (liveStates.empty())
			return;

		s_serialization->OpenRecord(kRecord_InvRefData, kRecordVersion);
		const UInt32 liveStateCount = static_cast<UInt32>(liveStates.size());
		s_serialization->WriteRecordData(&liveStateCount, sizeof(liveStateCount));

		for (const auto& live : liveStates)
		{
			const auto& state = *live.second;
			const UInt8 kind = static_cast<UInt8>(state.locator.kind);
			s_serialization->WriteRecordData(&kind, sizeof(kind));
			s_serialization->WriteRecordData(&state.locator.ownerRefID, sizeof(state.locator.ownerRefID));
			s_serialization->WriteRecordData(&state.locator.baseFormID, sizeof(state.locator.baseFormID));
			s_serialization->WriteRecordData(&state.locator.extendIndex, sizeof(state.locator.extendIndex));

			UInt16 namespaceCount = 0;
			for (const auto& nsIt : state.namespaces)
				if (!nsIt.second.empty())
					++namespaceCount;

			s_serialization->WriteRecordData(&namespaceCount, sizeof(namespaceCount));
			for (const auto& nsIt : state.namespaces)
			{
				if (nsIt.second.empty())
					continue;

				WriteString(nsIt.first);
				const UInt16 keyCount = static_cast<UInt16>(std::min<size_t>(nsIt.second.size(), 0xFFFF));
				s_serialization->WriteRecordData(&keyCount, sizeof(keyCount));
				UInt16 written = 0;
				for (const auto& keyIt : nsIt.second)
				{
					if (written++ == 0xFFFF)
						break;

					WriteString(keyIt.first);
					WriteValue(keyIt.second);
				}
			}
		}
	}

	struct BufferReader
	{
		const UInt8* pos;
		const UInt8* end;

		bool Read(void* out, UInt32 size)
		{
			if (size > static_cast<UInt32>(end - pos))
				return false;
			memcpy(out, pos, size);
			pos += size;
			return true;
		}

		bool Read8(UInt8& out) { return Read(&out, sizeof(out)); }
		bool Read16(UInt16& out) { return Read(&out, sizeof(out)); }
		bool Read32(UInt32& out) { return Read(&out, sizeof(out)); }

		bool ReadString(std::string& out)
		{
			UInt16 len = 0;
			if (!Read16(len))
				return false;
			if (len > static_cast<UInt32>(end - pos))
				return false;
			out.assign(reinterpret_cast<const char*>(pos), len);
			pos += len;
			return true;
		}
	};

	static bool ReadValue(BufferReader& reader, Value& value)
	{
		UInt8 type = 0;
		if (!reader.Read8(type))
			return false;

		value = Value{};
		value.type = static_cast<ValueType>(type);
		switch (value.type)
		{
			case ValueType::Number:
				return reader.Read(&value.number, sizeof(value.number));
			case ValueType::Form:
				return reader.Read32(value.formID);
			case ValueType::String:
				return reader.ReadString(value.string);
			default:
				value.type = ValueType::None;
				return true;
		}
	}

	static void LoadStateRecord(BufferReader& reader)
	{
		UInt8 kind = 0;
		Locator locator;
		UInt16 namespaceCount = 0;

		if (!reader.Read8(kind) ||
			!reader.Read32(locator.ownerRefID) ||
			!reader.Read32(locator.baseFormID) ||
			!reader.Read32(locator.extendIndex) ||
			!reader.Read16(namespaceCount))
		{
			return;
		}

		locator.kind = static_cast<LocatorKind>(kind);

		UInt32 resolvedOwner = 0;
		if (!s_serialization->ResolveRefID(locator.ownerRefID, &resolvedOwner))
			locator.kind = LocatorKind::None;
		locator.ownerRefID = resolvedOwner;

		if (locator.baseFormID)
		{
			UInt32 resolvedBase = 0;
			if (s_serialization->ResolveRefID(locator.baseFormID, &resolvedBase))
				locator.baseFormID = resolvedBase;
			else
				locator.baseFormID = 0;
		}

		ItemState state;
		state.locator = locator;

		for (UInt16 nsIndex = 0; nsIndex < namespaceCount; ++nsIndex)
		{
			std::string ns;
			UInt16 keyCount = 0;
			if (!reader.ReadString(ns) || !reader.Read16(keyCount))
				return;

			auto& keys = state.namespaces[ns];
			for (UInt16 keyIndex = 0; keyIndex < keyCount; ++keyIndex)
			{
				std::string key;
				Value value;
				if (!reader.ReadString(key) || !ReadValue(reader, value))
					return;

				if (value.type == ValueType::Form && value.formID)
				{
					UInt32 resolvedForm = 0;
					if (s_serialization->ResolveRefID(value.formID, &resolvedForm))
						value.formID = resolvedForm;
					else
						continue;
				}

				if (value.type != ValueType::None)
					keys[key] = std::move(value);
			}
		}

		ExtraDataList* xData = ResolveLocator(state.locator);
		if (xData && !IsEmpty(state))
			s_states[xData] = std::move(state);
	}

	static void LoadCallback(void*)
	{
		if (!s_serialization)
			return;

		UInt32 type = 0;
		UInt32 version = 0;
		UInt32 length = 0;
		while (s_serialization->GetNextRecordInfo(&type, &version, &length))
		{
			if (type != kRecord_InvRefData || version > kRecordVersion)
			{
				std::vector<UInt8> skip(length);
				if (length)
					s_serialization->ReadRecordData(skip.data(), length);
				continue;
			}

			std::vector<UInt8> buffer(length);
			if (length && s_serialization->ReadRecordData(buffer.data(), length) != length)
				continue;

			BufferReader reader{ buffer.data(), buffer.data() + buffer.size() };
			UInt32 stateCount = 0;
			if (!reader.Read32(stateCount))
				continue;

			for (UInt32 i = 0; i < stateCount; ++i)
				LoadStateRecord(reader);
		}
	}

	static void ClearAllCallback(void*)
	{
		s_states.clear();
	}

	static bool ExtractNamespaceKey(COMMAND_ARGS, char* ns, char* key)
	{
		ns[0] = 0;
		key[0] = 0;
		return s_extractArgsEx &&
			s_extractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, ns, key);
	}

	static ParamInfo kParams_NamespaceKey[2] = {
		{ "namespace", kParamType_String, 0 },
		{ "key", kParamType_String, 0 },
	};

	static ParamInfo kParams_NamespaceKeyFloat[3] = {
		{ "namespace", kParamType_String, 0 },
		{ "key", kParamType_String, 0 },
		{ "value", kParamType_Float, 0 },
	};

	static ParamInfo kParams_NamespaceKeyForm[3] = {
		{ "namespace", kParamType_String, 0 },
		{ "key", kParamType_String, 0 },
		{ "form", kParamType_AnyForm, 0 },
	};

	static ParamInfo kParams_NamespaceKeyString[3] = {
		{ "namespace", kParamType_String, 0 },
		{ "key", kParamType_String, 0 },
		{ "value", kParamType_String, 0 },
	};

	static ParamInfo kParams_Namespace[1] = {
		{ "namespace", kParamType_String, 0 },
	};

	DEFINE_COMMAND_PLUGIN(ITRSetInvRefDataFloat, "sets namespaced numeric data on an inventory reference or placed ref", 1, 3, kParams_NamespaceKeyFloat)
	DEFINE_COMMAND_PLUGIN(ITRGetInvRefDataFloat, "gets namespaced numeric data from an inventory reference or placed ref", 1, 2, kParams_NamespaceKey)
	DEFINE_COMMAND_PLUGIN(ITRSetInvRefDataForm, "sets namespaced form data on an inventory reference or placed ref", 1, 3, kParams_NamespaceKeyForm)
	DEFINE_COMMAND_PLUGIN(ITRGetInvRefDataForm, "gets namespaced form data from an inventory reference or placed ref", 1, 2, kParams_NamespaceKey)
	DEFINE_COMMAND_PLUGIN(ITRSetInvRefDataString, "sets namespaced string data on an inventory reference or placed ref", 1, 3, kParams_NamespaceKeyString)
	DEFINE_COMMAND_PLUGIN(ITRGetInvRefDataString, "gets namespaced string data from an inventory reference or placed ref", 1, 2, kParams_NamespaceKey)
	DEFINE_COMMAND_PLUGIN(ITRHasInvRefData, "returns whether namespaced data exists on an inventory reference or placed ref", 1, 2, kParams_NamespaceKey)
	DEFINE_COMMAND_PLUGIN(ITRClearInvRefData, "clears one namespaced data key from an inventory reference or placed ref", 1, 2, kParams_NamespaceKey)
	DEFINE_COMMAND_PLUGIN(ITRClearInvRefDataNamespace, "clears one data namespace from an inventory reference or placed ref", 1, 1, kParams_Namespace)
}

namespace
{
bool Cmd_ITRSetInvRefDataFloat_Execute(COMMAND_ARGS)
{
	*result = 0;
	char ns[0x100] = "";
	char key[0x100] = "";
	float value = 0.0f;
	if (!s_extractArgsEx || !s_extractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, ns, key, &value))
		return true;

	Target target;
	if (!ResolveTarget(thisObj, true, target))
		return true;

	Value stored;
	stored.type = ValueType::Number;
	stored.number = value;
	SetValue(target, ns, key, stored);
	*result = 1;
	return true;
}

bool Cmd_ITRGetInvRefDataFloat_Execute(COMMAND_ARGS)
{
	*result = 0;
	char ns[0x100] = "";
	char key[0x100] = "";
	if (!ExtractNamespaceKey(PASS_COMMAND_ARGS, ns, key))
		return true;

	Target target;
	if (!ResolveTarget(thisObj, false, target))
		return true;

	if (auto* value = FindValue(target, ns, key); value && value->type == ValueType::Number)
	{
		s_states[target.xData].locator = target.locator;
		*result = value->number;
	}
	return true;
}

bool Cmd_ITRSetInvRefDataForm_Execute(COMMAND_ARGS)
{
	*result = 0;
	char ns[0x100] = "";
	char key[0x100] = "";
	TESForm* form = nullptr;
	if (!s_extractArgsEx || !s_extractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, ns, key, &form))
		return true;

	Target target;
	if (!ResolveTarget(thisObj, true, target))
		return true;

	Value stored;
	stored.type = ValueType::Form;
	stored.formID = form ? form->refID : 0;
	SetValue(target, ns, key, stored);
	*result = 1;
	return true;
}

bool Cmd_ITRGetInvRefDataForm_Execute(COMMAND_ARGS)
{
	*reinterpret_cast<UInt32*>(result) = 0;
	char ns[0x100] = "";
	char key[0x100] = "";
	if (!ExtractNamespaceKey(PASS_COMMAND_ARGS, ns, key))
		return true;

	Target target;
	if (!ResolveTarget(thisObj, false, target))
		return true;

	if (auto* value = FindValue(target, ns, key); value && value->type == ValueType::Form)
	{
		s_states[target.xData].locator = target.locator;
		*reinterpret_cast<UInt32*>(result) = value->formID;
	}
	return true;
}

bool Cmd_ITRSetInvRefDataString_Execute(COMMAND_ARGS)
{
	*result = 0;
	char ns[0x100] = "";
	char key[0x100] = "";
	char stringValue[0x400] = "";
	if (!s_extractArgsEx || !s_extractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, ns, key, stringValue))
		return true;

	Target target;
	if (!ResolveTarget(thisObj, true, target))
		return true;

	Value stored;
	stored.type = ValueType::String;
	stored.string = stringValue;
	SetValue(target, ns, key, stored);
	*result = 1;
	return true;
}

bool Cmd_ITRGetInvRefDataString_Execute(COMMAND_ARGS)
{
	*result = 0;
	char ns[0x100] = "";
	char key[0x100] = "";
	if (!ExtractNamespaceKey(PASS_COMMAND_ARGS, ns, key))
	{
		if (s_stringInterface)
			s_stringInterface->Assign(PASS_COMMAND_ARGS, "");
		return true;
	}

	Target target;
	if (!ResolveTarget(thisObj, false, target))
	{
		if (s_stringInterface)
			s_stringInterface->Assign(PASS_COMMAND_ARGS, "");
		return true;
	}

	const char* out = "";
	if (auto* value = FindValue(target, ns, key); value && value->type == ValueType::String)
	{
		s_states[target.xData].locator = target.locator;
		out = value->string.c_str();
	}

	if (s_stringInterface)
		s_stringInterface->Assign(PASS_COMMAND_ARGS, out);
	return true;
}

bool Cmd_ITRHasInvRefData_Execute(COMMAND_ARGS)
{
	*result = 0;
	char ns[0x100] = "";
	char key[0x100] = "";
	if (!ExtractNamespaceKey(PASS_COMMAND_ARGS, ns, key))
		return true;

	Target target;
	if (!ResolveTarget(thisObj, false, target))
		return true;

	if (FindValue(target, ns, key))
	{
		s_states[target.xData].locator = target.locator;
		*result = 1;
	}
	return true;
}

bool Cmd_ITRClearInvRefData_Execute(COMMAND_ARGS)
{
	*result = 0;
	char ns[0x100] = "";
	char key[0x100] = "";
	if (!ExtractNamespaceKey(PASS_COMMAND_ARGS, ns, key))
		return true;

	Target target;
	if (!ResolveTarget(thisObj, false, target))
		return true;

	*result = ClearValue(target, ns, key) ? 1 : 0;
	return true;
}

bool Cmd_ITRClearInvRefDataNamespace_Execute(COMMAND_ARGS)
{
	*result = 0;
	char ns[0x100] = "";
	if (!s_extractArgsEx || !s_extractArgsEx(paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList, ns))
		return true;

	Target target;
	if (!ResolveTarget(thisObj, false, target))
		return true;

	*result = ClearNamespace(target, ns) ? 1 : 0;
	return true;
}
}

namespace InvRefData
{
	bool Init(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		s_serialization = static_cast<NVSESerializationInterface*>(nvse->QueryInterface(kInterface_Serialization));
		s_stringInterface = static_cast<NVSEStringVarInterface*>(nvse->QueryInterface(kInterface_StringVar));

		auto* script = static_cast<NVSEScriptInterface*>(nvse->QueryInterface(kInterface_Script));
		if (script)
			s_extractArgsEx = script->ExtractArgsEx;

		auto* data = static_cast<NVSEDataInterface*>(nvse->QueryInterface(kInterface_Data));
		if (data)
			s_getInventoryRefForID = reinterpret_cast<InventoryRefGetForID_t>(
				data->GetFunc(NVSEDataInterface::kNVSEData_InventoryReferenceGetForRefID));

		if (!s_serialization || !s_stringInterface || !s_extractArgsEx || !s_getInventoryRefForID)
		{
			Log("InvRefData: missing required NVSE interface");
			return false;
		}

		s_serialization->SetSaveCallback(g_pluginHandle, SaveCallback);
		s_serialization->SetLoadCallback(g_pluginHandle, LoadCallback);
		s_serialization->SetNewGameCallback(g_pluginHandle, ClearAllCallback);
		s_serialization->SetPreLoadCallback(g_pluginHandle, ClearAllCallback);

		InstallCopyHook();
		return true;
	}

	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_ITRSetInvRefDataFloat);
		nvse->RegisterCommand(&kCommandInfo_ITRGetInvRefDataFloat);
		nvse->RegisterCommand(&kCommandInfo_ITRSetInvRefDataForm);
		nvse->RegisterTypedCommand(&kCommandInfo_ITRGetInvRefDataForm, kRetnType_Form);
		nvse->RegisterCommand(&kCommandInfo_ITRSetInvRefDataString);
		nvse->RegisterTypedCommand(&kCommandInfo_ITRGetInvRefDataString, kRetnType_String);
		nvse->RegisterCommand(&kCommandInfo_ITRHasInvRefData);
		nvse->RegisterCommand(&kCommandInfo_ITRClearInvRefData);
		nvse->RegisterCommand(&kCommandInfo_ITRClearInvRefDataNamespace);
	}
}
