#include "EyeMeshOverride.h"

#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include "nvse/GameForms.h"

#include "internal/Detours.h"
#include "internal/ScopedLock.h"

#include <string>
#include <unordered_map>

extern const _ExtractArgs ExtractArgs;

namespace
{
	//FaceGenNpcData fields read in BSFaceGenManager::AttachEyesToHead (0x6558B0)
	//eye form supplies only the texture; left/right meshes come from the race
	constexpr UInt32 kOffset_EyeForm = 0x8C;
	constexpr UInt32 kOffset_LeftEyeModel = 0xD8;
	constexpr UInt32 kOffset_RightEyeModel = 0xDC;

	//single call site, inside BSFaceGenManager::CreateFaceGenHead
	constexpr UInt32 kAddr_AttachEyesCall = 0x655032;

	struct EyeEntry
	{
		std::string left;
		std::string right;
	};

	std::unordered_map<UInt32, EyeEntry> g_registry;
	CRITICAL_SECTION g_lock;
	volatile LONG g_lockInit = 0;

	using AttachEyes_t = void(__cdecl*)(void*, void*, void*, char);
	Detours::CallDetour g_detour;
	AttachEyes_t g_origAttachEyes = reinterpret_cast<AttachEyes_t>(0x6558B0);
	bool g_hookInstalled = false;

	//fake eye-model whose vtable slot 5 (the model-path getter at +0x14) hands
	//back our registered path. AttachEyesToHead touches the model object only
	//through that one call, so a stub vtable is enough.
	struct FakeEyeModel
	{
		void** vtbl;
		const char* path;
	};

	const char* __fastcall FakeEyeModel_GetPath(FakeEyeModel* self, void*)
	{
		return self->path;
	}

	void* s_fakeVtbl[6] = { nullptr, nullptr, nullptr, nullptr, nullptr,
	                        reinterpret_cast<void*>(&FakeEyeModel_GetPath) };

	std::string NormalizeMeshPath(const char* raw)
	{
		if (!raw) return std::string();
		while (*raw == '\\' || *raw == '/') ++raw;
		//paths are formatted as "Meshes\%s", strip a leading meshes\ if present
		if (_strnicmp(raw, "meshes\\", 7) == 0) raw += 7;
		else if (_strnicmp(raw, "meshes/", 7) == 0) raw += 7;
		return std::string(raw);
	}

	void __cdecl Hook_AttachEyesToHead(void* a1, void* a2, void* npcData, char a4)
	{
		TESForm* eyeForm = *reinterpret_cast<TESForm**>(static_cast<UInt8*>(npcData) + kOffset_EyeForm);

		std::string left, right;
		bool have = false;
		if (eyeForm)
		{
			ScopedLock lock(&g_lock);
			auto it = g_registry.find(eyeForm->refID);
			if (it != g_registry.end())
			{
				left = it->second.left;
				right = it->second.right;
				have = true;
			}
		}

		if (!have)
		{
			g_origAttachEyes(a1, a2, npcData, a4);
			return;
		}

		//restore the race's eye-model pointers no matter how the engine call
		//returns - the fakes live on this stack frame and must not outlive it
		struct ModelSwap
		{
			void** slot;
			void* saved;
			ModelSwap(void** s, void* fake) : slot(s), saved(*s) { *s = fake; }
			~ModelSwap() { *slot = saved; }
		};

		void** pLeft = reinterpret_cast<void**>(static_cast<UInt8*>(npcData) + kOffset_LeftEyeModel);
		void** pRight = reinterpret_cast<void**>(static_cast<UInt8*>(npcData) + kOffset_RightEyeModel);
		FakeEyeModel fakeLeft{ s_fakeVtbl, left.c_str() };
		FakeEyeModel fakeRight{ s_fakeVtbl, right.c_str() };

		ModelSwap swapL(pLeft, left.empty() ? *pLeft : &fakeLeft);
		ModelSwap swapR(pRight, right.empty() ? *pRight : &fakeRight);
		g_origAttachEyes(a1, a2, npcData, a4);
	}

	void EnsureHook()
	{
		if (g_hookInstalled) return;
		if (g_detour.WriteRelCall(kAddr_AttachEyesCall, Hook_AttachEyesToHead))
		{
			g_origAttachEyes = reinterpret_cast<AttachEyes_t>(g_detour.GetOverwrittenAddr());
			g_hookInstalled = true;
		}
	}

	TESForm* ExtractEyesForm(TESForm* form)
	{
		if (!form || form->typeID != kFormType_Eyes) return nullptr;
		return form;
	}
}

bool Cmd_SetEyeMesh_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;
	char leftPath[512] = { 0 };
	char rightPath[512] = { 0 };
	if (!ExtractArgs(EXTRACT_ARGS, &form, &leftPath, &rightPath))
		return true;

	TESForm* eyes = ExtractEyesForm(form);
	if (!eyes)
	{
		if (IsConsoleMode())
			Console_Print("SetEyeMesh >> form is not an eyes record");
		return true;
	}

	std::string left = NormalizeMeshPath(leftPath);
	std::string right = rightPath[0] ? NormalizeMeshPath(rightPath) : left;
	if (left.empty() && right.empty())
		return true;

	InitCriticalSectionOnce(&g_lockInit, &g_lock);
	{
		ScopedLock lock(&g_lock);
		g_registry[eyes->refID] = EyeEntry{ left, right };
	}
	EnsureHook();

	*result = 1;
	return true;
}

bool Cmd_ClearEyeMesh_Execute(COMMAND_ARGS)
{
	*result = 0;
	TESForm* form = nullptr;
	if (!ExtractArgs(EXTRACT_ARGS, &form))
		return true;

	TESForm* eyes = ExtractEyesForm(form);
	if (!eyes) return true;

	InitCriticalSectionOnce(&g_lockInit, &g_lock);
	{
		ScopedLock lock(&g_lock);
		g_registry.erase(eyes->refID);
	}

	*result = 1;
	return true;
}

static ParamInfo kParams_SetEyeMesh[3] = {
	{"eyes", kParamType_AnyForm, 0},
	{"leftMesh", kParamType_String, 0},
	{"rightMesh", kParamType_String, 1},
};

static ParamInfo kParams_ClearEyeMesh[1] = {
	{"eyes", kParamType_AnyForm, 0},
};

DEFINE_COMMAND_PLUGIN(SetEyeMesh, "Register a per-eyes-form mesh override (path relative to Meshes\\); rightMesh optional, defaults to leftMesh", 0, 3, kParams_SetEyeMesh);
DEFINE_COMMAND_PLUGIN(ClearEyeMesh, "Remove a per-eyes-form mesh override", 0, 1, kParams_ClearEyeMesh);

namespace EyeMeshOverride
{
	void RegisterCommands(void* nvsePtr)
	{
		NVSEInterface* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_SetEyeMesh);
		nvse->RegisterCommand(&kCommandInfo_ClearEyeMesh);
	}
}
