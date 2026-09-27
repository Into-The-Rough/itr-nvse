//fixes projectile hit chance in VATS
//NOT hot-reloadable - requires game restart

#include "VATSProjectileFix.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/Detours.h"
#include "internal/GameGlobals.h"
#include "internal/GameSDK.h"
#include "internal/layout/VATS.h"

#include "internal/globals.h"
#include "internal/CallTemplates.h"

#include <cstdio>
#include <share.h>

namespace VATSProjectileFix
{
	constexpr UInt32 kAddr_HookSite = 0x7ED349;
	constexpr UInt32 kAddr_FindTarget = 0x7F3C90;

	static Detours::CallDetour s_vatsMenuUpdateCall;
	typedef bool(__thiscall* VATSMenuUpdate_t)(void*);
	static bool s_enabled = false;
	static FILE* s_diagnosticLog = nullptr;
	constexpr UInt32 kMaxDiagnosticParts = 32;
	constexpr UInt32 kMaxDiagnosticRecords = 64;
	constexpr UInt32 kVATSMode_None = 0;

	struct PartDiagnostic {
		UInt32 id;
		float visibility;
		float chance;
		UInt8 onScreen;
		UInt8 calculated;
		UInt8 firstTimeShown;
		UInt8 needsRecalc;
	};

	struct TargetDiagnostic {
		UInt32 refID;
		UInt32 formType;
		UInt32 targetType;
		UInt32 count;
		UInt32 truncated;
		PartDiagnostic parts[kMaxDiagnosticParts];
	};

	struct Diagnostic {
		UInt32 mode;
		UInt32 updateResult;
		TargetDiagnostic beforeFix;
		TargetDiagnostic afterFix;
	};

	static Diagnostic s_records[kMaxDiagnosticRecords];
	static UInt32 s_recordCount = 0;
	static UInt32 s_droppedRecords = 0;
	static UInt32 s_updateCount = 0;
	static UInt32 s_hookDepth = 0;

	static void CaptureTarget(void* menu, TargetDiagnostic& snapshot)
	{
		auto* ref = VATSGetCurrentTarget();
		if (!ref) return;
		snapshot.refID = ref->refID;
		snapshot.formType = ref->typeID;
		auto* entry = ThisCall<BSSimpleListNodeView<VATSTargetView*>*>(kAddr_FindTarget, menu, ref);
		if (VATSTargetNodeIsEmpty(entry)) return;
		auto* target = entry->item;
		snapshot.targetType = target->type;
		auto* node = &target->bodyParts;
		for (; !VATSBodyPartNodeIsEmpty(node) && snapshot.count < kMaxDiagnosticParts; node = node->next) {
			auto* part = node->item;
			snapshot.parts[snapshot.count++] = {
				part->bodyPartID, part->percentVisible, part->hitChance,
				part->isOnScreen, part->chanceCalculated, part->firstTimeShown, part->needsRecalc
			};
		}
		snapshot.truncated = !VATSBodyPartNodeIsEmpty(node);
	}

	static void RecordDiagnostic(const Diagnostic& record)
	{
		if (s_recordCount && std::memcmp(&s_records[s_recordCount - 1], &record, sizeof(record)) == 0)
			return;
		if (s_recordCount < kMaxDiagnosticRecords)
			s_records[s_recordCount++] = record;
		else {
			//retain the latest state even if targeting generated more than 64 changes
			s_records[kMaxDiagnosticRecords - 1] = record;
			++s_droppedRecords;
		}
	}

