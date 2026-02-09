//prevents actors from switching weapons during combat
//hooks CombatProcedureSwitchWeapon::Update and finishes it early if actor is blocked

#include <Windows.h>
#include <cstdint>
#include <cstring>


struct ParamInfo;
struct TESObjectREFR;
struct Script;
struct ScriptEventList;
struct Actor;

#define COMMAND_ARGS ParamInfo* paramInfo, void* scriptData, TESObjectREFR* thisObj, TESObjectREFR* containingObj, Script* scriptObj, ScriptEventList* eventList, double* result, UInt32* opcodeOffsetPtr
#define EXTRACT_ARGS paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList

enum ParamType { kParamType_Integer = 1 };
struct ParamInfo { const char* name; UInt32 type; UInt32 isOptional; };
struct CommandInfo {
	const char* longName;
	const char* shortName;
	UInt32 opcode;
	const char* helpText;
	UInt16 needsParent;
	UInt16 numParams;
	ParamInfo* params;
	void* execute;
	void* parse;
	void* eval;
	UInt32 flags;
};
struct NVSEInterface {
	UInt32 nvseVersion, runtimeVersion, editorVersion, isEditor;
	bool (*RegisterCommand)(CommandInfo* info);
	void (*SetOpcodeBase)(UInt32 opcode);
	void* pad[4];
};

typedef bool (*ExtractArgs_t)(ParamInfo*, void*, UInt32*, TESObjectREFR*, TESObjectREFR*, Script*, ScriptEventList*, ...);
static ExtractArgs_t ExtractArgs = (ExtractArgs_t)0x5ACCB0;

//storage for blocked actors
static const int MAX_BLOCKED = 64;
static UInt32 g_blocked[MAX_BLOCKED] = {0};
static int g_count = 0;
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;

class ScopedLock {
	CRITICAL_SECTION* cs;
public:
	ScopedLock(CRITICAL_SECTION* c) : cs(c) { EnterCriticalSection(cs); }
	~ScopedLock() { LeaveCriticalSection(cs); }
	ScopedLock(const ScopedLock&) = delete;
	ScopedLock& operator=(const ScopedLock&) = delete;
};

static void EnsureLockInit()
{
	if (!g_lockInit)
	{
		InitializeCriticalSection(&g_lock);
		g_lockInit = true;
	}
}

static bool IsBlocked_Unlocked(UInt32 refID)
{
	for (int i = 0; i < g_count; i++)
		if (g_blocked[i] == refID)
			return true;
	return false;
}

static bool IsBlocked(UInt32 refID)
{
	ScopedLock lock(&g_lock);
	return IsBlocked_Unlocked(refID);
}

static void SetBlocked(UInt32 refID, bool block)
{
	ScopedLock lock(&g_lock);
	if (block)
	{
		for (int i = 0; i < g_count; i++)
			if (g_blocked[i] == refID) return;
		if (g_count < MAX_BLOCKED)
			g_blocked[g_count++] = refID;
	}
	else
	{
		for (int i = 0; i < g_count; i++)
		{
			if (g_blocked[i] == refID)
			{
				g_blocked[i] = g_blocked[--g_count];
				g_blocked[g_count] = 0;
				break;
			}
		}
	}
}

//0x9DA7C0 = CombatProcedureSwitchWeapon::Update
typedef void (__thiscall *SwitchWeaponUpdate_t)(void* procedure);
static SwitchWeaponUpdate_t Original = 0;

//0x97AE90 = CombatController::GetPackageOwner
typedef Actor* (__thiscall *GetPackageOwner_t)(void* controller);
static GetPackageOwner_t GetPackageOwner = (GetPackageOwner_t)0x97AE90;

void __fastcall Hook_SwitchWeaponUpdate(void* procedure, void* edx)
{
	bool shouldBlock = false;
	{
		ScopedLock lock(&g_lock);
		if (g_count > 0)
		{
			void* controller = *(void**)((char*)procedure + 0x4);
			if (controller)
			{
				Actor* actor = GetPackageOwner(controller);
				if (actor)
				{
					UInt32 refID = *(UInt32*)((char*)actor + 0x0C);
					if (IsBlocked_Unlocked(refID))
						shouldBlock = true;
				}
			}
		}
	}

	if (shouldBlock)
	{
		//set eStatus = 2 (finished) at offset 0x8
		*(UInt32*)((char*)procedure + 0x8) = 2;
		return;
	}
	Original(procedure);
}

