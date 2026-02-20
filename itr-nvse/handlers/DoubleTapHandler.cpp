//double tap detection - fires ITR:OnDoubleTap when any key pressed twice within threshold

#include <cstring>
#include <Windows.h>

#include "DoubleTapHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

static int g_thresholdMs = 300;
static bool g_wasDown[256];
static DWORD g_lastPressTime[256];

void DTH_Update()
{
	if (!g_eventManagerInterface) return;

	void* input = *(void**)0x11F35CC;
	if (!input) return;
	UInt8* keys = (UInt8*)((UInt8*)input + 0x18F8);
	DWORD now = GetTickCount();

	for (int i = 1; i < 256; i++) {
		bool down = keys[i] != 0;
		if (down && !g_wasDown[i]) {
			if (g_lastPressTime[i] && (now - g_lastPressTime[i]) <= (DWORD)g_thresholdMs)
				g_eventManagerInterface->DispatchEvent("ITR:OnDoubleTap", nullptr, i);
			g_lastPressTime[i] = now;
		}
		g_wasDown[i] = down;
	}
}

bool DTH_Init()
{
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	char* s = strrchr(path, '\\');
	if (s) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "Data\\config\\itr-nvse.ini");

	g_thresholdMs = GetPrivateProfileIntA("Events", "iDoubleTapThresholdMs", 300, path);
	memset(g_wasDown, 0, sizeof(g_wasDown));
	memset(g_lastPressTime, 0, sizeof(g_lastPressTime));
	return true;
}
