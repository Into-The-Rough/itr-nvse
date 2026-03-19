//fires ITR:OnConsoleOpen / ITR:OnConsoleClose via polling
//MenuConsole::IsConsoleVisible checks ucConsoleState > 0

#include "OnConsoleHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/EventDispatch.h"
#include "internal/CallTemplates.h"

static bool g_lastVisible = false;

namespace OnConsoleHandler {
void Update()
{
	//MenuConsole::Instance(1) at 0x71B160
	void* console = CdeclCall<void*>(0x71B160, 1);
	if (!console) return;

	//MenuConsole::IsConsoleActive at 0x4A4020
	bool visible = ThisCall<bool>(0x4A4020, console);

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
