//typed engine function pointers - centralized to catch address errors once
#pragma once

class Actor;
class TESObjectREFR;
class TESBoundObject;
class TESForm;

namespace Engine {

//detection — Actor::GetDetectionValue returns -100 if baseProcess null or DetectionData null
inline auto Actor_GetDetectionValue = (SInt32(__thiscall*)(Actor*, Actor*))0x8A8230;

//crime — engine builds a Crime per-alarm and adds witnesses to its kWitnesses list
struct Crime {
	UInt32          uiWitnessCount;    // +0x00
	UInt32          eCrimeType;        // +0x04 — see CrimeType enum below
	TESObjectREFR*  pCrimeTarget;      // +0x08
	Actor*          pCriminal;         // +0x0C
	UInt8           bReported;         // +0x10
	UInt8           pad11[3];
	TESBoundObject* pStolenObject;     // +0x14
	UInt32          uiNumberStolen;    // +0x18
	UInt32          kWitnessesHead;    // +0x1C — BSSimpleList<Actor*>.head
	UInt32          kWitnessesCount;   // +0x20
	TESForm*        pOwnership;        // +0x24
	UInt32          uiCrimeNumber;     // +0x28
	UInt32          dword2C;           // +0x2C
	UInt32          uiCrimeStamp;      // +0x30
	UInt32          kCrimeTimeStamp;   // +0x34
	UInt32          dword38;           // +0x38
};
static_assert(sizeof(Crime) == 0x3C, "Crime must be 60 bytes");

//crime type values read from actual engine call sites (StealAlarm/PickpocketAlarm/AttackAlarm/MurderAlarm);
//CRIME_TRESPASS is synthetic — Actor::TrespassAlarm bypasses the Crime object entirely
enum CrimeType : UInt32 {
	kCrimeType_Steal      = 0,
	kCrimeType_Pickpocket = 1,
	kCrimeType_Trespass   = 2,
	kCrimeType_Attack     = 3,
	kCrimeType_Murder     = 4,
};

//detection data — layout reverse-engineered from DetectionData::CopyFrom (0x8D6FC0)
//not a named type in NVSE headers; kept internal so future header additions don't collide
struct DetectionData {
	Actor*   actor;             // +0x00
	UInt32   detectionState;    // +0x04 (packed flags)
	SInt32   detectionValue;    // +0x08 — the threshold value (-100..500)
	float    locX;              // +0x0C
	float    locY;              // +0x10
	float    locZ;              // +0x14
	float    fTimestamp;        // +0x18 — -1.0f sentinel = location invalid
	UInt8    bForceResetLOS;    // +0x1C
	UInt8    byte1D;            // +0x1D
	UInt8    inLOS;             // +0x1E
	UInt8    byte1F;            // +0x1F
	SInt32   detectionModSneak; // +0x20
};
static_assert(sizeof(DetectionData) == 0x24, "DetectionData must be 36 bytes");

//tile manipulation
inline auto Tile_SetFloat = (void(__thiscall*)(void*, int, float, bool))0xA012D0;
inline auto Tile_SetString = (void(__thiscall*)(void*, int, const char*, bool))0xA01350;
inline auto Tile_GetValue = (void*(__thiscall*)(void*, int))0xA01000;
inline auto Tile_TextToTrait = (int(__cdecl*)(const char*))0xA01860;

//sound handles
inline auto BSSoundHandle_Play = (bool(__thiscall*)(void*, bool))0xAD8830;
inline auto BSSoundHandle_Stop = (void(__thiscall*)(void*))0xAD88F0;
inline auto BSSoundHandle_IsPlaying = (bool(__thiscall*)(void*))0xAD8930;
inline auto BSSoundHandle_SetVolume = (void(__thiscall*)(void*, float))0xAD89E0;

//input
inline auto OSInputGlobals_GetControlState = (bool(__thiscall*)(void*, UInt32, UInt8))0xA24660;

//combat
inline auto CombatController_GetPackageOwner = (void*(__thiscall*)(void*))0x97AE90;

//actor
inline auto Actor_GetProcess = (void*(__thiscall*)(void*))0x8D8520;
inline auto Actor_TryDropWeapon = (void(__thiscall*)(void*))0x89F580;
inline auto Actor_GetEquippedWeapon = (void*(__thiscall*)(void*))0x8A1710;

//ownership
inline auto TESObjectREFR_IsAnOwner = (bool(__thiscall*)(void*, void*, bool))0x5785E0;
inline auto TESObjectREFR_GetOwnerRawForm = (void*(__thiscall*)(void*))0x567790;

//extra data
inline auto BaseExtraList_GetByType = (void*(__thiscall*)(void*, UInt32))0x410220;

//form lookup
inline auto LookupFormByID = (void*(__cdecl*)(unsigned int))0x4839C0;

}
