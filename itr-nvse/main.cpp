#include "nvse/PluginAPI.h"
#include "ITR.h"
#include "commands/CommandTable.h"

#define ITR_VERSION 107

extern "C" {

__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "itr-nvse";
	info->version = ITR_VERSION;

	if (nvse->isEditor) return true;
	if (nvse->nvseVersion < NVSE_VERSION_INTEGER) return false;
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
