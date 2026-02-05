#pragma once

struct NVSEInterface_DTF;
struct Script_DTF;
class Actor;
class TESTopicInfo;
class TESTopic;

bool DTF_Init(void* nvseInterface);
bool DTF_AddFilter(const char* filterText, void* callback);
bool DTF_RemoveFilter(const char* filterText, void* callback);
unsigned int DTF_GetOpcode();
void DTF_ClearCallbacks();

//native callback type for other plugins
//duration is in seconds, calculated from text length * fNoticeTextTimePerCharacter
typedef void (*DTF_NativeCallback)(Actor* speaker, const char* text, float duration, TESTopicInfo* topicInfo, TESTopic* topic);

//exported functions for inter-plugin communication
extern "C" {
__declspec(dllexport) bool DTF_RegisterNativeCallback(DTF_NativeCallback callback);
__declspec(dllexport) bool DTF_UnregisterNativeCallback(DTF_NativeCallback callback);
}
