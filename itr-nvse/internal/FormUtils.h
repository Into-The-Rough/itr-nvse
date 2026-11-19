#pragma once
//shared form type utilities - used by both production code and tests

namespace FormUtils {

	enum FormTypeFilter : UInt32 {
		kFilter_AnyType = 0,
		kFilter_Actor = 200,
		kFilter_InventoryItem = 201,
	};

	inline bool IsInventoryItemType(UInt8 formType)
	{
		using ContainerCanHoldType_t = bool(__cdecl*)(UInt32);
		return reinterpret_cast<ContainerCanHoldType_t>(0x481F30)(formType);
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
