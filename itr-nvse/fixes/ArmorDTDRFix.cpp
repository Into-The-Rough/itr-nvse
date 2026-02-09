//npc armor DT/DR cache invalidation fix
//vanilla ResetArmorRating doesn't dirty the HighProcess CachedActorValues
//NOT hot-reloadable - requires game restart

#include "ArmorDTDRFix.h"
#include "internal/NVSEMinimal.h"
#include "internal/Detours.h"

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

	static Detours::JumpDetour s_detour;

	void __fastcall Hook_ResetArmorRating(void* character, void* edx) {
		typedef void(__thiscall* ResetArmorRating_t)(void*);
		s_detour.GetTrampoline<ResetArmorRating_t>()(character);

		BaseProcess* process = ((Actor*)character)->baseProcess;
		if (process && process->processLevel == 0) {
			ThisCall(kAddr_DirtyCachedActorValues, process, kAV_DamageThreshold);
			ThisCall(kAddr_DirtyCachedActorValues, process, kAV_DamageResistance);
		}
	}

	//prologue: 7 bytes
	void Init() {
		if (s_detour.WriteRelJump(kAddr_ResetArmorRating, Hook_ResetArmorRating, 7))
			Log("ArmorDTDRFix installed");
	}
}

void ArmorDTDRFix_Init()
{
	ArmorDTDRFix::Init();
}
