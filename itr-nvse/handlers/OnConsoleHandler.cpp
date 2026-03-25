//fires ITR:OnConsoleOpen / ITR:OnConsoleClose via polling
//MenuConsole::IsConsoleVisible checks ucConsoleState > 0

#include "OnConsoleHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/CallTemplates.h"

static bool g_lastVisible = false;
static UInt8* g_consoleOpen = (UInt8*)0x11DEA2E;

namespace OnConsoleHandler {
void Update()
{
	bool visible = false;
	if (*g_consoleOpen)
	{
		//MenuConsole::Instance(0) at 0x71B160
		void* console = CdeclCall<void*>(0x71B160, 0);
		if (console)
		{
			//MenuConsole::IsConsoleActive at 0x4A4020
			visible = ThisCall<bool>(0x4A4020, console);
		}
	}

	if (visible != g_lastVisible)
	{
		g_lastVisible = visible;
		if (g_eventManagerInterface)
		{
			if (visible)
				g_eventManagerInterface->DispatchEvent("ITR:OnConsoleOpen", nullptr);
			else
				g_eventManagerInterface->DispatchEvent("ITR:OnConsoleClose", nullptr);
		}
	}
}

bool Init(void* nvseInterface)
{
	NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
	if (nvse->isEditor) return false;
	return true;
}
}
