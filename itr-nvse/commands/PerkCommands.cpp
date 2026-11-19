#include "commands/PerkCommands.h"
#include "internal/EngineFunctions.h"
#include "internal/PerkEvaluator.h"
#include "internal/PerkIndex.h"
#include "internal/StringAlloc.h"
#include "internal/globals.h"

#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "nvse/PluginAPI.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern const _ExtractArgs ExtractArgs;
extern NVSEArrayVarInterface* g_arrInterface;

namespace {

	using Array = NVSEArrayVarInterface::Array;
	using Element = NVSEArrayVarInterface::Element;

	using PerkEvaluator::ApplicablePerkResult;
	using PerkEvaluator::BlockerText;
	using PerkEvaluator::EligibilityResult;
	using PerkEvaluator::EntryMatch;
	using PerkEvaluator::EvalContext;
	using PerkEvaluator::EvaluateApplicablePerk;
	using PerkEvaluator::EvaluatedCondition;
	using PerkEvaluator::EvaluateEligibility;
	using PerkEvaluator::IncludeApplicablePerk;
	using PerkEvaluator::IncludeInEligibleList;
	using PerkEvaluator::StateDelta;

	using PerkIndex::ComparisonName;
	using PerkIndex::ConditionDescriptor;
	using PerkIndex::EntryPointName;
	using PerkIndex::FindPerk;
	using PerkIndex::FormatAVName;
	using PerkIndex::FormName;
	using PerkIndex::FunctionName;
	using PerkIndex::GetPerks;
	using PerkIndex::Kind;
	using PerkIndex::KindName;
	using PerkIndex::MetadataRequirement;
	using PerkIndex::MinLevelRequirement;
	using PerkIndex::PerkDescriptor;
	using PerkIndex::RequiredValue;
	using PerkIndex::RunOnName;
	using PerkIndex::SourceName;

	constexpr UInt32 kConsoleDumpLimit = 32;

	void AddStringMapValue(std::vector<const char*>& keys, std::vector<Element>& values, const char* key, const Element& value)
	{
		keys.push_back(key);
		values.push_back(value);
	}

	Array* CreateStringMap(Script* scriptObj, const std::vector<const char*>& keys, const std::vector<Element>& values)
	{
		if (!g_arrInterface)
			return nullptr;
		if (keys.empty())
			return g_arrInterface->CreateStringMap(nullptr, nullptr, 0, scriptObj);
		return g_arrInterface->CreateStringMap(const_cast<const char**>(keys.data()), values.data(), static_cast<UInt32>(keys.size()), scriptObj);
	}

	Array* CreateArray(Script* scriptObj)
	{
		return g_arrInterface ? g_arrInterface->CreateArray(nullptr, 0, scriptObj) : nullptr;
	}

	void AssignArrayResult(Array* arr, double* result)
	{
		if (g_arrInterface && arr)
			g_arrInterface->AssignCommandResult(arr, result);
	}

	void ParseFormIDSet(Array* arr, std::unordered_set<UInt32>& out)
	{
		if (!g_arrInterface || !arr)
			return;

		const UInt32 size = g_arrInterface->GetArraySize(arr);
		if (!size)
			return;

		std::vector<Element> values(size);
		g_arrInterface->GetElements(arr, values.data(), nullptr);
		for (auto& value : values) {
			if (value.GetType() == Element::kType_Form && value.Form())
				out.insert(value.Form()->refID);
			else if (value.GetType() == Element::kType_Numeric)
				out.insert(static_cast<UInt32>(value.Number()));
		}
	}

	void ParseNumericMap(Array* arr, std::unordered_map<UInt32, double>& out)
	{
		if (!g_arrInterface || !arr)
			return;

		const UInt32 size = g_arrInterface->GetArraySize(arr);
		if (!size)
			return;

		std::vector<Element> values(size);
		std::vector<Element> keys(size);
		g_arrInterface->GetElements(arr, values.data(), keys.data());
		for (UInt32 i = 0; i < size; i++) {
			if (values[i].GetType() != Element::kType_Numeric)
				continue;

			UInt32 key = 0;
			if (keys[i].GetType() == Element::kType_Numeric)
				key = static_cast<UInt32>(keys[i].Number());
			else if (keys[i].GetType() == Element::kType_String && keys[i].String())
				key = strtoul(keys[i].String(), nullptr, 0);

			if (key)
				out[key] = values[i].Number();
		}
	}

