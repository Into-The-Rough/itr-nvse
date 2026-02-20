//typed engine function pointers - centralized to catch address errors once
#pragma once

namespace Engine {

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

}
