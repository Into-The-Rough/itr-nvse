//ELMO - convert objectives and reputation popups to corner messages
//NOT hot-reloadable - mid-function patches break instruction boundaries

#include "ELMO.h"
#include <Windows.h>
#include <cstdio>

extern void Log(const char* fmt, ...);

namespace ELMO
{
	char g_msgBuffer[512];

	typedef bool (*_QueueUIMessage)(const char* msg, UInt32 emotion, const char* ddsPath, const char* soundName, float msgTime, bool maybeNextToDisplay);
	static const _QueueUIMessage QueueUIMsg = (_QueueUIMessage)0x007052F0;

	const char* __cdecl FormatReputationMessage(const char* factionName, const char* repTitle, const char* repDesc)
	{
		sprintf_s(g_msgBuffer, "%s - %s. %s", factionName, repTitle, repDesc);
		return g_msgBuffer;
	}

	void __cdecl ShowAsCornerMessage(const char* text, UInt32 isCompleted, UInt32 allowDisplayMultiple)
	{
		if (text) {
			const char* metaPath = isCompleted ? "ELMO:objective:completed" : "ELMO:objective:displayed";
			QueueUIMsg(text, 2, metaPath, nullptr, 10.0f, false);
		}
	}

	__declspec(naked) void Hook_SetQuestUpdateText()
	{
		__asm {
			push ebp
			mov ebp, esp
			sub esp, 8
			push eax
			push ecx
			push edx
			mov eax, [ebp + 8]
			push eax
			mov ecx, [ebp + 0xC]
			push ecx
			mov edx, [ebp + 0x10]
			push edx
			push edx
			push ecx
			push eax
			call ShowAsCornerMessage
			add esp, 0xC
			pop edx
			pop ecx
			pop eax
			mov esp, ebp
			pop ebp
			ret
		}
	}

	__declspec(naked) void ReputationPopup_Hook()
	{
		static const UInt32 queueUIMsg = 0x007052F0;
		static const UInt32 getRepDesc = 0x00616890;
		static const UInt32 getRepTitle = 0x00616710;
		static const UInt32 getFactionName = 0x00408DA0;

		__asm {
			push ebp
			mov ebp, esp
			pushad
			mov edi, ecx
			mov ecx, edi
			call getRepDesc
			push eax
			mov ecx, edi
			call getRepTitle
			push eax
			mov ecx, edi
			add ecx, 0x18
			call getFactionName
			push eax
			call FormatReputationMessage
			add esp, 0xC
			push 0
			push 0x41000000
			push 0
			push 0
			push 2
			push eax
			call queueUIMsg
			add esp, 0x18
			popad
			pop ebp
			retn
		}
	}

	__declspec(naked) void ReputationCornerMessage_Hook_AddRep()
	{
		static const UInt32 queueUIMsg = 0x007052F0;
		static const UInt32 getRepDesc = 0x00616890;
		static const UInt32 getRepTitle = 0x00616710;
		static const UInt32 getFactionName = 0x00408DA0;
		static const UInt32 returnAddr = 0x615F71;

		__asm {
			pushad
			mov edi, [ebp - 0x128]
			mov ecx, edi
			call getRepDesc
			push eax
			mov ecx, edi
			call getRepTitle
			push eax
			mov ecx, edi
			add ecx, 0x18
			call getFactionName
			push eax
			call FormatReputationMessage
			add esp, 0xC
			push 0
			push 0x41000000
			push 0
			push 0
			push 2
			push eax
			call queueUIMsg
			add esp, 0x18
			popad
			jmp returnAddr
		}
	}

	__declspec(naked) void ReputationCornerMessage_Hook_AddRepExact()
	{
		static const UInt32 queueUIMsg = 0x007052F0;
		static const UInt32 getRepDesc = 0x00616890;
		static const UInt32 getRepTitle = 0x00616710;
		static const UInt32 getFactionName = 0x00408DA0;
		static const UInt32 returnAddr = 0x6159B2;

		__asm {
			pushad
			mov edi, [ebp - 0x110]
			mov ecx, edi
			call getRepDesc
			push eax
			mov ecx, edi
			call getRepTitle
			push eax
			mov ecx, edi
			add ecx, 0x18
			call getFactionName
			push eax
			call FormatReputationMessage
			add esp, 0xC
			push 0
			push 0x41000000
			push 0
			push 0
			push 2
			push eax
			call queueUIMsg
			add esp, 0x18
			popad
			jmp returnAddr
		}
	}

	__declspec(naked) void ReputationCornerMessage_Hook_RemRepExact()
	{
		static const UInt32 queueUIMsg = 0x007052F0;
		static const UInt32 getRepDesc = 0x00616890;
		static const UInt32 getRepTitle = 0x00616710;
		static const UInt32 getFactionName = 0x00408DA0;
		static const UInt32 returnAddr = 0x615C6A;

		__asm {
			pushad
			mov edi, [ebp - 0x110]
			mov ecx, edi
			call getRepDesc
			push eax
			mov ecx, edi
			call getRepTitle
			push eax
			mov ecx, edi
			add ecx, 0x18
			call getFactionName
			push eax
			call FormatReputationMessage
			add esp, 0xC
			push 0
			push 0x41000000
			push 0
			push 0
			push 2
			push eax
			call queueUIMsg
			add esp, 0x18
			popad
			jmp returnAddr
		}
	}

	__declspec(naked) void ReputationCornerMessage_Hook_RemRep()
	{
		static const UInt32 queueUIMsg = 0x007052F0;
		static const UInt32 getRepDesc = 0x00616890;
		static const UInt32 getRepTitle = 0x00616710;
		static const UInt32 getFactionName = 0x00408DA0;
		static const UInt32 returnAddr = 0x616269;

		__asm {
			pushad
			mov edi, [ebp - 0x128]
			mov ecx, edi
			call getRepDesc
			push eax
			mov ecx, edi
			call getRepTitle
			push eax
			mov ecx, edi
			add ecx, 0x18
			call getFactionName
			push eax
			call FormatReputationMessage
			add esp, 0xC
			push 0
			push 0x41000000
			push 0
			push 0
			push 2
			push eax
			call queueUIMsg
			add esp, 0x18
			popad
			jmp returnAddr
		}
	}

	void WriteRelJump(UInt32 src, UInt32 dst) {
		DWORD oldProtect;
		VirtualProtect((void*)src, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
		*(UInt8*)src = 0xE9;
		*(UInt32*)(src + 1) = dst - src - 5;
		VirtualProtect((void*)src, 5, oldProtect, &oldProtect);
	}

	void Init(bool suppressObjectives, bool suppressReputation)
	{
		if (suppressObjectives) {
			WriteRelJump(0x77A5B0, (UInt32)Hook_SetQuestUpdateText);
			Log("ELMO: Objectives popup suppressed");
		}

		if (suppressReputation) {
			WriteRelJump(0x6155F0, (UInt32)ReputationPopup_Hook);
			WriteRelJump(0x615F4A, (UInt32)ReputationCornerMessage_Hook_AddRep);
			WriteRelJump(0x61598B, (UInt32)ReputationCornerMessage_Hook_AddRepExact);
			WriteRelJump(0x615C43, (UInt32)ReputationCornerMessage_Hook_RemRepExact);
			WriteRelJump(0x616242, (UInt32)ReputationCornerMessage_Hook_RemRep);
			Log("ELMO: Reputation popup suppressed");
		}
	}
}

void ELMO_Init(bool suppressObjectives, bool suppressReputation)
{
	ELMO::Init(suppressObjectives, suppressReputation);
}
