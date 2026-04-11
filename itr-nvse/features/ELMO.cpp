//ELMO - convert objectives and reputation popups to corner messages
//NOT fully hot-reloadable - reload does not remove inline patches

#include "ELMO.h"
#include "handlers/CornerMessageHandler.h"
#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/SafeWrite.h"
#include <cstdio>

#include "internal/globals.h"

namespace ELMO
{
	char g_msgBuffer[512];

	enum eEmotion : UInt32 {
		kEmotion_Neutral = 0,
		kEmotion_Happy = 1,
	};

	typedef bool (*_QueueUIMessage)(const char* msg, UInt32 emotion, const char* ddsPath, const char* soundName, float msgTime, bool maybeNextToDisplay);
	static const _QueueUIMessage QueueUIMsg = (_QueueUIMessage)0x007052F0;
	static constexpr UInt32 kObjectiveDisplayedCall = 0x5EC653;
	static constexpr UInt32 kObjectiveCompletedCall = 0x5EC6BA;
	static constexpr UInt32 kObjectiveReminderCall = 0x77377E;
	static constexpr float kObjectiveDisplayTime = 10.0f;
	static constexpr float kReputationDisplayTime = 8.0f;

	static Detours::CallDetour s_objectiveDisplayedCall;
	static Detours::CallDetour s_objectiveCompletedCall;
	static Detours::CallDetour s_objectiveReminderCall;
	static bool s_objectiveHooksInstalled = false;

	const char* __cdecl FormatReputationMessage(const char* factionName, const char* repTitle, const char* repDesc)
	{
		sprintf_s(g_msgBuffer, "%s - %s. %s", factionName, repTitle, repDesc);
		return g_msgBuffer;
	}

	static UInt32 GetObjectiveEmotion(UInt32 isCompleted, UInt32 isReminder)
	{
		if (isReminder)
			return kEmotion_Neutral;
		return isCompleted ? kEmotion_Happy : kEmotion_Neutral;
	}

	static const char* GetINIString(UInt32 settingAddr)
	{
		return ThisCall<const char*>(0x403DF0, reinterpret_cast<void*>(settingAddr));
	}

	static void ShowReputationThresholdAsCornerMessage(void* reputation)
	{
		if (!reputation)
			return;

		auto repDesc = ThisCall<const char*>(0x616890, reputation);
		auto repTitle = ThisCall<const char*>(0x616710, reputation);
		auto factionName = ThisCall<const char*>(0x408DA0, reinterpret_cast<UInt8*>(reputation) + 0x18);
		auto message = FormatReputationMessage(
			factionName ? factionName : "",
			repTitle ? repTitle : "",
			repDesc ? repDesc : "");
		auto iconPath = ThisCall<const char*>(0x6167D0, reputation);
		auto wasPositive = *reinterpret_cast<UInt32*>(reinterpret_cast<UInt8*>(reputation) + 0x4C) == 1;
		auto soundPath = GetINIString(wasPositive ? 0x11CBCB0 : 0x11CBA30);

		CornerMessageHandler::TrackMessageMeta(message, kReputationDisplayTime, kCornerMeta_ReputationChange);
		QueueUIMsg(message, kEmotion_Neutral, iconPath, soundPath, kReputationDisplayTime, false);
	}

	void __cdecl ShowAsCornerMessage(const char* text, UInt32 isCompleted, UInt32 isReminder)
	{
		if (text) {
			const int metaType = isCompleted ? kCornerMeta_ObjectiveCompleted : kCornerMeta_ObjectiveDisplayed;
			CornerMessageHandler::TrackMessageMeta(text, kObjectiveDisplayTime, metaType);
			QueueUIMsg(text, GetObjectiveEmotion(isCompleted, isReminder), nullptr, nullptr, kObjectiveDisplayTime, false);
		}
	}

	//hook direct callers so other plugins can still patch SetQuestUpdateText itself
	bool InstallObjectiveHooks()
	{
		if (s_objectiveHooksInstalled)
			return true;

		static constexpr UInt32 kObjectiveHookSites[] = {
			kObjectiveDisplayedCall,
			kObjectiveCompletedCall,
			kObjectiveReminderCall
		};

		for (auto hookSite : kObjectiveHookSites) {
			if (*reinterpret_cast<UInt8*>(hookSite) != 0xE8) {
				Log("ELMO: Objective hook site %08X is not a CALL", hookSite);
				return false;
			}
		}

		if (!s_objectiveDisplayedCall.WriteRelCall(kObjectiveDisplayedCall, ShowAsCornerMessage) ||
			!s_objectiveCompletedCall.WriteRelCall(kObjectiveCompletedCall, ShowAsCornerMessage) ||
			!s_objectiveReminderCall.WriteRelCall(kObjectiveReminderCall, ShowAsCornerMessage)) {
			Log("ELMO: Failed to install objective call-site hooks");
			return false;
		}

		s_objectiveHooksInstalled = true;
		return true;
	}

	bool __fastcall ReputationPopup_Hook(void* reputation)
	{
		ShowReputationThresholdAsCornerMessage(reputation);
		return true;
	}

	__declspec(naked) void ReputationCornerMessage_Hook_AddRep()
	{
		static const UInt32 returnAddr = 0x615F71;

		__asm {
			pushad
			pushfd
			push dword ptr [ebp - 0x128]                    //Reputation* local in the caller's frame
			call ShowReputationThresholdAsCornerMessage
			add esp, 4                                      //cdecl cleanup
			popfd
			popad
			jmp returnAddr                                  //skip the vanilla popup path
		}
	}

	__declspec(naked) void ReputationCornerMessage_Hook_AddRepExact()
	{
		static const UInt32 returnAddr = 0x6159B2;

		__asm {
			pushad
			pushfd
			push dword ptr [ebp - 0x110]                    //Reputation* local (different frame layout)
			call ShowReputationThresholdAsCornerMessage
			add esp, 4
			popfd
			popad
			jmp returnAddr
		}
	}

	__declspec(naked) void ReputationCornerMessage_Hook_RemRepExact()
	{
		static const UInt32 returnAddr = 0x615C6A;

		__asm {
			pushad
			pushfd
			push dword ptr [ebp - 0x110]                    //shares layout with AddRepExact
			call ShowReputationThresholdAsCornerMessage
			add esp, 4
			popfd
			popad
			jmp returnAddr
		}
	}

	__declspec(naked) void ReputationCornerMessage_Hook_RemRep()
	{
		static const UInt32 returnAddr = 0x616269;

		__asm {
			pushad
			pushfd
			push dword ptr [ebp - 0x128]                    //shares layout with AddRep
			call ShowReputationThresholdAsCornerMessage
			add esp, 4
			popfd
			popad
			jmp returnAddr
		}
	}

	void Init(bool suppressObjectives, bool suppressReputation)
	{
		if (suppressObjectives)
			InstallObjectiveHooks();

		if (suppressReputation) {
			SafeWrite::WriteRelJump(0x6155F0, (UInt32)ReputationPopup_Hook);
			SafeWrite::WriteRelJump(0x615F4A, (UInt32)ReputationCornerMessage_Hook_AddRep);
			SafeWrite::WriteRelJump(0x61598B, (UInt32)ReputationCornerMessage_Hook_AddRepExact);
			SafeWrite::WriteRelJump(0x615C43, (UInt32)ReputationCornerMessage_Hook_RemRepExact);
			SafeWrite::WriteRelJump(0x616242, (UInt32)ReputationCornerMessage_Hook_RemRep);
		}
	}
}
