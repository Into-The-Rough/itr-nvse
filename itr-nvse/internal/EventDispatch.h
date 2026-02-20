//kNVSE event registration for ITR events
#pragma once

#include "EventManagerInterface.h"

extern EventManager::Interface* g_eventManagerInterface;

void ITR_InitEventManager(void* nvseInterface);
void ITR_RegisterEvents();