	static void LogCallOwner(const char* label, UInt32 site)
	{
		if (*reinterpret_cast<UInt8*>(site) != 0xE8) {
			std::fprintf(s_diagnosticLog, "%s site=%08X opcode=%02X\n", label, site, *reinterpret_cast<UInt8*>(site));
			return;
		}
		UInt32 target = site + 5 + *reinterpret_cast<UInt32*>(site + 1);
		HMODULE module = nullptr;
		char path[MAX_PATH] = {};
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(target), &module))
			GetModuleFileNameA(module, path, sizeof(path));
		std::fprintf(s_diagnosticLog, "%s site=%08X target=%08X module=%s\n", label, site, target, path);
	}

	static void PrintTarget(const char* phase, const TargetDiagnostic& target)
	{
		std::fprintf(s_diagnosticLog, "%s ref=%08X formType=%u vatsType=%u parts=%u truncated=%u\n",
			phase, target.refID, target.formType, target.targetType, target.count, target.truncated);
		for (UInt32 i = 0; i < target.count; ++i) {
			const auto& part = target.parts[i];
			std::fprintf(s_diagnosticLog, "  part=%08X visibility=%.6f chance=%.6f onScreen=%u calculated=%u first=%u recalc=%u\n",
				part.id, part.visibility, part.chance, part.onScreen, part.calculated, part.firstTimeShown, part.needsRecalc);
		}
	}

	void Update()
	{
		//only copied values survive the hook; file i/o waits until vats has closed
		if (!s_diagnosticLog || !s_recordCount || s_hookDepth || VATSGetMode() != kVATSMode_None)
			return;
		std::fprintf(s_diagnosticLog, "VATS closed: enabled=%u updates=%u records=%u dropped=%u ownsHook=%u\n",
			s_enabled, s_updateCount, s_recordCount, s_droppedRecords, s_vatsMenuUpdateCall.OwnsPatch());
		LogCallOwner("menu update", kAddr_HookSite);
		LogCallOwner("visibility query", 0x7F4544);
		for (UInt32 i = 0; i < s_recordCount; ++i) {
			const auto& record = s_records[i];
			std::fprintf(s_diagnosticLog, "record=%u mode=%u updateResult=%u\n", i, record.mode, record.updateResult);
			PrintTarget("after original update / before fix", record.beforeFix);
			PrintTarget("after fix", record.afterFix);
		}
		std::fflush(s_diagnosticLog);
		s_recordCount = 0;
		s_droppedRecords = 0;
		s_updateCount = 0;
	}

	static bool ApplyFix(void* pThis, bool result)
	{
		if (!result || !s_enabled) return result;

		TESObjectREFR* pTargetRef = VATSGetCurrentTarget();
		if (!pTargetRef) return result;

		auto* pTargetEntry = ThisCall<BSSimpleListNodeView<VATSTargetView*>*>(kAddr_FindTarget, pThis, pTargetRef);
		if (VATSTargetNodeIsEmpty(pTargetEntry)) return result;

		VATSTargetView* pTarget = pTargetEntry->item;
		if (!VATSTargetIsProjectile(pTarget)) return result;

		auto* pIter = &pTarget->bodyParts;
		while (!VATSBodyPartNodeIsEmpty(pIter)) {
			VATSBodyPartView* pPart = pIter->item;
			if (pPart) {
				VATSBodyPartForceVisible(pPart);
				ThisCall<double>(0x7F1290, pThis, pIter);
			}
			pIter = pIter->next;
		}

		return result;
	}

	static bool __fastcall VATSMenuUpdate_Hook(void* pThis, void*)
	{
		++s_hookDepth;
		auto original = reinterpret_cast<VATSMenuUpdate_t>(s_vatsMenuUpdateCall.GetOverwrittenAddr());
		bool result = original(pThis);
		Diagnostic record = {};
		if (s_diagnosticLog) {
			++s_updateCount;
			record.mode = VATSGetMode();
			record.updateResult = result;
			if (result)
				CaptureTarget(pThis, record.beforeFix);
		}
		ApplyFix(pThis, result);
		if (s_diagnosticLog) {
			if (result && VATSGetMenu() == pThis)
				CaptureTarget(pThis, record.afterFix);
			RecordDiagnostic(record);
		}
		--s_hookDepth;
		return result;
	}

	void Init(bool enabled, bool diagnostic)
	{
		s_enabled = enabled;
		char path[MAX_PATH] = "off";
		if (diagnostic) {
			GetModuleFileNameA(nullptr, path, sizeof(path));
			if (char* slash = std::strrchr(path, '\\')) {
				slash[1] = '\0';
				strcat_s(path, "itr-nvse-vats.log");
				s_diagnosticLog = _fsopen(path, "a", _SH_DENYWR);
			}
		}
		if (s_diagnosticLog) {
			std::fprintf(s_diagnosticLog, "\nVATS diagnostic 1: enabled=%u; restart required between settings\n", s_enabled);
			LogCallOwner("original menu update", kAddr_HookSite);
		}
		bool installed = s_vatsMenuUpdateCall.WriteRelCall(kAddr_HookSite, VATSMenuUpdate_Hook);
		if (s_diagnosticLog) {
			std::fprintf(s_diagnosticLog, "installed=%u capturedOriginal=%08X\n", installed, s_vatsMenuUpdateCall.GetOverwrittenAddr());
			std::fflush(s_diagnosticLog);
		}
		Log("VATSProjectileFix: enabled=%u installed=%u diagnostic log=%s", enabled, installed, s_diagnosticLog || !diagnostic ? path : "unavailable");
	}
}
