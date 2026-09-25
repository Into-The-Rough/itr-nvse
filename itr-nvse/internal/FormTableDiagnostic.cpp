//logs form-table entries a save load can trip on: temporary forms still registered, entries keyed away from their refID, freed forms left behind

#include "FormTableDiagnostic.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <unordered_map>
#include <vector>
#include "nvse/GameObjects.h"
#include "nvse/NiTypes.h"
#include "internal/Detours.h"
#include "internal/CallTemplates.h"
#include "internal/globals.h"

namespace FormTableDiagnostic
{
	constexpr UInt32 kAddr_AllFormsMap = 0x11C54C0;
	constexpr UInt32 kAddr_MapSetAt = 0x844700;
	constexpr UInt32 kAddr_Unlink = 0x483C70;
	constexpr UInt32 kAddr_CtorInsertCall = 0x4835BE;		//TESForm::TESForm
	constexpr UInt32 kAddr_SetRefIDInsertCall = 0x485D24;	//TESForm::SetRefID
	constexpr UInt32 kAddr_MarkTempUnlinkCall = 0x48449A;	//TESForm::MarkAsTemporary
	constexpr UInt32 kAddr_DtorUnlinkCall = 0x483669;		//TESForm::~TESForm
	constexpr UInt32 kFormFlag_Temporary = 0x4000;
	constexpr UInt32 kFrames = 10;
	constexpr UInt32 kCaptureDepth = 24;
	constexpr UInt32 kMaxWalkReports = 200;
	constexpr UInt32 kMaxDangling = 256;

	using FormMap = NiTPointerMap<TESForm>;
	using Frames = UInt32[kFrames];

	struct Origin
	{
		UInt32 key = 0;
		UInt32 thread = 0;
		bool viaSetRefID = false;
		Frames created = {};
		Frames temped = {};
	};

	struct Dangling
	{
		const TESForm* form;
		UInt32 key;
		Origin origin;
		Frames destroyed;
	};

	static CRITICAL_SECTION s_lock;
	static CRITICAL_SECTION s_logLock;
	typedef USHORT(WINAPI* CaptureStackBackTrace_t)(ULONG, ULONG, PVOID*, PULONG);
	static CaptureStackBackTrace_t s_captureStackBackTrace = nullptr;
	static std::unordered_map<const TESForm*, Origin> s_origins;
	static std::vector<Dangling> s_dangling;
	static FILE* s_log = nullptr;
	static UInt32 s_selfBase = 0, s_selfEnd = 0;
	static UInt32 s_exeBase = 0, s_exeEnd = 0;
	static Detours::CallDetour s_ctorInsert;
	static Detours::CallDetour s_setRefIDInsert;
	static Detours::CallDetour s_markTempUnlink;
	static Detours::CallDetour s_dtorUnlink;

	static void ModuleRange(HMODULE module, UInt32& base, UInt32& end)
	{
		base = (UInt32)module;
		auto* dos = (IMAGE_DOS_HEADER*)module;
		auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
		end = base + nt->OptionalHeader.SizeOfImage;
	}

	__declspec(noinline) static void Capture(Frames& out)
	{
		void* raw[kCaptureDepth] = {};
		const USHORT count = s_captureStackBackTrace ? s_captureStackBackTrace(0, kCaptureDepth, raw, nullptr) : 0;
		UInt32 kept = 0;
		for (USHORT i = 0; i < count && kept < kFrames; i++) {
			const UInt32 addr = (UInt32)raw[i];
			if (addr >= s_selfBase && addr < s_selfEnd) continue;
			out[kept++] = addr;
		}
		for (; kept < kFrames; kept++) out[kept] = 0;
	}

	static FormMap* Map()
	{
		return *(FormMap**)kAddr_AllFormsMap;
	}

	static TESForm* Lookup(FormMap* map, UInt32 key)
	{
		if (!map || !map->m_numBuckets || !map->m_buckets) return nullptr;
		for (auto* entry = map->m_buckets[key % map->m_numBuckets]; entry; entry = entry->next)
			if (entry->key == key) return entry->data;
		return nullptr;
	}

