#pragma once

#include <cstddef>

#include "common/ITypes.h"

//netimmerse layouts not fully covered by the bundled sdk

struct NiTransformView {
	float rotate[9];
	float translate[3];
	float scale;
};

struct NiRefObjectView {
	void* vtbl;
	UInt32 refCount;
};

template <typename T>
struct NiTObjectArrayView {
	void** vtbl;
	T* data;
	UInt16 capacity;
	UInt16 firstFreeEntry;
	UInt16 numObjs;
	UInt16 growSize;
};

struct NiAVObjectView {
	UInt8 pad00[0x18];
	void* parent;
	void* collisionObject;
	UInt8 pad20[0x34 - 0x20];
	NiTransformView local;
	NiTransformView world;
};

struct NiNodeView {
	NiAVObjectView object;
	NiTObjectArrayView<void*> children;
};

struct NiMaterialPropertyView {
	UInt8 pad00[0x28];
	float emissiveR;
	float emissiveG;
	float emissiveB;
	UInt8 pad34[0x40 - 0x34];
	float emitMult;
	UInt32 revision;
};

struct NiGeometryMaterialView {
	UInt8 pad000[0xA4];
	NiMaterialPropertyView* materialProperty;
};

struct Ni2DBufferView {
	NiRefObjectView object;
	UInt32 width;
	UInt32 height;
	void* rendererData;
};

struct NiRenderTargetGroupView {
	NiRefObjectView object;
	UInt8 byte08;
	UInt8 pad09[3];
	void* buffers[4];
	UInt32 numBuffers;
	void* depthStencil;
	void* rendererData;
};

struct NiRenderTargetRendererDataView {
	UInt8 pad00[0x10];
	UInt32 msaaPref;
};

struct NiSkinInstanceView {
	UInt8 pad00[0x10];
	void* actorRoot;
};

static_assert(offsetof(NiRefObjectView, refCount) == 0x04);
static_assert(sizeof(NiTransformView) == 0x34);
static_assert(sizeof(NiTObjectArrayView<void*>) == 0x10);
static_assert(offsetof(NiTObjectArrayView<void*>, data) == 0x04);
static_assert(offsetof(NiTObjectArrayView<void*>, firstFreeEntry) == 0x0A);
static_assert(offsetof(NiTObjectArrayView<void*>, numObjs) == 0x0C);

static_assert(sizeof(NiAVObjectView) == 0x9C);
static_assert(offsetof(NiAVObjectView, parent) == 0x18);
static_assert(offsetof(NiAVObjectView, collisionObject) == 0x1C);
static_assert(offsetof(NiAVObjectView, local) == 0x34);
static_assert(offsetof(NiAVObjectView, local.rotate) == 0x34);
static_assert(offsetof(NiAVObjectView, local.translate) == 0x58);
static_assert(offsetof(NiAVObjectView, local.scale) == 0x64);
static_assert(offsetof(NiAVObjectView, world) == 0x68);
static_assert(offsetof(NiAVObjectView, world.rotate) == 0x68);
static_assert(offsetof(NiAVObjectView, world.translate) == 0x8C);
static_assert(offsetof(NiAVObjectView, world.scale) == 0x98);

static_assert(offsetof(NiNodeView, children) == 0x9C);
static_assert(offsetof(NiNodeView, children.data) == 0xA0);
static_assert(offsetof(NiNodeView, children.firstFreeEntry) == 0xA6);

static_assert(offsetof(NiMaterialPropertyView, emissiveR) == 0x28);
static_assert(offsetof(NiMaterialPropertyView, emissiveG) == 0x2C);
static_assert(offsetof(NiMaterialPropertyView, emissiveB) == 0x30);
static_assert(offsetof(NiMaterialPropertyView, emitMult) == 0x40);
static_assert(offsetof(NiMaterialPropertyView, revision) == 0x44);
static_assert(offsetof(NiGeometryMaterialView, materialProperty) == 0xA4);
static_assert(offsetof(Ni2DBufferView, width) == 0x08);
static_assert(offsetof(Ni2DBufferView, height) == 0x0C);
static_assert(offsetof(Ni2DBufferView, rendererData) == 0x10);
static_assert(offsetof(NiRenderTargetGroupView, depthStencil) == 0x20);
static_assert(offsetof(NiRenderTargetGroupView, rendererData) == 0x24);
static_assert(offsetof(NiRenderTargetRendererDataView, msaaPref) == 0x10);
static_assert(offsetof(NiSkinInstanceView, actorRoot) == 0x10);

inline NiAVObjectView* NiAVObjectAsView(void* object)
{
	return static_cast<NiAVObjectView*>(object);
}

inline NiRefObjectView* NiRefObjectAsView(void* object)
{
	return static_cast<NiRefObjectView*>(object);
}

inline NiNodeView* NiNodeAsView(void* node)
{
	return static_cast<NiNodeView*>(node);
}

inline NiMaterialPropertyView* NiGeometryGetMaterialProperty(void* geometry)
{
	return static_cast<NiGeometryMaterialView*>(geometry)->materialProperty;
}

inline void* NiAVObjectGetCollisionObject(void* object)
{
	return object ? NiAVObjectAsView(object)->collisionObject : nullptr;
}

inline void* NiAVObjectGetAsNiNode(void* object)
{
	if (!object)
		return nullptr;
	void** vtbl = *static_cast<void***>(object);
	return reinterpret_cast<void*(__thiscall*)(void*)>(vtbl[3])(object); //NiAVObject::GetAsNiNode
}

inline UInt16 NiNodeGetChildLimit(void* node)
{
	return NiNodeAsView(node)->children.firstFreeEntry;
}

inline void** NiNodeGetChildData(void* node)
{
	return NiNodeAsView(node)->children.data;
}

inline void NiMaterialPropertySetEmissive(NiMaterialPropertyView* material, float r, float g, float b, float mult)
{
	material->emissiveR = r;
	material->emissiveG = g;
	material->emissiveB = b;
	material->emitMult = mult;
	material->revision++;
}

inline void* NiRenderTargetGroupGetDepthStencil(void* renderTargetGroup)
{
	return renderTargetGroup ? static_cast<NiRenderTargetGroupView*>(renderTargetGroup)->depthStencil : nullptr;
}

inline UInt32 Ni2DBufferGetWidth(void* buffer)
{
	return buffer ? static_cast<Ni2DBufferView*>(buffer)->width : 0;
}

inline UInt32 Ni2DBufferGetHeight(void* buffer)
{
	return buffer ? static_cast<Ni2DBufferView*>(buffer)->height : 0;
}

inline void* Ni2DBufferGetRendererData(void* buffer)
{
	return buffer ? static_cast<Ni2DBufferView*>(buffer)->rendererData : nullptr;
}

inline UInt32 NiRenderTargetRendererDataGetMSAAPref(void* rendererData)
{
	return rendererData ? static_cast<NiRenderTargetRendererDataView*>(rendererData)->msaaPref : 0;
}

inline void* NiSkinInstanceGetActorRoot(void* skinInstance)
{
	return skinInstance ? static_cast<NiSkinInstanceView*>(skinInstance)->actorRoot : nullptr;
}