//trampoline: save original bytes and jump
//first 6 bytes: push ebp (1) + mov ebp,esp (2) + sub esp,0x24 (3)
static UInt8 g_originalBytes[6];
static UInt8 g_trampoline[16];

static void WriteJmp(UInt32 src, UInt32 dst)
{
	DWORD oldProtect;
	VirtualProtect((void*)src, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
	*(UInt8*)src = 0xE9;
	*(UInt32*)(src + 1) = dst - src - 5;
	*(UInt8*)(src + 5) = 0x90; //nop the 6th byte
	VirtualProtect((void*)src, 6, oldProtect, &oldProtect);
}

void PreventWeaponSwitch_Init()
{
	EnsureLockInit();
	const UInt32 kAddr = 0x9DA7C0;

	//save original 6 bytes
	memcpy(g_originalBytes, (void*)kAddr, 6);

	//build trampoline: original bytes + jmp back
	DWORD oldProtect;
	VirtualProtect(g_trampoline, sizeof(g_trampoline), PAGE_EXECUTE_READWRITE, &oldProtect);
	memcpy(g_trampoline, g_originalBytes, 6);
	g_trampoline[6] = 0xE9; //jmp
	*(UInt32*)(g_trampoline + 7) = (kAddr + 6) - (UInt32)(g_trampoline + 6) - 5;

	Original = (SwitchWeaponUpdate_t)(void*)g_trampoline;

	//install hook
	WriteJmp(kAddr, (UInt32)Hook_SwitchWeaponUpdate);
}

//IsActor virtual at vtable index 0x100
inline bool IsActorRef(TESObjectREFR* ref) {
	if (!ref) return false;
	UInt32 vtable = *(UInt32*)ref;
	UInt32 isActorFn = *(UInt32*)(vtable + 0x100);
	return ((bool (__thiscall*)(TESObjectREFR*))isActorFn)(ref);
}

static bool Cmd_SetPreventWeaponSwitch_Execute(COMMAND_ARGS)
{
	*result = 0;
	UInt32 block = 0;
	if (!ExtractArgs(EXTRACT_ARGS, &block))
		return true;

	if (IsActorRef(thisObj))
	{
		UInt32 refID = *(UInt32*)((char*)thisObj + 0x0C);
		SetBlocked(refID, block != 0);
		*result = 1;
	}
	return true;
}

static bool Cmd_GetPreventWeaponSwitch_Execute(COMMAND_ARGS)
{
	*result = 0;
	if (IsActorRef(thisObj))
	{
		UInt32 refID = *(UInt32*)((char*)thisObj + 0x0C);
		*result = IsBlocked(refID) ? 1 : 0;
	}
	return true;
}

static ParamInfo kParams_SetPreventWeaponSwitch[1] = {
	{"block", kParamType_Integer, 0}
};

static CommandInfo kCommandInfo_SetPreventWeaponSwitch = {
	"SetPreventWeaponSwitch", "", 0, "Prevent actor from switching weapons",
	1, 1, kParams_SetPreventWeaponSwitch, (void*)Cmd_SetPreventWeaponSwitch_Execute, 0, 0, 0
};

static CommandInfo kCommandInfo_GetPreventWeaponSwitch = {
	"GetPreventWeaponSwitch", "", 0, "Check if actor weapon switching is prevented",
	1, 0, 0, (void*)Cmd_GetPreventWeaponSwitch_Execute, 0, 0, 0
};

void PreventWeaponSwitch_RegisterCommands(const void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	nvseIntf->RegisterCommand(&kCommandInfo_SetPreventWeaponSwitch);
	nvseIntf->RegisterCommand(&kCommandInfo_GetPreventWeaponSwitch);
}
