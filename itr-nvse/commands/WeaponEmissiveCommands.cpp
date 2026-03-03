#include "WeaponEmissiveCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"

namespace
{
	//NiAVObject::GetObjectByName - __cdecl(NiAVObject* root, const char* name) -> NiAVObject*
	typedef void* (__cdecl *_GetObjectByName)(void* root, const char* name);
	static const _GetObjectByName GetObjectByName = (_GetObjectByName)0x4AAE30;

	struct EmissiveOriginal {
		float r, g, b, mult;
	};

	static EmissiveOriginal s_originals[64];
	static UInt32 s_count = 0;
	static bool s_active = false;

	static void* GetPlayer1stPersonNode()
	{
		void* player = *(void**)0x11DEA3C;
		if (!player) return nullptr;
		return *(void**)((UInt8*)player + 0x694);
	}

	static bool IsGeometry(void* obj)
	{
		if (!obj) return false;
		UInt32 vtbl = *(UInt32*)obj;
		return vtbl == 0x109CD44 //NiTriStrips
			|| vtbl == 0x109D454 //NiTriShape
			|| vtbl == 0x109E834 //BSSegmentedTriShape
			|| vtbl == 0x109E704; //BSResizableTriShape
	}

	static void* GetMaterialProp(UInt8* geom)
	{
		return *(void**)(geom + 0xA4);
	}

	static void SetEmissive(UInt8* matProp, float r, float g, float b, float mult)
	{
		*(float*)(matProp + 0x28) = r;
		*(float*)(matProp + 0x2C) = g;
		*(float*)(matProp + 0x30) = b;
		*(float*)(matProp + 0x40) = mult;
		(*(UInt32*)(matProp + 0x44))++;
	}

	//traverse and save original values, returns count of geometry nodes found
	static UInt32 TraverseCacheOriginals(void* node)
	{
		if (!node) return 0;

		if (IsGeometry(node))
		{
			UInt8* matProp = (UInt8*)GetMaterialProp((UInt8*)node);
			if (!matProp) return 0;
			if (s_count < 64)
			{
				auto& orig = s_originals[s_count++];
				orig.r = *(float*)(matProp + 0x28);
				orig.g = *(float*)(matProp + 0x2C);
				orig.b = *(float*)(matProp + 0x30);
				orig.mult = *(float*)(matProp + 0x40);
			}
			return 1;
		}

		UInt8* n = (UInt8*)node;
		void** childData = *(void***)(n + 0xA0);
		UInt16 childCount = *(UInt16*)(n + 0xA6);
		if (!childData) return 0;
		UInt32 found = 0;
		for (UInt16 i = 0; i < childCount; i++)
		{
			if (childData[i])
				found += TraverseCacheOriginals(childData[i]);
		}
		return found;
	}

	//traverse and set emissive on all geometry
	static void TraverseSetEmissive(void* node, float r, float g, float b, float emitMult)
	{
		if (!node) return;

		if (IsGeometry(node))
		{
			UInt8* matProp = (UInt8*)GetMaterialProp((UInt8*)node);
			if (matProp)
				SetEmissive(matProp, r, g, b, emitMult);
			return;
		}

		UInt8* n = (UInt8*)node;
		void** childData = *(void***)(n + 0xA0);
		UInt16 childCount = *(UInt16*)(n + 0xA6);
		if (!childData) return;
		for (UInt16 i = 0; i < childCount; i++)
		{
			if (childData[i])
				TraverseSetEmissive(childData[i], r, g, b, emitMult);
		}
	}

	//traverse and restore originals by index (matching traversal order)
	static void TraverseRestore(void* node, UInt32& idx)
	{
		if (!node || idx >= s_count) return;

		if (IsGeometry(node))
		{
			UInt8* matProp = (UInt8*)GetMaterialProp((UInt8*)node);
			if (matProp && idx < s_count)
			{
				auto& orig = s_originals[idx];
				SetEmissive(matProp, orig.r, orig.g, orig.b, orig.mult);
			}
			idx++;
			return;
		}

		UInt8* n = (UInt8*)node;
		void** childData = *(void***)(n + 0xA0);
		UInt16 childCount = *(UInt16*)(n + 0xA6);
		if (!childData) return;
		for (UInt16 i = 0; i < childCount; i++)
		{
			if (childData[i])
				TraverseRestore(childData[i], idx);
		}
	}

	static ParamInfo kParams_SetWeaponEmissiveColor[4] = {
		{"fR",        kParamType_Float, 0},
		{"fG",        kParamType_Float, 0},
		{"fB",        kParamType_Float, 0},
		{"fEmitMult", kParamType_Float, 0},
	};

	DEFINE_COMMAND_PLUGIN(SetWeaponEmissiveColor, "Sets emissive color on 1st person weapon geometry", 0, 4, kParams_SetWeaponEmissiveColor);
	DEFINE_COMMAND_PLUGIN(ClearWeaponEmissiveColor, "Restores original emissive color on 1st person weapon", 0, 0, nullptr);

	bool Cmd_SetWeaponEmissiveColor_Execute(COMMAND_ARGS)
	{
		*result = 0;

		float r = 0, g = 0, b = 0, emitMult = 1.0f;
		if (!ExtractArgs(EXTRACT_ARGS, &r, &g, &b, &emitMult))
			return true;

		void* root1st = GetPlayer1stPersonNode();
		if (!root1st) return true;

		void* weaponNode = GetObjectByName(root1st, "Weapon");
		if (!weaponNode) return true;

		//first call: cache originals from live nodes
		if (!s_active)
		{
			s_count = 0;
			TraverseCacheOriginals(weaponNode);
			s_active = true;
		}

		//always traverse fresh - never use cached pointers
		TraverseSetEmissive(weaponNode, r, g, b, emitMult);
		*result = 1;

		if (IsConsoleMode())
			Console_Print("SetWeaponEmissiveColor >> R=%.2f G=%.2f B=%.2f Mult=%.2f (%d geom nodes)", r, g, b, emitMult, s_count);

		return true;
	}

	bool Cmd_ClearWeaponEmissiveColor_Execute(COMMAND_ARGS)
	{
		*result = 0;

		if (!s_active)
			return true;

		void* root1st = GetPlayer1stPersonNode();
		if (!root1st)
		{
			//model gone, nothing to restore
			s_active = false;
			s_count = 0;
			return true;
		}

		void* weaponNode = GetObjectByName(root1st, "Weapon");
		if (!weaponNode)
		{
			s_active = false;
			s_count = 0;
			return true;
		}

		//traverse live nodes, apply cached original values by index
		UInt32 idx = 0;
		TraverseRestore(weaponNode, idx);

		s_active = false;
		s_count = 0;
		*result = 1;

		if (IsConsoleMode())
			Console_Print("ClearWeaponEmissiveColor >> Restored original emissive values");

		return true;
	}
}

bool WeaponEmissiveCommands_Init(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;

	nvse->SetOpcodeBase(0x4050);
	/*4050*/ nvse->RegisterCommand(&kCommandInfo_SetWeaponEmissiveColor);
	/*4051*/ nvse->RegisterCommand(&kCommandInfo_ClearWeaponEmissiveColor);

	Log("Registered SetWeaponEmissiveColor/ClearWeaponEmissiveColor at 0x4050-0x4051");
	return true;
}

void WeaponEmissive_ClearState()
{
	s_count = 0;
	s_active = false;
}
