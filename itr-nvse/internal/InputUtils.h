#pragma once
#include <Windows.h>

inline int MapDIKToVK(unsigned int dik) {
	static const int dikToVk[256] = {
		0, VK_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', VK_OEM_MINUS, VK_OEM_PLUS, VK_BACK, VK_TAB,
		'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', VK_OEM_4, VK_OEM_6, VK_RETURN, VK_LCONTROL,
		'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', VK_OEM_1, VK_OEM_7, VK_OEM_3, VK_LSHIFT, VK_OEM_5,
		'Z', 'X', 'C', 'V', 'B', 'N', 'M', VK_OEM_COMMA, VK_OEM_PERIOD, VK_OEM_2, VK_RSHIFT, VK_MULTIPLY,
		VK_LMENU, VK_SPACE, VK_CAPITAL, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10,
		VK_NUMLOCK, VK_SCROLL, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_SUBTRACT, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6,
		VK_ADD, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL, 0, 0, VK_OEM_102, VK_F11, VK_F12
	};
	return (dik < 256) ? dikToVk[dik] : 0;
}

inline bool IsRawKeyPressed(unsigned int keycode) {
	int vk = MapDIKToVK(keycode);
	return vk && (GetAsyncKeyState(vk) & 0x8000);
}
