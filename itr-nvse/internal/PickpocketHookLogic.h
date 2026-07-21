#pragma once

namespace PickpocketHookLogic
{
	//The game passes selection/count in ECX/EDX and actor/itemValue on the stack.
	using TryPickpocket_t = bool (__fastcall*)(void* selection, SInt32 count, void* actor, SInt32 itemValue);

	inline bool ForwardTryPickpocket(TryPickpocket_t target, void* selection, SInt32 count, void* actor, SInt32 itemValue)
	{
		return target ? target(selection, count, actor, itemValue) : false;
	}
}
