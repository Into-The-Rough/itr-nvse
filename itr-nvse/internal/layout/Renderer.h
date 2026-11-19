#pragma once

#include <cstddef>

#include "common/ITypes.h"

struct IDirect3DDevice9;

//nidx9renderer layout verified against falloutnv.exe 1.4.0.525
struct NiDX9RendererView {
	UInt8 pad000[0x288];
	IDirect3DDevice9* device;
	UInt8 pad28C[0x684 - 0x28C];
	float nearPlane;
	float depthRange;
	UInt8 pad68C[0x6FC - 0x68C];
	UInt8 inactive;
	UInt8 pad6FD[0x980 - 0x6FD];
	float view[16];
	float projection[16];
};

static_assert(offsetof(NiDX9RendererView, device) == 0x288);
static_assert(offsetof(NiDX9RendererView, nearPlane) == 0x684);
static_assert(offsetof(NiDX9RendererView, depthRange) == 0x688);
static_assert(offsetof(NiDX9RendererView, inactive) == 0x6FC);
static_assert(offsetof(NiDX9RendererView, view) == 0x980);
static_assert(offsetof(NiDX9RendererView, projection) == 0x9C0);

inline NiDX9RendererView** const g_niDX9RendererPtr = (NiDX9RendererView**)0x11F4748;
inline float* const g_renderOriginPtr = (float*)0x11F474C;
inline void** const g_systemColorManagerPtr = (void**)0x11D8A88;

inline constexpr UInt32 kSystemColor_HUDMain = 1;

inline NiDX9RendererView* GetNiDX9Renderer()
{
	return *g_niDX9RendererPtr;
}

inline void NiDX9RendererSetCameraData(NiDX9RendererView* renderer, void* camera)
{
	if (renderer && camera)
		reinterpret_cast<void(__thiscall*)(NiDX9RendererView*, void*)>(0x4E9BB0)(renderer, camera);
}

inline UInt32 GetSystemHUDColorRGBA()
{
	void* mgr = *g_systemColorManagerPtr;
	if (!mgr)
		return 0;
	return reinterpret_cast<UInt32(__thiscall*)(void*, UInt32)>(0x7190A0)(mgr, kSystemColor_HUDMain);
}
