//corner message event handler - fires when HUD notification displays
//hooks earlier than SetShowOffOnCornerMessageEventHandler to catch ALL messages including first on load

#include <vector>
#include <string>
#include <Windows.h>
#include <cmath>

#include "CornerMessageHandler.h"
#include "internal/NVSEMinimal.h"
#include "internal/ScopedLock.h"
#include "internal/Detours.h"
#include "internal/EventDispatch.h"

enum eEmotion : UInt32 {
    kEmotion_Neutral = 0,
    kEmotion_Happy = 1,
    kEmotion_Sad = 2,
    kEmotion_Pain = 3,
};

static int NormalizeMetaType(int metaType)
{
    if (metaType < kCornerMeta_Generic || metaType > kCornerMeta_ReputationChange)
        return kCornerMeta_Generic;
    return metaType;
}

struct PendingMetaEntry {
    std::string text;
    float displayTime;
    int metaType;
    DWORD tick;
};

static std::vector<PendingMetaEntry> g_pendingMeta;
static CRITICAL_SECTION g_metaLock;
static volatile LONG g_metaLockInit = 0;

static void EnsureMetaLockInitialized()
{
    InitCriticalSectionOnce(&g_metaLockInit, &g_metaLock);
}

static int ConsumeMessageMeta(const char* text, float displayTime)
{
    if (g_metaLockInit != 2 || !text || !text[0])
        return kCornerMeta_Generic;

    const DWORD now = GetTickCount();
    constexpr DWORD kMetaTtlMs = 60000;
    constexpr float kDisplayTimeTolerance = 0.25f;

    ScopedLock lock(&g_metaLock);

    for (auto it = g_pendingMeta.begin(); it != g_pendingMeta.end();) {
        if ((now - it->tick) > kMetaTtlMs)
            it = g_pendingMeta.erase(it);
        else
            ++it;
    }

    for (auto it = g_pendingMeta.begin(); it != g_pendingMeta.end(); ++it) {
        if (it->text == text && std::fabs(it->displayTime - displayTime) <= kDisplayTimeTolerance) {
            int metaType = it->metaType;
            g_pendingMeta.erase(it);
            return metaType;
        }
    }

    return kCornerMeta_Generic;
}

void CMH_TrackMessageMeta(const char* text, float displayTime, int metaType)
{
    if (!text || !text[0]) return;
    EnsureMetaLockInitialized();

    PendingMetaEntry entry;
    entry.text = text;
    entry.displayTime = displayTime;
    entry.metaType = NormalizeMetaType(metaType);
    entry.tick = GetTickCount();

    ScopedLock lock(&g_metaLock);
    constexpr size_t kMaxMetaEntries = 256;
    if (g_pendingMeta.size() >= kMaxMetaEntries)
        g_pendingMeta.erase(g_pendingMeta.begin());
    g_pendingMeta.push_back(entry);
}

constexpr UInt32 kAddr_HUDMainMenu_ShowNotify = 0x775380;
static Detours::JumpDetour s_detour;

static void DispatchCornerMessage(const char* text, UInt32 emotion,
    const char* iconPath, const char* soundPath, float displayTime, int metaType)
{
    const char* safeText = text ? text : "";
    const char* safeIcon = iconPath ? iconPath : "";
    const char* safeSound = soundPath ? soundPath : "";

    if (g_eventManagerInterface)
        g_eventManagerInterface->DispatchEvent("ITR:OnCornerMessage", nullptr,
            safeText, (int)emotion, safeIcon, safeSound, (double)displayTime, metaType);
}

static bool __fastcall Hook_HUDMainMenu_ShowNotify(
    void* thisPtr, void* edx,
    const char* text, UInt32 emotion,
    const char* iconPath, const char* soundPath,
    float displayTime, bool instant
) {
    int metaType = ConsumeMessageMeta(text, displayTime);

    if (text && text[0])
        DispatchCornerMessage(text, emotion, iconPath, soundPath, displayTime, metaType);

    typedef bool(__thiscall* ShowNotify_t)(void*, const char*, UInt32, const char*, const char*, float, bool);
    return s_detour.GetTrampoline<ShowNotify_t>()(thisPtr, text, emotion, iconPath, soundPath, displayTime, instant);
}

bool CMH_Init(void* nvseInterface) {
    NVSEInterface* nvse = (NVSEInterface*)nvseInterface;
    if (nvse->isEditor) return false;

    EnsureMetaLockInitialized();

    //prologue: push ebp; mov ebp, esp; push -1 = 5 bytes
    if (!s_detour.WriteRelJump(kAddr_HUDMainMenu_ShowNotify, Hook_HUDMainMenu_ShowNotify, 5))
        return false;

    return true;
}
