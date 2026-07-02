#pragma once
//shared form type utilities - used by both production code and tests

#include <cstdint>

namespace FormUtils {

enum FormTypeFilter : uint32_t {
	kFilter_AnyType = 0,
	kFilter_Actor = 200,
	kFilter_InventoryItem = 201,
};

inline bool IsInventoryItemType(uint8_t formType)
{
#ifdef FORMUTILS_TEST_MIRROR_ENGINE
	switch (formType)
	{
		case 0x18: //Armor
		case 0x19: //Book
		case 0x1A: //Clothing
		case 0x1D: //Ingredient
		case 0x1E: //Light
		case 0x1F: //Misc
		case 0x28: //Weapon
		case 0x29: //Ammo
		case 0x2E: //Key
		case 0x2F: //Ingestible
		case 0x31: //Note
		case 0x32: //ConstructibleObject
		case 0x34: //LeveledItem
		case 0x67: //WeaponMods
		case 0x6C: //CasinoChip
		case 0x73: //CaravanCard
		case 0x74: //FactionCurrency
			return true;
		default:
			return false;
	}
#else
	using ContainerCanHoldType_t = bool(__cdecl*)(uint32_t);
	return reinterpret_cast<ContainerCanHoldType_t>(0x481F30)(formType);
#endif
}

template<typename T>
inline float CalcDistanceSquared(T* a, T* b)
{
	float dx = a->posX - b->posX;
	float dy = a->posY - b->posY;
	float dz = a->posZ - b->posZ;
	return dx * dx + dy * dy + dz * dz;
}

}
