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

extern const _ExtractArgs ExtractArgs;

//TESChallenge offsets (from JIP)
constexpr UInt32 kOffset_Challenge_FullName = 0x18;   //TESFullName
constexpr UInt32 kOffset_Challenge_Description = 0x24; //TESDescription
constexpr UInt32 kOffset_Challenge_Icon = 0x38;       //TESIcon
constexpr UInt32 kOffset_Challenge_Type = 0x54;       //data.type
constexpr UInt32 kOffset_Challenge_Threshold = 0x58;  //data.threshold
constexpr UInt32 kOffset_Challenge_DataFlags = 0x5C;  //data.flags - bit 1 = recurring
constexpr UInt32 kOffset_Challenge_Value1 = 0x64;     //data.value1 (UInt16)
constexpr UInt32 kOffset_Challenge_Amount = 0x6C;     //current progress
constexpr UInt32 kOffset_Challenge_Flags = 0x70;      //bit 1 = completed, bit 2 = flag4, bit 3 = norecur
constexpr UInt32 kOffset_Challenge_SNAM = 0x74;       //completion script

//vanilla getter function addresses
//4230560 decimal = 0x408DA0 hex
constexpr UInt32 kAddr_TESFullName_GetName = 0x408DA0;
//4777776 decimal = 0x48E730 hex
constexpr UInt32 kAddr_TESTexture_GetTextureName = 0x48E730;

//function addresses
constexpr UInt32 kAddr_TESChallenge_IncrementAmount = 0x5F60E0;
constexpr UInt32 kAddr_TESChallenge_ToggleIsCompleted = 0x5F6000;
constexpr UInt32 kAddr_TESChallenge_ClearProgress = 0x5F5820;
constexpr UInt32 kAddr_TESChallenge_ToggleFlag4 = 0x5F6060;
constexpr UInt32 kAddr_IncPCMiscStat = 0x4D5C60;
constexpr UInt32 kAddr_Script_RunScriptEffectStart = 0x5AC340;
constexpr UInt32 kAddr_Interface_ShowNotify = 0x7052F0;
constexpr UInt32 kAddr_PlayMenuSound = 0x706F30;
constexpr UInt32 kAddr_InitChallengesList = 0x5F5880;
//TESDescription vtable Get method is at index 3
//chunk ID for description is 'DESC' = 0x43534544
constexpr UInt32 kChunkID_DESC = 0x43534544;
typedef const char* (__thiscall *_TESDescriptionGet)(void*, TESForm*, UInt32);

constexpr UInt32 kMiscStat_ChallengesCompleted = 27;
constexpr UInt32 kChallengeType_MiscStat = 11;

static bool Cmd_ModChallenge_Execute(COMMAND_ARGS)
{
	*result = 0;

	TESForm* form = nullptr;
	SInt32 amount = 0;

	if (!ExtractArgs(EXTRACT_ARGS, &form, &amount))
		return true;

	if (!form || form->typeID != kFormType_Challenge)
		return true;

	UInt8* challenge = (UInt8*)form;

	//get flags
	UInt32 flags = *(UInt32*)(challenge + kOffset_Challenge_Flags);
	UInt32 dataFlags = *(UInt32*)(challenge + kOffset_Challenge_DataFlags);
	bool isCompleted = (flags & 2) != 0;
	bool isRecurring = (dataFlags & 2) != 0;
	bool isNoRecur = (flags & 8) != 0;

	//check if already completed (and not recurring)
	if (isCompleted && !isRecurring)
	{
		*result = 0;
		return true;
	}

	//get challenge data
	UInt32 threshold = *(UInt32*)(challenge + kOffset_Challenge_Threshold);
	UInt32 oldAmount = *(UInt32*)(challenge + kOffset_Challenge_Amount);
	UInt32 challengeType = *(UInt32*)(challenge + kOffset_Challenge_Type);
	UInt16 value1 = *(UInt16*)(challenge + kOffset_Challenge_Value1);

	//increment the amount
	ThisStdCall(kAddr_TESChallenge_IncrementAmount, form, amount);

	//get new amount
	UInt32 newAmount = *(UInt32*)(challenge + kOffset_Challenge_Amount);

	//check for completion (crossed threshold)
	if (oldAmount < threshold && newAmount >= threshold)
	{
		//run completion script if present
		Script* completionScript = *(Script**)(challenge + kOffset_Challenge_SNAM);
		if (completionScript)
		{
			PlayerCharacter* player = PlayerCharacter::GetSingleton();
			ThisStdCall(kAddr_Script_RunScriptEffectStart, completionScript, player, 0);
		}

		//increment challenges completed stat (unless this IS a challenges completed MiscStat challenge)
		if (challengeType != kChallengeType_MiscStat || value1 != kMiscStat_ChallengesCompleted)
		{
			((void(__cdecl*)(UInt32))kAddr_IncPCMiscStat)(kMiscStat_ChallengesCompleted);
		}

		//show notification (bShowChallengeUpdates:GamePlay defaults to true)
		{
			//get name via TESFullName::GetName(this)
			void* fullNameObj = challenge + kOffset_Challenge_FullName;
			const char* name = (const char*)ThisStdCall(kAddr_TESFullName_GetName, fullNameObj);

			//skip description for now - vtable call needs investigation
			const char* desc = "";

			//get icon via TESTexture::GetTextureName(form, refr)
			const char* iconPath = ((const char*(__cdecl*)(TESForm*, void*))kAddr_TESTexture_GetTextureName)(form, nullptr);

			//format notification
			char msg[512];
			snprintf(msg, 512, "%s   %d\\%d\n%s", name ? name : "", threshold, threshold, desc ? desc : "");

			//show notification and play sound
			((void(__cdecl*)(const char*, eEmotion, const char*, const char*, float, bool))kAddr_Interface_ShowNotify)
				(msg, neutral, iconPath, nullptr, 2.0f, false);
			((void(__cdecl*)(int))kAddr_PlayMenuSound)(21);
		}

		//handle completion vs recurring
		if (!isRecurring || isNoRecur)
		{
			//mark as completed
			ThisStdCall(kAddr_TESChallenge_ToggleIsCompleted, form, 1);
			//refresh challenge lists
			((void(__cdecl*)())kAddr_InitChallengesList)();
		}
		else
		{
			//recurring: clear progress, set flag4, keep overflow
			UInt32 overflow = newAmount - threshold;
			ThisStdCall(kAddr_TESChallenge_ClearProgress, form);
			ThisStdCall(kAddr_TESChallenge_ToggleFlag4, form, 1);
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

void ChallengeCommands_Init(void* nvse)
{
	NVSEInterface* nvseIntf = (NVSEInterface*)nvse;
	nvseIntf->SetOpcodeBase(0x4034);
	nvseIntf->RegisterCommand(&kCommandInfo_ModChallenge);
}
