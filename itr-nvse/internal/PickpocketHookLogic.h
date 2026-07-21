#pragma once

namespace PickpocketHookLogic
{
	//Use the complete call-site machine shape so chaining preserves ECX, EDX and both
	//stack arguments. Vanilla ignores EDX; another hook may not.
	using TryPickpocket_t = bool (__fastcall*)(void* menu, SInt32 incomingEdx, void* actor, SInt32 count);

	inline bool ForwardTryPickpocket(TryPickpocket_t target, void* menu, SInt32 incomingEdx, void* actor, SInt32 count)
	{
		return target ? target(menu, incomingEdx, actor, count) : false;
	}

	template <typename IsLiveGrenadeFn>
	inline bool ShouldSkipKarma(void* menu, void* selectedEntry, void* player, void* actor,
		bool isReverse, IsLiveGrenadeFn isLiveGrenade)
	{
		if (!menu || !selectedEntry || !player || !actor || !isReverse)
			return false;

		return !isLiveGrenade(menu, selectedEntry, player, actor);
	}
}
