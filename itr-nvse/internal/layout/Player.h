//Player layout - aim-zoom POV/FOV, casino data, killcam timer
#pragma once

#include <cstddef>

#include "nvse/GameObjects.h"

struct PlayerCasinoDataView {
	UInt8 pad00[0x610];
	void* casinoDataList;
};

struct PlayerAimFOVView {
	UInt8 pad00[0x64B];
	UInt8 is3rdPerson;
	bool bThirdPerson;
	UInt8 pad64D[0x670 - 0x64D];
	float worldFOV;
	float firstPersonFOV;
};

struct PlayerKillCamView {
	UInt8 pad00[0xE18];
	float killCamTimer;
};

struct TESCasinoView {
	UInt8 pad00[0x210];
	UInt32 maxWinnings;
};

static_assert(offsetof(PlayerCharacter, actorMover) == 0x190);
static_assert(offsetof(PlayerCharacter, bThirdPerson) == 0x64C);
static_assert(offsetof(PlayerCharacter, playerNode) == 0x694);
static_assert(offsetof(PlayerCasinoDataView, casinoDataList) == 0x610);
static_assert(offsetof(PlayerAimFOVView, is3rdPerson) == 0x64B);
static_assert(offsetof(PlayerAimFOVView, bThirdPerson) == 0x64C);
static_assert(offsetof(PlayerAimFOVView, worldFOV) == 0x670);
static_assert(offsetof(PlayerAimFOVView, firstPersonFOV) == 0x674);
static_assert(offsetof(PlayerKillCamView, killCamTimer) == 0xE18);
static_assert(offsetof(TESCasinoView, maxWinnings) == 0x210);

inline void* PlayerCharacterGetCasinoDataList(PlayerCharacter* player)
{
	return player ? reinterpret_cast<PlayerCasinoDataView*>(player)->casinoDataList : nullptr;
}

inline bool PlayerCharacterIsAimZoomThirdPerson(PlayerCharacter* player)
{
	return player && reinterpret_cast<PlayerAimFOVView*>(player)->is3rdPerson != 0;
}

inline void PlayerCharacterSetFOVs(PlayerCharacter* player, float worldFOV, float firstPersonFOV)
{
	if (!player) return;
	auto* view = reinterpret_cast<PlayerAimFOVView*>(player);
	view->worldFOV = worldFOV;
	view->firstPersonFOV = firstPersonFOV;
}

inline float PlayerCharacterGetKillCamTimer(PlayerCharacter* player)
{
	return player ? reinterpret_cast<PlayerKillCamView*>(player)->killCamTimer : 0.0f;
}

inline UInt32 TESCasinoGetMaxWinnings(TESForm* casino)
{
	return casino ? reinterpret_cast<TESCasinoView*>(casino)->maxWinnings : 0;
}
