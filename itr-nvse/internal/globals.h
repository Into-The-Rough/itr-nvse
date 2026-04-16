#pragma once

extern void Log(const char* fmt, ...);
extern void Console_Print(const char* fmt, ...);
extern bool IsConsoleMode();

//true between kMessage_PreLoadGame and kMessage_PostLoadGame — hooks that would
//dispatch script events during save restore must gate on this to avoid firing on
//rebuilt state (e.g. Crime::AddtoActorKnowList called from ExtraDataList::InitLoadGameBGS)
extern bool g_isLoadingSave;
