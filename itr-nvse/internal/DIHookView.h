//view of xNVSE's DIHookControl singleton (kNVSEData_DIHookControl)
//layout matches xNVSE Hooks_DirectInput8Create.h, cross-checked against
//Console Clipboard's raw offsets (base+4+key*7, e.g. left ctrl rawState at 0xCF)
#pragma once

#include <cstddef>

#include "common/ITypes.h"

namespace DIHook
{
	enum
	{
		kNVSEData_DIHookControl = 1,
		kMacro_MouseButtonOffset = 256,
		kMacro_MouseWheelOffset = kMacro_MouseButtonOffset + 8,
		kMaxMacros = kMacro_MouseWheelOffset + 2,
	};

	enum
	{
		kDisable_User = 1 << 0,
		kDisable_Script = 1 << 1,
		kDisable_All = kDisable_User | kDisable_Script,
	};

	struct KeyInfo
	{
		bool rawState;
		bool gameState;
		bool insertedState;
		bool hold;
		bool tap;
		bool userDisable;
		bool scriptDisable;
	};

	struct ControlView
	{
		void* vtable;
		KeyInfo keys[kMaxMacros];

		void SetKeyDisableState(UInt32 keycode, bool disable, UInt32 mask)
		{
			if (!mask)
				mask = kDisable_All;
			if (keycode >= kMaxMacros)
				return;
			if (mask & kDisable_User)
				keys[keycode].userDisable = disable;
			if (mask & kDisable_Script)
				keys[keycode].scriptDisable = disable;
		}

		void ClearKeyState(UInt32 keycode)
		{
			if (keycode >= kMaxMacros)
				return;
			keys[keycode].rawState = false;
			keys[keycode].gameState = false;
			keys[keycode].insertedState = false;
		}

		//zeroes the per-frame state fields, leaves script requests (hold/tap) and disable flags
		void ClearKeyStates(UInt32 first, UInt32 count)
		{
			for (UInt32 i = first; i < first + count && i < kMaxMacros; ++i)
				ClearKeyState(i);
		}
	};

	static_assert(sizeof(KeyInfo) == 7);
	static_assert(offsetof(ControlView, keys) == 4);
}