	bool GetMapElement(Array* arr, const char* key, Element& out)
	{
		if (!g_arrInterface || !arr)
			return false;

		Element keyElem(key);
		return g_arrInterface->GetElement(arr, keyElem, out);
	}

	StateDelta ParseStateDelta(Array* arr)
	{
		StateDelta delta;
		if (!g_arrInterface || !arr)
			return delta;

		Element value;
		if (GetMapElement(arr, "level", value) && value.GetType() == Element::kType_Numeric) {
			delta.hasLevel = true;
			delta.level = value.Number();
		}

		if (GetMapElement(arr, "av", value) && value.GetType() == Element::kType_Array)
			ParseNumericMap(value.Array(), delta.av);
		if (GetMapElement(arr, "avDelta", value) && value.GetType() == Element::kType_Array)
			ParseNumericMap(value.Array(), delta.avDelta);
		if (GetMapElement(arr, "itemsAdded", value) && value.GetType() == Element::kType_Array)
			ParseNumericMap(value.Array(), delta.itemsAdded);
		if (GetMapElement(arr, "itemsRemoved", value) && value.GetType() == Element::kType_Array)
			ParseNumericMap(value.Array(), delta.itemsRemoved);
		if (GetMapElement(arr, "perksAdded", value) && value.GetType() == Element::kType_Array)
			ParseFormIDSet(value.Array(), delta.perksAdded);
		if (GetMapElement(arr, "perksRemoved", value) && value.GetType() == Element::kType_Array)
			ParseFormIDSet(value.Array(), delta.perksRemoved);

		return delta;
	}