	static bool FindOrigin(const TESForm* form, Origin& out)
	{
		EnterCriticalSection(&s_lock);
		auto it = s_origins.find(form);
		const bool found = it != s_origins.end();
		if (found) out = it->second;
		LeaveCriticalSection(&s_lock);
		return found;
	}

	static void Describe(UInt32 addr, char* out, size_t size)
	{
		HMODULE module = nullptr;
		char path[MAX_PATH] = {};
		if (addr && !(addr >= s_exeBase && addr < s_exeEnd)
			&& GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &module)
			&& GetModuleFileNameA(module, path, MAX_PATH)) {
			const char* name = std::strrchr(path, '\\');
			sprintf_s(out, size, "%s+%X", name ? name + 1 : path, addr - (UInt32)module);
			return;
		}
		sprintf_s(out, size, "%08X", addr);
	}

	static void WriteFrames(const char* tag, const Frames& frames)
	{
		if (!frames[0]) return;
		std::fprintf(s_log, "    %s:", tag);
		for (UInt32 frame : frames) {
			if (!frame) break;
			char text[96];
			Describe(frame, text, sizeof(text));
			std::fprintf(s_log, " %s", text);
		}
		std::fprintf(s_log, "\n");
	}

	static void WriteForm(const TESForm* form, UInt32 key, bool readable, bool deep)
	{
		const UInt32 vtbl = *(const UInt32*)form;
		std::fprintf(s_log, "    key %08X form %p vtbl %08X", key, form, vtbl);
		if (!readable) {
			std::fprintf(s_log, " (vtbl outside FalloutNV, freed or foreign)\n");
			return;
		}
		std::fprintf(s_log, " refID %08X type %02X flags %08X", form->refID, form->typeID, form->flags);
		if (form->typeID >= kFormType_Reference && form->typeID <= kFormType_ACRE) {
			auto* ref = (const TESObjectREFR*)form;
			if (deep)
				std::fprintf(s_log, " base %08X cell %08X", ref->baseForm ? ref->baseForm->refID : 0,
					ref->parentCell ? ref->parentCell->refID : 0);
			else
				std::fprintf(s_log, " base %p cell %p", ref->baseForm, ref->parentCell);
			std::fprintf(s_log, " pos %.0f/%.0f/%.0f", ref->posX, ref->posY, ref->posZ);
		}
		std::fprintf(s_log, "\n");
	}

	static void WriteOrigin(const Origin& origin)
	{
		std::fprintf(s_log, "    registered as %08X via %s on thread %u\n", origin.key,
			origin.viaSetRefID ? "SetRefID" : "constructor", origin.thread);
		WriteFrames("created", origin.created);
		WriteFrames("marked temporary", origin.temped);
	}

	static void Report(const char* what, const TESForm* form, UInt32 key, const Frames& now)
	{
		if (!s_log) return;
		Origin origin;
		const bool known = FindOrigin(form, origin);
		EnterCriticalSection(&s_logLock);
		std::fprintf(s_log, "%s (thread %u)\n", what, GetCurrentThreadId());
		WriteForm(form, key, true, false);
		if (known) WriteOrigin(origin);
		WriteFrames("now", now);
		std::fflush(s_log);
		LeaveCriticalSection(&s_logLock);
	}

	static void RecordInsert(UInt32 key, TESForm* form, bool viaSetRefID)
	{
		if (!form || (key >> 24) != 0xFF) return;
		Frames frames;
		Capture(frames);
		EnterCriticalSection(&s_lock);
		auto it = s_origins.find(form);
		if (viaSetRefID && it != s_origins.end()) {
			it->second.key = key;
			it->second.viaSetRefID = true;
		} else {
			Origin& origin = s_origins[form];
			origin = Origin();
			origin.key = key;
			origin.thread = GetCurrentThreadId();
			origin.viaSetRefID = viaSetRefID;
			std::memcpy(origin.created, frames, sizeof(frames));
		}
		LeaveCriticalSection(&s_lock);
	}

	static void* __fastcall CtorInsert_Hook(FormMap* map, void*, UInt32 key, TESForm* form)
	{
		void* result = ThisCall<void*>(s_ctorInsert.GetOverwrittenAddr(), map, key, form);
		RecordInsert(key, form, false);
		return result;
	}

	static void* __fastcall SetRefIDInsert_Hook(FormMap* map, void*, UInt32 key, TESForm* form)
	{
		void* result = ThisCall<void*>(s_setRefIDInsert.GetOverwrittenAddr(), map, key, form);
		RecordInsert(key, form, true);
		return result;
	}

	static UInt32 __fastcall MarkTempUnlink_Hook(TESForm* form, void*)
	{
		Frames frames;
		Capture(frames);
		UInt32 recordedKey = 0;
		EnterCriticalSection(&s_lock);
		auto it = s_origins.find(form);
		if (it != s_origins.end()) {
			std::memcpy(it->second.temped, frames, sizeof(frames));
			recordedKey = it->second.key;
		}
		LeaveCriticalSection(&s_lock);

		FormMap* map = Map();
		TESForm* underRefID = Lookup(map, form->refID);
		if (form->flags & kFormFlag_Temporary) {
			if (underRefID == form)
				Report("MARKTEMP: already temporary but still registered, the unlink is skipped", form, form->refID, frames);
		} else if (underRefID && underRefID != form) {
			Report("MARKTEMP: refID key holds a different form, the unlink removes that entry", form, form->refID, frames);
		} else if (recordedKey && recordedKey != form->refID && Lookup(map, recordedKey) == form) {
			Report("MARKTEMP: registered under an older key, that entry survives", form, recordedKey, frames);
		}
		return ThisCall<UInt32>(s_markTempUnlink.GetOverwrittenAddr(), form);
	}

	static UInt32 __fastcall DtorUnlink_Hook(TESForm* form, void*)
	{
		Origin origin;
		const bool known = FindOrigin(form, origin);
		const bool temporary = (form->flags & kFormFlag_Temporary) != 0;
		if (temporary || (known && origin.key != form->refID)) {
			FormMap* map = Map();
			UInt32 key = 0;
			if (temporary && form->refID && Lookup(map, form->refID) == form)
				key = form->refID;
			else if (known && origin.key != form->refID && Lookup(map, origin.key) == form)
				key = origin.key;
			if (key) {
				Dangling dangling = { form, key, origin, {} };
				Capture(dangling.destroyed);
				Report(temporary ? "DANGLE: temporary form destroyed while registered, the entry now points at freed memory"
					: "DANGLE: form destroyed while registered under an older key", form, key, dangling.destroyed);
				EnterCriticalSection(&s_lock);
				if (s_dangling.size() < kMaxDangling) s_dangling.push_back(dangling);
				LeaveCriticalSection(&s_lock);
			}
		}
		const UInt32 result = ThisCall<UInt32>(s_dtorUnlink.GetOverwrittenAddr(), form);
		if (known) {
			EnterCriticalSection(&s_lock);
			s_origins.erase(form);
			LeaveCriticalSection(&s_lock);
		}
		return result;
	}

	static const Dangling* FindDangling(const TESForm* form, UInt32 key)
	{
		for (const Dangling& dangling : s_dangling)
			if (dangling.form == form && dangling.key == key) return &dangling;
		return nullptr;
	}

	void Walk(const char* label)
	{
		FormMap* map = Map();
		if (!s_log || !map || !map->m_buckets) return;
		UInt32 total = 0, created = 0, freed = 0, temporary = 0, mismatched = 0, baseless = 0, reported = 0;
		EnterCriticalSection(&s_lock);
		EnterCriticalSection(&s_logLock);
		std::fprintf(s_log, "walk %s: %u buckets\n", label, map->m_numBuckets);
		for (UInt32 bucket = 0; bucket < map->m_numBuckets; bucket++) {
			for (auto* entry = map->m_buckets[bucket]; entry; entry = entry->next) {
				total++;
				const TESForm* form = entry->data;
				const UInt32 key = entry->key;
				if ((key >> 24) == 0xFF) created++;
				if (!form) continue;
				const UInt32 vtbl = *(const UInt32*)form;
				const bool readable = vtbl >= s_exeBase && vtbl < s_exeEnd;
				const char* what = nullptr;
				//a TEMP or FREED entry may be a destroyed form, its base and cell pointers can be stale
				bool live = false;
				if (!readable) { freed++; what = "FREED"; }
				else if (form->flags & kFormFlag_Temporary) { temporary++; what = "TEMP"; }
				else if (form->refID != key) { mismatched++; what = "KEY"; live = true; }
				else if (form->typeID >= kFormType_Reference && form->typeID <= kFormType_ACRE
					&& !((const TESObjectREFR*)form)->baseForm) { baseless++; what = "NOBASE"; live = true; }
				if (!what || reported >= kMaxWalkReports) continue;
				reported++;
				std::fprintf(s_log, "  %s\n", what);
				WriteForm(form, key, readable, live);
				auto it = s_origins.find(form);
				if (it != s_origins.end()) WriteOrigin(it->second);
				if (const Dangling* dangling = FindDangling(form, key)) {
					std::fprintf(s_log, "    matches a DANGLE logged earlier\n");
					WriteOrigin(dangling->origin);
					WriteFrames("destroyed", dangling->destroyed);
				}
			}
		}
		std::fprintf(s_log, "walk %s: %u entries, %u FF, freed %u, temp %u, key mismatch %u, ref without base %u, tracked %u\n",
			label, total, created, freed, temporary, mismatched, baseless, (UInt32)s_origins.size());
		std::fflush(s_log);
		LeaveCriticalSection(&s_logLock);
		LeaveCriticalSection(&s_lock);
	}

	static bool Install(Detours::CallDetour& detour, UInt32 site, UInt32 expected, UInt32 hook, const char* name)
	{
		const bool installed = detour.WriteRelCall(site, hook);
		std::fprintf(s_log, "hook %s at %08X: installed=%u target %08X%s\n", name, site, installed,
			detour.GetOverwrittenAddr(), installed && detour.GetOverwrittenAddr() != expected ? " (not vanilla, chained)" : "");
		return installed;
	}

	void Init()
	{
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Init, &self);
		ModuleRange(self, s_selfBase, s_selfEnd);
		ModuleRange(GetModuleHandleA(nullptr), s_exeBase, s_exeEnd);
		InitializeCriticalSection(&s_lock);
		InitializeCriticalSection(&s_logLock);
		s_captureStackBackTrace = (CaptureStackBackTrace_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "RtlCaptureStackBackTrace");

		char path[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, path, sizeof(path));
		if (char* slash = std::strrchr(path, '\\')) {
			slash[1] = '\0';
			strcat_s(path, "itr-nvse-formtable.log");
			s_log = _fsopen(path, "a", _SH_DENYWR);
		}
		if (!s_log) {
			Log("FormTableDiagnostic: log unavailable, not installed");
			return;
		}
		SYSTEMTIME now;
		GetLocalTime(&now);
		std::fprintf(s_log, "\nsession %04u-%02u-%02u %02u:%02u:%02u\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
		bool ok = Install(s_ctorInsert, kAddr_CtorInsertCall, kAddr_MapSetAt, (UInt32)CtorInsert_Hook, "ctor insert");
		ok &= Install(s_setRefIDInsert, kAddr_SetRefIDInsertCall, kAddr_MapSetAt, (UInt32)SetRefIDInsert_Hook, "SetRefID insert");
		ok &= Install(s_markTempUnlink, kAddr_MarkTempUnlinkCall, kAddr_Unlink, (UInt32)MarkTempUnlink_Hook, "MarkAsTemporary unlink");
		ok &= Install(s_dtorUnlink, kAddr_DtorUnlinkCall, kAddr_Unlink, (UInt32)DtorUnlink_Hook, "dtor unlink");
		std::fflush(s_log);
		Log("FormTableDiagnostic: hooks %s, log %s", ok ? "installed" : "PARTIAL", path);
	}
}
