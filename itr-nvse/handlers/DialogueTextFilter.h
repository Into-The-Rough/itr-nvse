#pragma once

class Actor;
class TESTopicInfo;
class TESTopic;

bool DTF_Init(void* nvseInterface);
void DTF_Update();

//native callback type for other plugins
//duration is in seconds, calculated from text length * fNoticeTextTimePerCharacter
typedef void (*DTF_NativeCallback)(Actor* speaker, const char* text, float duration, TESTopicInfo* topicInfo, TESTopic* topic);

//exported functions for inter-plugin communication
extern "C" {
__declspec(dllexport) bool DTF_RegisterNativeCallback(DTF_NativeCallback callback);
__declspec(dllexport) bool DTF_UnregisterNativeCallback(DTF_NativeCallback callback);
}