	Array* ConditionToMap(const ConditionDescriptor& descriptor, Script* scriptObj)
	{
		std::vector<const char*> keys;
		std::vector<Element> values;
		auto* perkForm = (TESForm*)Engine::LookupFormByID(descriptor.perkFormID);

		AddStringMapValue(keys, values, "source", Element(SourceName(descriptor.source)));
		AddStringMapValue(keys, values, "perk", Element(perkForm));
		AddStringMapValue(keys, values, "rank", Element(static_cast<double>(descriptor.rank)));
		AddStringMapValue(keys, values, "entryPoint", Element(static_cast<double>(descriptor.entryPoint)));
		AddStringMapValue(keys, values, "entryFunction", Element(static_cast<double>(descriptor.entryFunction)));
		AddStringMapValue(keys, values, "tabIndex", Element(static_cast<double>(descriptor.tabIndex)));
		AddStringMapValue(keys, values, "rawFunctionId", Element(static_cast<double>(descriptor.rawFunctionId)));
		AddStringMapValue(keys, values, "commandOpcode", Element(static_cast<double>(descriptor.commandOpcode)));
		AddStringMapValue(keys, values, "function", Element(FunctionName(descriptor.rawFunctionId)));
		AddStringMapValue(keys, values, "runOn", Element(static_cast<double>(descriptor.runOn)));
		AddStringMapValue(keys, values, "runOnName", Element(RunOnName(descriptor.runOn)));
		AddStringMapValue(keys, values, "runOnReference", Element(static_cast<TESForm*>(descriptor.runOnReference)));
		AddStringMapValue(keys, values, "comparison", Element(ComparisonName(descriptor.comparison)));
		AddStringMapValue(keys, values, "comparisonValue", Element(static_cast<double>(RequiredValue(descriptor))));
		AddStringMapValue(keys, values, "isOr", Element(descriptor.isOr ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "usesGlobalComparison", Element(descriptor.usesGlobalComparison ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "kind", Element(KindName(descriptor.kind)));
		AddStringMapValue(keys, values, "argForm", Element(descriptor.argForm));
		AddStringMapValue(keys, values, "argValue", Element(static_cast<double>(descriptor.argValue)));

		return CreateStringMap(scriptObj, keys, values);
	}

	Array* BlockerToMap(const ConditionDescriptor& descriptor, const EvaluatedCondition& evaluated, Script* scriptObj)
	{
		std::vector<const char*> keys;
		std::vector<Element> values;

		AddStringMapValue(keys, values, "kind", Element(KindName(descriptor.kind)));
		AddStringMapValue(keys, values, "function", Element(FunctionName(descriptor.rawFunctionId)));
		AddStringMapValue(keys, values, "comparison", Element(ComparisonName(descriptor.comparison)));
		AddStringMapValue(keys, values, "currentValue", Element(evaluated.currentValue));
		AddStringMapValue(keys, values, "requiredValue", Element(evaluated.requiredValue));
		AddStringMapValue(keys, values, "missingValue", Element(evaluated.missingValue));
		AddStringMapValue(keys, values, "argForm", Element(descriptor.argForm));
		AddStringMapValue(keys, values, "argValue", Element(static_cast<double>(descriptor.argValue)));
		AddStringMapValue(keys, values, "text", Element(BlockerText(descriptor, evaluated).c_str()));

		return CreateStringMap(scriptObj, keys, values);
	}

	Array* ConditionsArray(const PerkDescriptor& descriptor, Script* scriptObj)
	{
		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return nullptr;

		if (descriptor.minLevel > 0)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MinLevelRequirement(descriptor), scriptObj)));
		else
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MetadataRequirement(descriptor, Kind::NonLevelUp), scriptObj)));
		if (descriptor.trait)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MetadataRequirement(descriptor, Kind::Trait), scriptObj)));
		if (!descriptor.playable)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MetadataRequirement(descriptor, Kind::NonPlayable), scriptObj)));
		if (descriptor.hidden)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(MetadataRequirement(descriptor, Kind::Hidden), scriptObj)));

		for (const auto& group : descriptor.requirements) {
			for (const auto& condition : group.descriptors)
				g_arrInterface->AppendElement(arr, Element(ConditionToMap(condition, scriptObj)));
		}

		for (const auto& entry : descriptor.entries) {
			for (const auto& condition : entry.filters)
				g_arrInterface->AppendElement(arr, Element(ConditionToMap(condition, scriptObj)));
		}

		return arr;
	}

	Array* BlockersArray(const EligibilityResult& eligibility, Script* scriptObj)
	{
		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return nullptr;

		for (const auto& [descriptor, evaluated] : eligibility.blockers)
			g_arrInterface->AppendElement(arr, Element(BlockerToMap(descriptor, evaluated, scriptObj)));

		return arr;
	}

	Array* IndeterminateArray(const EligibilityResult& eligibility, Script* scriptObj)
	{
		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return nullptr;

		for (const auto& descriptor : eligibility.indeterminate)
			g_arrInterface->AppendElement(arr, Element(ConditionToMap(descriptor, scriptObj)));

		return arr;
	}

	Array* EligibilityToMap(const EligibilityResult& eligibility, Script* scriptObj)
	{
		std::vector<const char*> keys;
		std::vector<Element> values;

		AddStringMapValue(keys, values, "perk", Element(reinterpret_cast<TESForm*>(eligibility.perk)));
		AddStringMapValue(keys, values, "name", Element(eligibility.name.c_str()));
		AddStringMapValue(keys, values, "ownedRank", Element(static_cast<double>(eligibility.ownedRank)));
		AddStringMapValue(keys, values, "nextRank", Element(static_cast<double>(eligibility.nextRank)));
		AddStringMapValue(keys, values, "maxRank", Element(static_cast<double>(eligibility.maxRank)));
		AddStringMapValue(keys, values, "eligible", Element(eligibility.eligible ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "status", Element(eligibility.status.c_str()));
		AddStringMapValue(keys, values, "blockers", Element(BlockersArray(eligibility, scriptObj)));
		AddStringMapValue(keys, values, "indeterminate", Element(IndeterminateArray(eligibility, scriptObj)));

		return CreateStringMap(scriptObj, keys, values);
	}

	Array* EntryMatchToMap(const EntryMatch& match, Script* scriptObj)
	{
		std::vector<const char*> keys;
		std::vector<Element> values;
		const auto* entry = match.entry;

		AddStringMapValue(keys, values, "rank", Element(static_cast<double>(entry ? entry->rank : 0)));
		AddStringMapValue(keys, values, "entryPoint", Element(static_cast<double>(entry ? entry->entryPoint : -1)));
		AddStringMapValue(keys, values, "entryPointName", Element(EntryPointName(entry ? entry->entryPoint : -1)));
		AddStringMapValue(keys, values, "entryFunction", Element(static_cast<double>(entry ? entry->function : -1)));
		AddStringMapValue(keys, values, "conditionTabs", Element(static_cast<double>(entry ? entry->conditionTabs : 0)));
		AddStringMapValue(keys, values, "match", Element(match.match.c_str()));
		AddStringMapValue(keys, values, "contextual", Element(match.contextual ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "unknown", Element(match.unknown ? 1.0 : 0.0));

		return CreateStringMap(scriptObj, keys, values);
	}

	Array* EntryMatchesArray(const ApplicablePerkResult& result, Script* scriptObj)
	{
		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return nullptr;

		for (const auto& match : result.matches)
			g_arrInterface->AppendElement(arr, Element(EntryMatchToMap(match, scriptObj)));

		return arr;
	}

	Array* ApplicablePerkToMap(const ApplicablePerkResult& result, TESForm* subject, Script* scriptObj)
	{
		const auto& eligibility = result.eligibility;
		std::vector<const char*> keys;
		std::vector<Element> values;

		AddStringMapValue(keys, values, "perk", Element(reinterpret_cast<TESForm*>(eligibility.perk)));
		AddStringMapValue(keys, values, "name", Element(eligibility.name.c_str()));
		AddStringMapValue(keys, values, "subject", Element(subject));
		AddStringMapValue(keys, values, "ownedRank", Element(static_cast<double>(eligibility.ownedRank)));
		AddStringMapValue(keys, values, "nextRank", Element(static_cast<double>(eligibility.nextRank)));
		AddStringMapValue(keys, values, "maxRank", Element(static_cast<double>(eligibility.maxRank)));
		AddStringMapValue(keys, values, "relevant", Element(result.relevant ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "eligible", Element(eligibility.eligible ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "owned", Element(result.owned ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "contextual", Element(result.contextual ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "unknown", Element(result.unknown ? 1.0 : 0.0));
		AddStringMapValue(keys, values, "status", Element(result.status.c_str()));
		AddStringMapValue(keys, values, "blockers", Element(BlockersArray(eligibility, scriptObj)));
		AddStringMapValue(keys, values, "indeterminate", Element(IndeterminateArray(eligibility, scriptObj)));
		AddStringMapValue(keys, values, "matchedEntryPoints", Element(EntryMatchesArray(result, scriptObj)));

		return CreateStringMap(scriptObj, keys, values);
	}

	const char* DisplayName(const std::string& name)
	{
		return name.empty() ? "<unnamed>" : name.c_str();
	}

	void ConsolePrintBlockers(const EligibilityResult& eligibility, const char* commandName)
	{
		if (!IsConsoleMode())
			return;

		const UInt32 formID = eligibility.perk ? eligibility.perk->refID : 0;
		Console_Print("%s >> %s (%08X): %u blocker(s)",
			commandName, DisplayName(eligibility.name), formID, static_cast<UInt32>(eligibility.blockers.size()));

		const UInt32 count = std::min<UInt32>(static_cast<UInt32>(eligibility.blockers.size()), kConsoleDumpLimit);
		for (UInt32 i = 0; i < count; i++) {
			const auto& [descriptor, evaluated] = eligibility.blockers[i];
			const std::string text = BlockerText(descriptor, evaluated);
			Console_Print("  [%u] %s", i, text.c_str());
		}

		if (eligibility.blockers.size() > kConsoleDumpLimit)
			Console_Print("  ... %u more", static_cast<UInt32>(eligibility.blockers.size() - kConsoleDumpLimit));
	}

	void ConsolePrintIndeterminate(const EligibilityResult& eligibility)
	{
		if (!IsConsoleMode() || eligibility.indeterminate.empty())
			return;

		Console_Print("  unknown requirement(s): %u", static_cast<UInt32>(eligibility.indeterminate.size()));
		const UInt32 count = std::min<UInt32>(static_cast<UInt32>(eligibility.indeterminate.size()), kConsoleDumpLimit);
		for (UInt32 i = 0; i < count; i++) {
			const auto& descriptor = eligibility.indeterminate[i];
			Console_Print("    [%u] %s kind=%s arg=%u",
				i, FunctionName(descriptor.rawFunctionId), KindName(descriptor.kind), descriptor.argValue);
		}
	}

	void ConsolePrintEligibility(const EligibilityResult& eligibility)
	{
		if (!IsConsoleMode())
			return;

		const UInt32 formID = eligibility.perk ? eligibility.perk->refID : 0;
		Console_Print("GetPerkEligibility >> %s (%08X): %s eligible=%u owned=%u next=%u max=%u",
			DisplayName(eligibility.name), formID, eligibility.status.c_str(),
			eligibility.eligible ? 1 : 0, eligibility.ownedRank, eligibility.nextRank, eligibility.maxRank);

		if (!eligibility.blockers.empty())
			ConsolePrintBlockers(eligibility, "  blockers");
		ConsolePrintIndeterminate(eligibility);
	}

	std::string DescriptorArgText(const ConditionDescriptor& descriptor)
	{
		if (descriptor.argForm)
			return FormName(descriptor.argForm);

		switch (descriptor.kind) {
			case Kind::ActorValue:
			case Kind::BaseActorValue:
			case Kind::PermanentActorValue:
				return FormatAVName(descriptor.argValue);
			default: {
				char buf[32];
				snprintf(buf, sizeof(buf), "%u", descriptor.argValue);
				return buf;
			}
		}
	}

	void ConsolePrintConditions(const PerkDescriptor& descriptor)
	{
		if (!IsConsoleMode())
			return;

		UInt32 total = 1;
		total += descriptor.trait ? 1 : 0;
		total += descriptor.playable ? 0 : 1;
		total += descriptor.hidden ? 1 : 0;
		for (const auto& group : descriptor.requirements)
			total += static_cast<UInt32>(group.descriptors.size());
		for (const auto& entry : descriptor.entries)
			total += static_cast<UInt32>(entry.filters.size());

		Console_Print("GetPerkConditions >> %s (%08X): %u condition(s)",
			DisplayName(descriptor.name), descriptor.formID, total);

		UInt32 printed = 0;
		if (descriptor.minLevel > 0 && printed < kConsoleDumpLimit) {
			Console_Print("  [%u] data GetLevel >= %u", printed, descriptor.minLevel);
			printed++;
		} else if (printed < kConsoleDumpLimit) {
			Console_Print("  [%u] data MinLevel = 0 (not level-up selectable)", printed);
			printed++;
		}
		if (descriptor.trait && printed < kConsoleDumpLimit) {
			Console_Print("  [%u] data Trait = true", printed);
			printed++;
		}
		if (!descriptor.playable && printed < kConsoleDumpLimit) {
			Console_Print("  [%u] data Playable = false", printed);
			printed++;
		}
		if (descriptor.hidden && printed < kConsoleDumpLimit) {
			Console_Print("  [%u] data Hidden = true", printed);
			printed++;
		}

		for (const auto& group : descriptor.requirements) {
			for (const auto& condition : group.descriptors) {
				if (printed >= kConsoleDumpLimit)
					break;

				const std::string argText = DescriptorArgText(condition);
				Console_Print("  [%u] req %s %s %.0f arg=%s",
					printed, FunctionName(condition.rawFunctionId), ComparisonName(condition.comparison),
					static_cast<double>(RequiredValue(condition)), argText.c_str());
				printed++;
			}
		}

		for (const auto& entry : descriptor.entries) {
			for (const auto& condition : entry.filters) {
				if (printed >= kConsoleDumpLimit)
					break;

				const std::string argText = DescriptorArgText(condition);
				Console_Print("  [%u] entry ep=%d rank=%u tab=%d runOn=%s %s %s %.0f arg=%s",
					printed, condition.entryPoint, condition.rank, condition.tabIndex,
					RunOnName(condition.runOn), FunctionName(condition.rawFunctionId),
					ComparisonName(condition.comparison), static_cast<double>(RequiredValue(condition)),
					argText.c_str());
				printed++;
			}
		}

		if (total > kConsoleDumpLimit)
			Console_Print("  ... %u more", total - kConsoleDumpLimit);
	}

	void ConsolePrintApplicablePerks(TESForm* subject, UInt32 total, const std::vector<ApplicablePerkResult>& rows)
	{
		if (!IsConsoleMode())
			return;

		const std::string subjectName = FormName(subject);
		Console_Print("GetPerksForForm >> %s (%08X): %u perk(s)",
			subjectName.empty() ? "<unnamed>" : subjectName.c_str(), subject ? subject->refID : 0, total);

		for (UInt32 i = 0; i < rows.size(); i++) {
			const auto& row = rows[i];
			Console_Print("  [%u] %s (%08X) status=%s owned=%u next=%u/%u matches=%u",
				i, DisplayName(row.eligibility.name),
				row.eligibility.perk ? row.eligibility.perk->refID : 0,
				row.status.c_str(), row.owned ? 1 : 0,
				row.eligibility.nextRank, row.eligibility.maxRank,
				static_cast<UInt32>(row.matches.size()));
		}

		if (total > kConsoleDumpLimit)
			Console_Print("  ... %u more", total - kConsoleDumpLimit);
	}

	BGSPerk* ExtractPerk(TESForm* form)
	{
		return form && form->typeID == kFormType_Perk ? static_cast<BGSPerk*>(form) : nullptr;
	}

	EvalContext MakeEvalContext(Array* deltaArray)
	{
		EvalContext ctx;
		ctx.player = PlayerCharacter::GetSingleton();
		ctx.delta = ParseStateDelta(deltaArray);
		return ctx;
	}

	ParamInfo kParams_PerkDeltaFlags[3] = {
		{"perk", kParamType_AnyForm, 0},
		{"delta", kParamType_Array, 1},
		{"flags", kParamType_Integer, 1},
	};

	ParamInfo kParams_DeltaFlags[2] = {
		{"delta", kParamType_Array, 1},
		{"flags", kParamType_Integer, 1},
	};

	ParamInfo kParams_PerkFlags[2] = {
		{"perk", kParamType_AnyForm, 0},
		{"flags", kParamType_Integer, 1},
	};

	ParamInfo kParams_FormDeltaFlags[3] = {
		{"subject", kParamType_AnyForm, 0},
		{"delta", kParamType_Array, 1},
		{"flags", kParamType_Integer, 1},
	};

	ParamInfo kParams_FlagsLevel[2] = {
		{"flags", kParamType_Integer, 0},
		{"levelOverride", kParamType_Integer, 1},
	};

	DEFINE_COMMAND_PLUGIN(GetPerkEligibility, "Returns structured player eligibility for a perk", 0, 3, kParams_PerkDeltaFlags);
	DEFINE_COMMAND_PLUGIN(GetPerkBlockers, "Returns structured blockers for a perk", 0, 3, kParams_PerkDeltaFlags);
	DEFINE_COMMAND_PLUGIN(GetEligiblePerks, "Returns currently level-up selectable perks", 0, 2, kParams_DeltaFlags);
	DEFINE_COMMAND_PLUGIN(GetPerkConditions, "Returns parsed perk requirement and entry-point descriptors", 0, 2, kParams_PerkFlags);
	DEFINE_COMMAND_PLUGIN(GetEligiblePerksEx, "GetEligiblePerks for runtime-compiled scripts, flags plus optional level override", 0, 2, kParams_FlagsLevel);
	DEFINE_COMMAND_PLUGIN(GetPerksForForm, "Returns perks relevant to a weapon, armour, or item form", 0, 3, kParams_FormDeltaFlags);

	bool Cmd_GetPerkEligibility_Execute(COMMAND_ARGS)
	{
		*result = 0;
		TESForm* form = nullptr;
		Array* deltaArray = nullptr;
		UInt32 flags = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &form, &deltaArray, &flags) || !g_arrInterface)
			return true;
		if (!PerkIndex::EnsureIndex())
			return true;

		auto* perk = ExtractPerk(form);
		const auto* descriptor = FindPerk(perk);
		if (!descriptor)
			return true;

		auto eligibility = EvaluateEligibility(*descriptor, MakeEvalContext(deltaArray), flags);
		ConsolePrintEligibility(eligibility);
		AssignArrayResult(EligibilityToMap(eligibility, scriptObj), result);
		return true;
	}

	bool Cmd_GetPerkBlockers_Execute(COMMAND_ARGS)
	{
		*result = 0;
		TESForm* form = nullptr;
		Array* deltaArray = nullptr;
		UInt32 flags = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &form, &deltaArray, &flags) || !g_arrInterface)
			return true;
		if (!PerkIndex::EnsureIndex())
			return true;

		auto* perk = ExtractPerk(form);
		const auto* descriptor = FindPerk(perk);
		if (!descriptor)
			return true;

		auto eligibility = EvaluateEligibility(*descriptor, MakeEvalContext(deltaArray), flags);
		ConsolePrintBlockers(eligibility, "GetPerkBlockers");
		ConsolePrintIndeterminate(eligibility);
		AssignArrayResult(BlockersArray(eligibility, scriptObj), result);
		return true;
	}

	bool BuildEligiblePerksResult(const EvalContext& ctx, UInt32 flags, Script* scriptObj, double* result)
	{
		if (!g_arrInterface || !PerkIndex::EnsureIndex())
			return true;

		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return true;

		std::vector<EligibilityResult> consoleRows;
		UInt32 consoleCount = 0;
		for (const auto& descriptor : GetPerks()) {
			auto eligibility = EvaluateEligibility(descriptor, ctx, flags);
			if (IncludeInEligibleList(eligibility, descriptor, flags)) {
				if (IsConsoleMode()) {
					consoleCount++;
					if (consoleRows.size() < kConsoleDumpLimit)
						consoleRows.push_back(eligibility);
				}
				g_arrInterface->AppendElement(arr, Element(EligibilityToMap(eligibility, scriptObj)));
			}
		}

		if (IsConsoleMode()) {
			Console_Print("GetEligiblePerks >> %u perk(s)", consoleCount);
			for (UInt32 i = 0; i < consoleRows.size(); i++) {
				const auto& eligibility = consoleRows[i];
				Console_Print("  [%u] %s (%08X) next=%u/%u",
					i, DisplayName(eligibility.name),
					eligibility.perk ? eligibility.perk->refID : 0,
					eligibility.nextRank, eligibility.maxRank);
			}
			if (consoleCount > kConsoleDumpLimit)
				Console_Print("  ... %u more", consoleCount - kConsoleDumpLimit);
		}

		AssignArrayResult(arr, result);
		return true;
	}

	bool Cmd_GetEligiblePerks_Execute(COMMAND_ARGS)
	{
		*result = 0;
		Array* deltaArray = nullptr;
		UInt32 flags = 0;
		ExtractArgs(EXTRACT_ARGS, &deltaArray, &flags);
		return BuildEligiblePerksResult(MakeEvalContext(deltaArray), flags, scriptObj, result);
	}

	bool Cmd_GetEligiblePerksEx_Execute(COMMAND_ARGS)
	{
		*result = 0;
		UInt32 flags = 0;
		UInt32 levelOverride = 0;
		ExtractArgs(EXTRACT_ARGS, &flags, &levelOverride);
		auto ctx = MakeEvalContext(nullptr);
		if (levelOverride) {
			ctx.delta.hasLevel = true;
			ctx.delta.level = levelOverride;
		}
		return BuildEligiblePerksResult(ctx, flags, scriptObj, result);
	}

	bool Cmd_GetPerkConditions_Execute(COMMAND_ARGS)
	{
		*result = 0;
		TESForm* form = nullptr;
		UInt32 flags = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &form, &flags) || !g_arrInterface)
			return true;
		if (!PerkIndex::EnsureIndex())
			return true;

		auto* perk = ExtractPerk(form);
		const auto* descriptor = FindPerk(perk);
		if (!descriptor)
			return true;

		ConsolePrintConditions(*descriptor);
		AssignArrayResult(ConditionsArray(*descriptor, scriptObj), result);
		return true;
	}

	bool Cmd_GetPerksForForm_Execute(COMMAND_ARGS)
	{
		*result = 0;
		TESForm* subject = nullptr;
		Array* deltaArray = nullptr;
		UInt32 flags = 0;
		if (!ExtractArgs(EXTRACT_ARGS, &subject, &deltaArray, &flags) || !g_arrInterface || !subject)
			return true;
		if (!PerkIndex::EnsureIndex())
			return true;

		auto* arr = CreateArray(scriptObj);
		if (!arr)
			return true;

		auto ctx = MakeEvalContext(deltaArray);
		std::vector<ApplicablePerkResult> consoleRows;
		UInt32 consoleCount = 0;
		for (const auto& descriptor : GetPerks()) {
			auto applicable = EvaluateApplicablePerk(descriptor, subject, ctx, flags);
			if (!IncludeApplicablePerk(applicable, descriptor, flags))
				continue;

			consoleCount++;
			if (consoleRows.size() < kConsoleDumpLimit)
				consoleRows.push_back(applicable);
			g_arrInterface->AppendElement(arr, Element(ApplicablePerkToMap(applicable, subject, scriptObj)));
		}

		ConsolePrintApplicablePerks(subject, consoleCount, consoleRows);
		AssignArrayResult(arr, result);
		return true;
	}

}

namespace PerkCommands {

	bool Init(void* nvse)
	{
		return nvse != nullptr;
	}

	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPerkEligibility, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPerkBlockers, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetEligiblePerks, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPerkConditions, kRetnType_Array);
		nvse->RegisterTypedCommand(&kCommandInfo_GetPerksForForm, kRetnType_Array);
	}

	void RegisterEligibleExCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterTypedCommand(&kCommandInfo_GetEligiblePerksEx, kRetnType_Array);
	}

}
