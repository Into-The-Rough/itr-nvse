//kNVSE event registration for ITR events
#pragma once

struct NVSEEventManagerInterface;

extern NVSEEventManagerInterface* g_eventManagerInterface;

void ITR_InitEventManager(void* nvseInterface);
void ITR_RegisterEvents();
