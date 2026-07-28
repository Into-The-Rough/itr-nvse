#include "nvse/PluginAPI.h"
#include "ITR.h"
#include "commands/CommandTable.h"

#define ITR_VERSION 20102 // 2.1.1 (major*10000 + minor*100 + patch)

constexpr UInt32 kRequiredNVSEVersion = MAKE_NEW_VEGAS_VERSION(6, 4, 5);

extern "C" {

__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "itr-nvse";
	info->version = ITR_VERSION;

	if (nvse->isEditor) return true;

	if (nvse->nvseVersion < kRequiredNVSEVersion) {
		MessageBoxA(nullptr,
			"ITR requires xNVSE 6.4.5-2 or newer.\n\n"
			"Download the latest xNVSE from the xNVSE Nexus page and replace your nvse_1_4.dll.",
			"ITR - Outdated xNVSE",
			MB_OK | MB_ICONERROR | MB_TASKMODAL);
		return false;
	}

	if (nvse->runtimeVersion != RUNTIME_VERSION_1_4_0_525) return false;
	if (nvse->isNogore) return false;

	return true;
}

__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse)
{
	RegisterAllCommands((void*)nvse);
	if (nvse->isEditor) return true;
	return ITR::Init((void*)nvse);
}

}
