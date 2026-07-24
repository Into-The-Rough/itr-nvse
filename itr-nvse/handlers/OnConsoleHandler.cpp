//fires ITR:OnConsoleOpen / ITR:OnConsoleClose via polling
//MenuConsole::IsConsoleVisible checks ucConsoleState > 0

#include "OnConsoleHandler.h"
#include "internal/NVSEPluginAPI.h"
#include "internal/EventDispatch.h"
#include "internal/EngineFunctions.h"

static bool g_lastVisible = false;

namespace OnConsoleHandler {
void Update()
{
	const bool visible = Engine::IsConsoleVisible();

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
