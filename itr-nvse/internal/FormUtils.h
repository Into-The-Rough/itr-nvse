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
	return formType == 0x18 ||  //Armor
	       formType == 0x19 ||  //Book
	       formType == 0x1A ||  //Clothing
	       formType == 0x1D ||  //Ingredient
	       formType == 0x1F ||  //Misc
	       formType == 0x28 ||  //Weapon
	       formType == 0x29 ||  //Ammo
	       formType == 0x2E ||  //Key
	       formType == 0x2F ||  //AlchemyItem
	       formType == 0x31;    //Note
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
