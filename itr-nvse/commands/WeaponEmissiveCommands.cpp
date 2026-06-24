#include "WeaponEmissiveCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"

extern const _ExtractArgs ExtractArgs;
#include "internal/globals.h"
#include "internal/GameGlobals.h"
#include "internal/NiLayout.h"

namespace
{
	//NiAVObject::GetObjectByName - __cdecl(NiAVObject* root, const char* name) -> NiAVObject*
	typedef void* (__cdecl *_GetObjectByName)(void* root, const char* name);
	static const _GetObjectByName GetObjectByName = (_GetObjectByName)0x4AAE30;

	struct EmissiveOriginal {
		void* node;
		float r, g, b, mult;
	};

	static EmissiveOriginal s_originals[64];
	static UInt32 s_count = 0;
	static bool s_active = false;

	static void* GetPlayer1stPersonNode()
	{
		PlayerCharacter* player = *g_thePlayerPtr;
		if (!player) return nullptr;
		return player->playerNode;
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

	static bool IsNiNode(void* obj)
	{
		//vtable[3] = NiObject::IsNiNode - returns this for NiNode subclasses, nullptr otherwise
		auto fn = (void*(__thiscall*)(void*))(*((UInt32**)obj))[3];
		return fn(obj) != nullptr;
	}

	//traverse and save original values, returns count of geometry nodes found
	static UInt32 TraverseCacheOriginals(void* node)
	{
		if (!node) return 0;

		if (IsGeometry(node))
		{
			auto* matProp = NiGeometryGetMaterialProperty(node);
			if (!matProp) return 0;
			if (s_count < 64)
			{
				auto& orig = s_originals[s_count++];
				orig.node = node;
				orig.r = matProp->emissiveR;
				orig.g = matProp->emissiveG;
				orig.b = matProp->emissiveB;
				orig.mult = matProp->emitMult;
			}
			return 1;
		}

		if (!IsNiNode(node)) return 0;

		void** childData = NiNodeGetChildData(node);
		UInt16 childCount = NiNodeGetChildLimit(node);
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
			auto* matProp = NiGeometryGetMaterialProperty(node);
			if (matProp)
				NiMaterialPropertySetEmissive(matProp, r, g, b, emitMult);
			return;
		}

		if (!IsNiNode(node)) return;

		void** childData = NiNodeGetChildData(node);
		UInt16 childCount = NiNodeGetChildLimit(node);
		if (!childData) return;
		for (UInt16 i = 0; i < childCount; i++)
		{
			if (childData[i])
				TraverseSetEmissive(childData[i], r, g, b, emitMult);
		}
	}

	//verify the live geometry still matches the cache (same nodes, same order) before restoring
	static bool TraverseVerify(void* node, UInt32& idx)
	{
		if (!node) return true;

		if (IsGeometry(node))
		{
			if (idx >= s_count || s_originals[idx].node != node) return false;
			idx++;
			return true;
		}

		if (!IsNiNode(node)) return true;

		void** childData = NiNodeGetChildData(node);
		UInt16 childCount = NiNodeGetChildLimit(node);
		if (!childData) return true;
		for (UInt16 i = 0; i < childCount; i++)
		{
			if (childData[i] && !TraverseVerify(childData[i], idx))
				return false;
		}
		return true;
	}

	//traverse and restore originals by index (matching traversal order)
	static void TraverseRestore(void* node, UInt32& idx)
	{
		if (!node || idx >= s_count) return;

		if (IsGeometry(node))
		{
			auto* matProp = NiGeometryGetMaterialProperty(node);
			if (matProp && idx < s_count)
			{
				auto& orig = s_originals[idx];
				NiMaterialPropertySetEmissive(matProp, orig.r, orig.g, orig.b, orig.mult);
			}
			idx++;
			return;
		}

		if (!IsNiNode(node)) return;

		void** childData = NiNodeGetChildData(node);
		UInt16 childCount = NiNodeGetChildLimit(node);
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

		//only restore if the live model is still the one we cached - a weapon switch
		//rebuilds the geometry, and restoring by index would corrupt the new model
		UInt32 vidx = 0;
		if (TraverseVerify(weaponNode, vidx) && vidx == s_count)
		{
			UInt32 idx = 0;
			TraverseRestore(weaponNode, idx);
		}

		s_active = false;
		s_count = 0;
		*result = 1;

		if (IsConsoleMode())
			Console_Print("ClearWeaponEmissiveColor >> Restored original emissive values");

		return true;
	}
}

namespace WeaponEmissiveCommands {
bool Init(void* nvsePtr) { return true; }

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_SetWeaponEmissiveColor);
	nvse->RegisterCommand(&kCommandInfo_ClearWeaponEmissiveColor);
}

void ClearState()
{
	s_count = 0;
	s_active = false;
}
}
