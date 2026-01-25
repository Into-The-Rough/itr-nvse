#pragma once
//shared form type utilities - used by both production code and tests

#include <cstdint>

namespace FormUtils {

//form type constants - only used when NVSE headers not available (tests)
#ifndef FORMUTILS_USE_NVSE_TYPES
enum FormType : uint8_t {
	kFormType_Armor = 0x19,
	kFormType_Book = 0x1A,
	kFormType_Clothing = 0x1B,
	kFormType_Ingredient = 0x1D,
	kFormType_Misc = 0x1F,
	kFormType_Weapon = 0x28,
	kFormType_Ammo = 0x29,
	kFormType_Key = 0x2D,
	kFormType_AlchemyItem = 0x2E,
	kFormType_Note = 0x32,
	kFormType_Creature = 0x30,
	kFormType_NPC = 0x36,
};
#endif

enum FormTypeFilter : uint32_t {
	kFilter_AnyType = 0,
	kFilter_Actor = 200,
	kFilter_InventoryItem = 201,
};

inline bool IsInventoryItemType(uint8_t formType)
{
	return formType == 0x19 ||  //Armor
	       formType == 0x1A ||  //Book
	       formType == 0x1B ||  //Clothing
	       formType == 0x1D ||  //Ingredient
	       formType == 0x1F ||  //Misc
	       formType == 0x28 ||  //Weapon
	       formType == 0x29 ||  //Ammo
	       formType == 0x2D ||  //Key
	       formType == 0x2E ||  //AlchemyItem
	       formType == 0x32;    //Note
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
