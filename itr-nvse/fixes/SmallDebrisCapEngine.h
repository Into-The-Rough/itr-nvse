#pragma once
#include "common/ITypes.h"

class TESObjectREFR;

namespace SmallDebrisCap::Engine {
	void Retain(void* node);
	void Release(void* node);
	bool IsFadeNode(void* node);
	void* Root(void* node);
	bool InScene(void* node);
	void* DebrisNode(void* body);
	TESObjectREFR* CullableRef(void* node);
	void Fade(void* object, float fraction);
	void Cull(void* node, bool whole, const bool& cancelled);
	SInt32 MaxCount();
	float FadeTime();
}
