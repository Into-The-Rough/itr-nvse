//key held detection - fires ITR:OnKeyHeld while any key held past threshold

#include <cstring>
#include <Windows.h>

#include "KeyHeldHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"

static int g_thresholdMs = 500;
static int g_repeatMs = 250;
static DWORD g_downSince[256];
static DWORD g_lastDispatch[256];

namespace KeyHeldHandler {
void Update()
{
	if (!g_eventManagerInterface) return;

	void* input = *(void**)0x11F35CC;
	if (!input) return;
	UInt8* keys = (UInt8*)((UInt8*)input + 0x18F8);
	DWORD now = GetTickCount();

	for (int i = 1; i < 256; i++) {
		if (keys[i]) {
			if (!g_downSince[i]) {
				g_downSince[i] = now;
				g_lastDispatch[i] = 0;
			}
			DWORD held = now - g_downSince[i];
			if (held >= (DWORD)g_thresholdMs) {
				if (!g_lastDispatch[i] || (now - g_lastDispatch[i]) >= (DWORD)g_repeatMs) {
					float sec = held / 1000.0f;
					g_eventManagerInterface->DispatchEvent("ITR:OnKeyHeld", nullptr, i, PackEventFloatArg(sec));
					g_lastDispatch[i] = now;
				}
			}
		} else {
			g_downSince[i] = 0;
		}
	}
}

bool Init()
{
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	char* s = strrchr(path, '\\');
	if (s) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "Data\\config\\itr-nvse.ini");

	g_thresholdMs = GetPrivateProfileIntA("Events", "iKeyHeldThresholdMs", 500, path);
	g_repeatMs = GetPrivateProfileIntA("Events", "iKeyHeldRepeatMs", 250, path);
	memset(g_downSince, 0, sizeof(g_downSince));
	memset(g_lastDispatch, 0, sizeof(g_lastDispatch));
	return true;
}
}
