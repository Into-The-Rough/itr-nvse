//ModChallenge - modify challenge progress by any amount (positive or negative)
//works on ALL challenge types, not just scripted
//replicates full vanilla completion logic: script, stat, notification, sound

#include "ChallengeCommands.h"
#include "nvse/PluginAPI.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/CommandTable.h"
#include "nvse/ParamInfos.h"
#include <cstdio>
#include "internal/CallTemplates.h"
#include "internal/EngineFunctions.h"
#include "internal/GameSDK.h"
#include "internal/layout/Challenge.h"

extern const _ExtractArgs ExtractArgs;

constexpr UInt32 kAddr_TESChallenge_IncrementAmount = 0x5F60E0;
constexpr UInt32 kChallengeFlag_Completed = 2;
constexpr UInt32 kChallengeFlag_NoRecur = 8;
constexpr UInt32 kChallengeDataFlag_Recurring = 2;
constexpr UInt32 kChallengeType_MiscStat = 11;
constexpr UInt32 kMiscStat_ChallengesCompleted = 27;

//TESDescription::Get is at vtable[4] (after 4 BaseFormComponent virtuals)
typedef const char* (__thiscall *_TESDescriptionGet)(void*, TESForm*, UInt32);

//helper to show challenge notification
static void ShowChallengeNotification(TESChallengeView* challenge, TESForm* form, UInt32 currentAmount, UInt32 threshold)
{
	void* fullNameObj = &challenge->fullName;
	const char* name = (const char*)ThisStdCall(0x408DA0, fullNameObj); //TESFullName::GetName

	//get description via vtable[4] - TESDescription::Get(overrideForm, chunkID)
	void* descObj = &challenge->description;
	void** descVtbl = *(void***)descObj;
	const char* desc = ((_TESDescriptionGet)descVtbl[4])(descObj, nullptr, 0x43534544); //'DESC' chunk ID

	const char* iconPath = CdeclCall<const char*>(0x48E730, form, nullptr); //TESTexture::GetTextureName

	char msg[512];
	snprintf(msg, 512, "%s   %d\\%d\n%s", name ? name : "", currentAmount, threshold, desc ? desc : "");

	Engine::QueueUIMessage(msg, neutral, iconPath, nullptr, 2.0f, false);
}

static bool Cmd_ModChallenge_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* form = nullptr;
	SInt32 amount = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &form, &amount))
		return true;

	if (!form || form->typeID != kFormType_Challenge)
		return true;

	TESChallengeView* challenge = TESChallengeAsView(form);

	UInt32 flags = challenge->challengeFlags;
	UInt32 dataFlags = challenge->data.flags;
	bool isCompleted = (flags & kChallengeFlag_Completed) != 0;
	bool isRecurring = (dataFlags & kChallengeDataFlag_Recurring) != 0;
	bool isNoRecur = (flags & kChallengeFlag_NoRecur) != 0;

	if (isCompleted && !isRecurring)
	{
		*result = 0;
		return true;
	}

	UInt32 threshold = challenge->data.threshold;
	UInt32 oldAmount = challenge->amount;
	UInt32 challengeType = challenge->data.type;
	UInt16 value1 = challenge->data.value1;

	//increment the amount, clamp to 0 to prevent unsigned wrap triggering completion
	SInt32 signedOld = (SInt32)oldAmount;
	SInt32 signedNew = signedOld + amount;
	if (signedNew < 0) signedNew = 0;
	challenge->amount = (UInt32)signedNew;
	ThisStdCall(0x5F5800, form); //TESChallenge::MarkCountAsModified

	UInt32 newAmount = (UInt32)signedNew;

	//show progress notification at interval boundaries (vanilla behaviour)
	if (newAmount < threshold)
	{
		UInt32 interval = challenge->data.interval;
		if (interval == 0) interval = 100; //default

		UInt32 oldIntervals = oldAmount / interval;
		UInt32 newIntervals = newAmount / interval;
		if (newIntervals > oldIntervals)
		{
			ShowChallengeNotification(challenge, form, newAmount, threshold);
		}
	}

	if (oldAmount < threshold && newAmount >= threshold)
	{
		Script* completionScript = reinterpret_cast<Script*>(challenge->completionScript);
		if (completionScript)
		{
			PlayerCharacter* player = PlayerCharacter::GetSingleton();
			if (player)
				ThisStdCall(0x5AC340, completionScript, player, 0); //Script::RunScriptEffectStart
		}

		//increment challenges completed stat (unless this IS a challenges completed MiscStat challenge)
		if (challengeType != kChallengeType_MiscStat || value1 != kMiscStat_ChallengesCompleted)
		{
			CdeclCall(0x4D5C60, kMiscStat_ChallengesCompleted); //IncPCMiscStat
		}

		ShowChallengeNotification(challenge, form, threshold, threshold);
		CdeclCall(0x706F30, 21); //PlayMenuSound

		if (!isRecurring || isNoRecur)
		{
			ThisStdCall(0x5F6000, form, 1); //TESChallenge::ToggleIsCompleted
			CdeclCall(0x5F5880); //InitChallengesList
		}
		else
		{
			//recurring: clear progress, set flag4, keep overflow
			UInt32 overflow = newAmount - threshold;
			ThisStdCall(0x5F5820, form); //TESChallenge::ClearProgress
			ThisStdCall(0x5F6060, form, 1); //TESChallenge::ToggleFlag4
			if (overflow > 0)
			{
				ThisStdCall(kAddr_TESChallenge_IncrementAmount, form, (SInt32)overflow);
			}
		}
	}

	*result = 1;
	return true;
}

static ParamInfo kParams_ModChallenge[2] = {
	{"challenge", kParamType_AnyForm, 0},
	{"amount", kParamType_Integer, 0},
};

DEFINE_COMMAND_PLUGIN(ModChallenge, "Modify challenge progress by amount (positive or negative). Works on all challenge types.", 0, 2, kParams_ModChallenge);

namespace ChallengeCommands {
void Init(void* nvse) {}

void RegisterCommands(void* nvsePtr)
{
	NVSEInterface* nvse = (NVSEInterface*)nvsePtr;
	nvse->RegisterCommand(&kCommandInfo_ModChallenge);
}
}
