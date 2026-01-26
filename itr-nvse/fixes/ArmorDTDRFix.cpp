//npc armor DT/DR cache invalidation fix
//vanilla ResetArmorRating doesn't dirty the HighProcess CachedActorValues

#include "ArmorDTDRFix.h"
#include <Windows.h>
#include <cstdint>

extern void Log(const char* fmt, ...);

namespace ArmorDTDRFix
{
	//Character::ResetArmorRating: 9260672 dec
	//HighProcess::DirtyCachedActorValues: 9439104 dec
	constexpr uint32_t kAddr_ResetArmorRating = 0x8D4E80;
	constexpr uint32_t kAddr_DirtyCachedActorValues = 0x900780;
	constexpr uint32_t kAV_DamageResistance = 18;
	constexpr uint32_t kAV_DamageThreshold = 76;

	struct BaseProcess {
		char pad[0x28];
		uint32_t processLevel; //0=High, 1=MiddleHigh, 2=MiddleLow, 3=Low
	};

	struct Actor {
		char pad[0x68];
		BaseProcess* baseProcess;
	};

	template <typename T_Ret = void, typename... Args>
	__forceinline T_Ret ThisCall(uint32_t addr, void* _this, Args... args) {
		return ((T_Ret(__thiscall*)(void*, Args...))addr)(_this, args...);
	}

	static uint32_t g_trampolineResetArmor = 0;

	void __fastcall Hook_ResetArmorRating(void* character, void* edx) {
		ThisCall(g_trampolineResetArmor, character);

		BaseProcess* process = ((Actor*)character)->baseProcess;
		if (process && process->processLevel == 0) {
			ThisCall(kAddr_DirtyCachedActorValues, process, kAV_DamageThreshold);
			ThisCall(kAddr_DirtyCachedActorValues, process, kAV_DamageResistance);
		}
	}

	void* CreateTrampoline(uint32_t funcAddr, uint32_t prologBytes) {
		void* trampoline = VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!trampoline) return nullptr;

		memcpy(trampoline, (void*)funcAddr, prologBytes);
		uint8_t* p = (uint8_t*)trampoline + prologBytes;
		*p++ = 0xE9;
		*(uint32_t*)p = (funcAddr + prologBytes) - ((uint32_t)trampoline + prologBytes) - 5;
		return trampoline;
	}

	void WriteRelJump(uint32_t src, uint32_t dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(uint8_t*)src = 0xE9;
		*(uint32_t*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void Init() {
		g_trampolineResetArmor = (uint32_t)CreateTrampoline(kAddr_ResetArmorRating, 7);
		if (g_trampolineResetArmor)
			WriteRelJump(kAddr_ResetArmorRating, (uint32_t)Hook_ResetArmorRating);
		Log("ArmorDTDRFix installed");
	}
}

void ArmorDTDRFix_Init()
{
	ArmorDTDRFix::Init();
}
