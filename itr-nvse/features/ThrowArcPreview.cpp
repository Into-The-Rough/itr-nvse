#include "ThrowArcPreview.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <d3d9.h>

#include "internal/CallTemplates.h"
#include "internal/Detours.h"
#include "internal/EngineFunctions.h"
#include "internal/EngineHelpers.h"
#include "internal/GameGlobals.h"
#include "internal/HavokLayout.h"
#include "internal/NiLayout.h"
#include "internal/RayCast.h"
#include "internal/ThrowArcLogic.h"
#include "internal/globals.h"
#include "internal/layout/Player.h"
#include "internal/layout/Renderer.h"

#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameForms.h"
#include "nvse/GameObjects.h"
#include "nvse/ParamInfos.h"
#include "nvse/PluginAPI.h"

extern const _ExtractArgs ExtractArgs;
extern bool g_isLoadingSave;

namespace {
	using ThrowArcLogic::Vec3;
	using ThrowArcLogic::Vec4;

	struct PreviewConfig {
		UInt32 refID;
		UInt32 startTick;
		float minForce;
		float maxForce;
		float chargeTime;
		float tapGrace;
		float upBoostMin;
		float upBoostMax;
		float linearDamping;
		float dashPhase;
		UInt32 lastTick;
		bool active;
	};

	struct ArcSnapshot {
		Vec3 points[ThrowArcLogic::kMaxArcPoints];
		UInt32 pointCount;
		Vec3 hitPos;
		Vec3 hitNormal;
		float chargeRatio;
		float speed;
		float dashPhase;
		UInt8 colorR;
		UInt8 colorG;
		UInt8 colorB;
		bool hasHit;
		bool valid;
	};

	struct ScreenVertex {
		float x;
		float y;
		float z;
		float rhw;
		D3DCOLOR color;
	};

	struct CameraSnapshot {
		float view[16];
		float projection[16];
		Vec3 origin;
		float nearPlane;
		D3DVIEWPORT9 viewport;
	};

	constexpr DWORD kScreenVertexFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
	constexpr UInt32 kMaxScreenVertices = (ThrowArcLogic::kMaxDashSegments + ThrowArcLogic::kRingSegments + 8) * 6;
	constexpr float kStepDt = 0.04f;
	constexpr float kDashLength = 13.0f;
	constexpr float kGapLength = 10.0f;
	constexpr float kTwoPi = 6.2831853f;

	using RenderSceneFn = int(__cdecl*)(void*, void*, UInt32);

	Detours::CallDetour s_renderSceneCall;
	std::atomic_flag s_rendering = ATOMIC_FLAG_INIT;
	PreviewConfig s_preview = {};
	ArcSnapshot s_snapshot = {};
	ScreenVertex s_vertices[kMaxScreenVertices];
	bool s_loggedStart = false;
	bool s_loggedDraw = false;

	void ResetPreview()
	{
		s_preview = {};
		s_snapshot.valid = false;
		s_snapshot.pointCount = 0;
		s_snapshot.hasHit = false;
	}

	Vec3 ReadWorldGravity(void* hkpBody)
	{
		if (hkpBody) {
			void* rawWorld = HkpWorldObjectAsView(hkpBody)->world;
			if (rawWorld) {
				auto* world = reinterpret_cast<HkpWorldView*>(rawWorld);
				Vec3 g = {
					world->gravity[0] * kInvHavokScale,
					world->gravity[1] * kInvHavokScale,
					world->gravity[2] * kInvHavokScale};
				if (ThrowArcLogic::IsFiniteVec3(g) && std::fabs(g.z) > 10.0f && std::fabs(g.z) < 5000.0f)
					return g;
			}
		}
		return {0.0f, 0.0f, ThrowArcLogic::kDefaultGravityZ};
	}

	float ReadLinearDamping(void* hkpBody, float overrideDamping)
	{
		if (overrideDamping >= 0.0f && std::isfinite(overrideDamping))
			return ThrowArcLogic::Clamp(overrideDamping, 0.0f, 5.0f);
		if (hkpBody) {
			const float d = HkpRigidBodyAsView(hkpBody)->linearDamping;
			if (std::isfinite(d) && d >= 0.0f && d <= 5.0f)
				return d;
		}
		return 0.05f;
	}

	void AppendScreenSegment(const CameraSnapshot& cam, const Vec3& worldA, const Vec3& worldB,
		float halfWidthPx, D3DCOLOR colorA, D3DCOLOR colorB, UInt32& written)
	{
		if (written + 6 > kMaxScreenVertices)
			return;

		const Vec4 av4 = ThrowArcLogic::Transform(ThrowArcLogic::Sub(worldA, cam.origin), cam.view);
		const Vec4 bv4 = ThrowArcLogic::Transform(ThrowArcLogic::Sub(worldB, cam.origin), cam.view);
		Vec3 a = {av4.x, av4.y, av4.z};
		Vec3 b = {bv4.x, bv4.y, bv4.z};
		if (!ThrowArcLogic::ClipNear(a, b, cam.nearPlane + 0.05f))
			return;

		const Vec4 ca = ThrowArcLogic::Transform(a, cam.projection);
		const Vec4 cb = ThrowArcLogic::Transform(b, cam.projection);
		if (ca.w <= 1e-4f || cb.w <= 1e-4f || !std::isfinite(ca.w) || !std::isfinite(cb.w))
			return;

		const float invWa = 1.0f / ca.w;
		const float invWb = 1.0f / cb.w;
		const float ndcAx = ca.x * invWa;
		const float ndcAy = ca.y * invWa;
		const float ndcAz = ca.z * invWa;
		const float ndcBx = cb.x * invWb;
		const float ndcBy = cb.y * invWb;
		const float ndcBz = cb.z * invWb;

		if ((ndcAx < -1.2f && ndcBx < -1.2f) || (ndcAx > 1.2f && ndcBx > 1.2f) || (ndcAy < -1.2f && ndcBy < -1.2f) || (ndcAy > 1.2f && ndcBy > 1.2f))
			return;

		const float vpX = static_cast<float>(cam.viewport.X);
		const float vpY = static_cast<float>(cam.viewport.Y);
		const float vpW = static_cast<float>(cam.viewport.Width);
		const float vpH = static_cast<float>(cam.viewport.Height);
		const float zSpan = cam.viewport.MaxZ - cam.viewport.MinZ;

		const float sxA = vpX + (ndcAx * 0.5f + 0.5f) * vpW - 0.5f;
		const float syA = vpY + (0.5f - ndcAy * 0.5f) * vpH - 0.5f;
		const float szA = ThrowArcLogic::Clamp(cam.viewport.MinZ + ndcAz * zSpan, 0.0f, 1.0f);

		const float sxB = vpX + (ndcBx * 0.5f + 0.5f) * vpW - 0.5f;
		const float syB = vpY + (0.5f - ndcBy * 0.5f) * vpH - 0.5f;
		const float szB = ThrowArcLogic::Clamp(cam.viewport.MinZ + ndcBz * zSpan, 0.0f, 1.0f);

		const float dx = sxB - sxA;
		const float dy = syB - syA;
		const float len = std::sqrt(dx * dx + dy * dy);
		if (!std::isfinite(len) || len < 0.25f)
			return;

		const float nx = (-dy / len) * halfWidthPx;
		const float ny = (dx / len) * halfWidthPx;

		const ScreenVertex v0 = {sxA - nx, syA - ny, szA, invWa, colorA};
		const ScreenVertex v1 = {sxA + nx, syA + ny, szA, invWa, colorA};
		const ScreenVertex v2 = {sxB - nx, syB - ny, szB, invWb, colorB};
		const ScreenVertex v3 = {sxB + nx, syB + ny, szB, invWb, colorB};

		s_vertices[written++] = v0;
		s_vertices[written++] = v1;
		s_vertices[written++] = v2;
		s_vertices[written++] = v2;
		s_vertices[written++] = v1;
		s_vertices[written++] = v3;
	}

	UInt32 BuildArcVertices(const CameraSnapshot& cam, const ArcSnapshot& snap)
	{
		UInt32 written = 0;
		const float power = ThrowArcLogic::ComputeThrowPower(snap.chargeRatio);
		const float baseAlpha = 0.58f + 0.30f * power;

		ThrowArcLogic::DashSegment dashes[ThrowArcLogic::kMaxDashSegments];
		const UInt32 dashCount = ThrowArcLogic::BuildDashedArc(
			snap.points, snap.pointCount, snap.hasHit,
			kDashLength, kGapLength, snap.dashPhase,
			dashes, ThrowArcLogic::kMaxDashSegments);

		const float dashHalfWidth = 1.45f + 0.45f * power;
		for (UInt32 i = 0; i < dashCount; ++i) {
			const auto& seg = dashes[i];
			const UInt8 aA = static_cast<UInt8>(ThrowArcLogic::Clamp(seg.alphaA * baseAlpha * 255.0f, 0.0f, 255.0f));
			const UInt8 aB = static_cast<UInt8>(ThrowArcLogic::Clamp(seg.alphaB * baseAlpha * 255.0f, 0.0f, 255.0f));
			if (aA < 4 && aB < 4)
				continue;
			const D3DCOLOR cA = ThrowArcLogic::MakeD3DColor(aA, snap.colorR, snap.colorG, snap.colorB);
			const D3DCOLOR cB = ThrowArcLogic::MakeD3DColor(aB, snap.colorR, snap.colorG, snap.colorB);
			AppendScreenSegment(cam, seg.a, seg.b, dashHalfWidth, cA, cB, written);
		}

		if (snap.hasHit) {
			Vec3 right = {};
			Vec3 forward = {};
			ThrowArcLogic::BuildSurfaceBasis(snap.hitNormal, right, forward);
			const Vec3 center = ThrowArcLogic::Add(snap.hitPos, ThrowArcLogic::Scale(snap.hitNormal, ThrowArcLogic::kSurfaceLift));
			const float ringRadius = 8.0f + 4.0f * power;
			const UInt8 ringAlpha = static_cast<UInt8>(ThrowArcLogic::Clamp((baseAlpha + 0.12f) * 255.0f, 0.0f, 255.0f));
			const D3DCOLOR ringColor = ThrowArcLogic::MakeD3DColor(ringAlpha, snap.colorR, snap.colorG, snap.colorB);

			auto ringPoint = [&](float angle, float radius) -> Vec3 {
				const float c = std::cos(angle) * radius;
				const float s = std::sin(angle) * radius;
				return {
					center.x + right.x * c + forward.x * s,
					center.y + right.y * c + forward.y * s,
					center.z + right.z * c + forward.z * s};
			};

			for (UInt32 i = 0; i < ThrowArcLogic::kRingSegments; ++i) {
				const float a0 = (kTwoPi * i) / ThrowArcLogic::kRingSegments;
				const float a1 = (kTwoPi * (i + 1)) / ThrowArcLogic::kRingSegments;
				AppendScreenSegment(cam, ringPoint(a0, ringRadius), ringPoint(a1, ringRadius), 1.55f, ringColor, ringColor, written);
			}

			const float dotRadius = 2.1f;
			for (UInt32 i = 0; i < 4; ++i) {
				const float a0 = (kTwoPi * i) / 4.0f;
				const float a1 = (kTwoPi * (i + 1)) / 4.0f;
				AppendScreenSegment(cam, ringPoint(a0, dotRadius), ringPoint(a1, dotRadius), 1.35f, ringColor, ringColor, written);
			}
		}

		return written;
	}

	void DrawArc(IDirect3DDevice9* device, const CameraSnapshot& cam, const ArcSnapshot& snap)
	{
		if (!device || !snap.valid || snap.pointCount < 2 || !cam.viewport.Width || !cam.viewport.Height)
			return;

		IDirect3DSurface9* depth = nullptr;
		if (FAILED(device->GetDepthStencilSurface(&depth)) || !depth)
			return;
		depth->Release();

		const UInt32 vertCount = BuildArcVertices(cam, snap);
		if (vertCount < 3)
			return;

		//temporary state block captures current state on creation and avoids holding COM objects across Device::Reset
		IDirect3DStateBlock9* stateBlock = nullptr;
		if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) || !stateBlock)
			return;

		const bool reversed = ThrowArcLogic::ReversedDepth(cam.projection);
		device->SetViewport(&cam.viewport);
		device->SetVertexShader(nullptr);
		device->SetPixelShader(nullptr);
		device->SetTexture(0, nullptr);
		device->SetFVF(kScreenVertexFVF);
		device->SetStreamSourceFreq(0, 1);

		device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
		device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

		device->SetRenderState(D3DRS_LIGHTING, FALSE);
		device->SetRenderState(D3DRS_FOGENABLE, FALSE);
		device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		device->SetRenderState(D3DRS_ZFUNC, reversed ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);
		device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		device->SetRenderState(D3DRS_DEPTHBIAS, 0);
		device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
		device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
		device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		device->SetRenderState(D3DRS_COLORWRITEENABLE,
			D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
		device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
		device->SetRenderState(D3DRS_COLORVERTEX, TRUE);

		const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, vertCount / 3, s_vertices, sizeof(ScreenVertex));
		stateBlock->Apply();
		stateBlock->Release();

		if (SUCCEEDED(hr) && !s_loggedDraw) {
			s_loggedDraw = true;
			Log("ThrowArcPreview: first draw %ux%u reversed=%d verts=%u rgb=(%u,%u,%u) hit=%d",
				cam.viewport.Width, cam.viewport.Height, reversed ? 1 : 0, vertCount,
				snap.colorR, snap.colorG, snap.colorB, snap.hasHit ? 1 : 0);
		}
	}

	int __cdecl Hook_RenderScene(void* camera, void* accumulator, UInt32 flags)
	{
		auto original = reinterpret_cast<RenderSceneFn>(s_renderSceneCall.GetOverwrittenAddr());
		if (!s_snapshot.valid || s_rendering.test_and_set())
			return original(camera, accumulator, flags);

		CameraSnapshot cam = {};
		IDirect3DDevice9* device = nullptr;
		NiDX9RendererView* renderer = GetNiDX9Renderer();
		if (camera && renderer && !renderer->inactive && renderer->device) {
			NiDX9RendererSetCameraData(renderer, camera);
			std::memcpy(cam.view, renderer->view, sizeof(cam.view));
			std::memcpy(cam.projection, renderer->projection, sizeof(cam.projection));
			cam.origin = {g_renderOriginPtr[0], g_renderOriginPtr[1], g_renderOriginPtr[2]};
			cam.nearPlane = renderer->nearPlane;
			device = renderer->device;
			device->AddRef();
			if (FAILED(device->GetViewport(&cam.viewport))) {
				device->Release();
				device = nullptr;
			}
		}

		const int result = original(camera, accumulator, flags);

		if (device) {
			NiDX9RendererView* live = GetNiDX9Renderer();
			if (s_snapshot.valid && live && !live->inactive && live->device == device)
				DrawArc(device, cam, s_snapshot);
			device->Release();
		}

		s_rendering.clear();
		return result;
	}

	static ParamInfo kParams_StartThrowArcPreview[7] = {
		{"minForce", kParamType_Float, 0},
		{"maxForce", kParamType_Float, 0},
		{"chargeTime", kParamType_Float, 0},
		{"tapGrace", kParamType_Float, 1},
		{"upBoostMin", kParamType_Float, 1},
		{"upBoostMax", kParamType_Float, 1},
		{"linearDamping", kParamType_Float, 1},
	};
}

DEFINE_COMMAND_PLUGIN(StartThrowArcPreview, "starts rendering a live 3D ballistic throw arc for the calling grabbed reference", 1, 7, kParams_StartThrowArcPreview);
DEFINE_COMMAND_PLUGIN(StopThrowArcPreview, "stops rendering the live 3D throw arc preview", 0, 0, nullptr);

bool Cmd_StartThrowArcPreview_Execute(COMMAND_ARGS)
{
	*result = 0;
	float minForce = 200.0f;
	float maxForce = 1400.0f;
	float chargeTime = 1.2f;
	float tapGrace = 0.10f;
	float upBoostMin = 55.0f;
	float upBoostMax = 200.0f;
	float linearDamping = -1.0f;

	if (!ExtractArgs(EXTRACT_ARGS, &minForce, &maxForce, &chargeTime, &tapGrace, &upBoostMin, &upBoostMax, &linearDamping))
		return true;

	PlayerCharacter* player = *g_thePlayerPtr;
	if (!player || !thisObj || !thisObj->IsReference() || thisObj == player || g_isLoadingSave || IsGameLoading()) {
		ResetPreview();
		return true;
	}

	const UInt32 now = GetTickCount();
	s_preview.refID = thisObj->refID;
	s_preview.startTick = now;
	s_preview.lastTick = now;
	s_preview.minForce = ThrowArcLogic::Clamp(minForce, 10.0f, 10000.0f);
	s_preview.maxForce = ThrowArcLogic::Clamp(maxForce, s_preview.minForce, 10000.0f);
	s_preview.chargeTime = ThrowArcLogic::Clamp(chargeTime, 0.15f, 10.0f);
	s_preview.tapGrace = ThrowArcLogic::Clamp(tapGrace, 0.0f, 1.0f);
	s_preview.upBoostMin = ThrowArcLogic::Clamp(upBoostMin, -500.0f, 2000.0f);
	s_preview.upBoostMax = ThrowArcLogic::Clamp(upBoostMax, -500.0f, 2000.0f);
	s_preview.linearDamping = linearDamping;
	s_preview.dashPhase = 0.0f;
	s_preview.active = true;

	*result = 1;
	ThrowArcPreview::Update();
	return true;
}

bool Cmd_StopThrowArcPreview_Execute(COMMAND_ARGS)
{
	ResetPreview();
	*result = 1;
	return true;
}

namespace ThrowArcPreview {
	void RegisterCommands(void* nvsePtr)
	{
		auto* nvse = static_cast<NVSEInterface*>(nvsePtr);
		nvse->RegisterCommand(&kCommandInfo_StartThrowArcPreview);
		nvse->RegisterCommand(&kCommandInfo_StopThrowArcPreview);
	}

	void Init()
	{
		s_renderSceneCall.WriteRelCall(0x8741DB, Hook_RenderScene);
	}

	void Update()
	{
		if (!s_preview.active) {
			s_snapshot.valid = false;
			return;
		}

		PlayerCharacter* player = *g_thePlayerPtr;
		if (!player || g_isLoadingSave || IsGameLoading() || CdeclCall<bool>(0x702360) || ::IsVATSActive() || Engine::Actor_IsDead(player, false)) {
			ResetPreview();
			return;
		}

		TESObjectREFR* ref = LookupRefByID(s_preview.refID);
		PlayerGrabView* grab = PlayerCharacterGetGrabView(player);
		if (!ref || Engine::TESForm_GetDeleted(ref) || Engine::TESForm_GetDisabled(ref) || !grab || grab->grabbedRef != ref) {
			ResetPreview();
			return;
		}

		NiNode* root = ref->GetNiNode();
		void* bhkBody = root ? Engine::GetHeaviestRigidBody(root) : nullptr;
		void* hkpBody = BhkWorldObjectGetHavokObject(bhkBody);

		Vec3 startPos = {ref->posX, ref->posY, ref->posZ};
		if (root) {
			const float* wt = NiAVObjectAsView(root)->world.translate;
			const Vec3 nodePos = {wt[0], wt[1], wt[2]};
			if (ThrowArcLogic::IsFiniteVec3(nodePos))
				startPos = nodePos;
		}
		if (!ThrowArcLogic::IsFiniteVec3(startPos)) {
			s_snapshot.valid = false;
			return;
		}

		const UInt32 now = GetTickCount();
		const float elapsedSec = static_cast<float>(now - s_preview.startTick) * 0.001f;
		const float dtReal = ThrowArcLogic::Clamp(static_cast<float>(now - s_preview.lastTick) * 0.001f, 0.0f, 0.1f);
		s_preview.lastTick = now;

		const float chargeRatio = ThrowArcLogic::ComputeChargeRatio(elapsedSec, s_preview.chargeTime, s_preview.tapGrace);
		Vec3 vel = ThrowArcLogic::ComputeInitialVelocity(
			player->rotZ, player->rotX,
			s_preview.minForce, s_preview.maxForce,
			s_preview.upBoostMin, s_preview.upBoostMax,
			chargeRatio);
		const float speed = ThrowArcLogic::Length(vel);
		const Vec3 gravity = ReadWorldGravity(hkpBody);
		const float damping = ReadLinearDamping(hkpBody, s_preview.linearDamping);

		s_preview.dashPhase += (35.0f + speed * 0.08f) * dtReal;

		UInt8 colorR = 255;
		UInt8 colorG = 182;
		UInt8 colorB = 66;
		ThrowArcLogic::UnpackHUDColor(GetSystemHUDColorRGBA(), colorR, colorG, colorB);

		if (!s_loggedStart) {
			s_loggedStart = true;
			Log("ThrowArcPreview: start ref=%08X min=%.0f max=%.0f grav=(%.1f,%.1f,%.1f) damp=%.3f hud=(%u,%u,%u)",
				s_preview.refID, s_preview.minForce, s_preview.maxForce,
				gravity.x, gravity.y, gravity.z, damping, colorR, colorG, colorB);
		}

		const UInt16 playerGroup = ActorGetCollisionGroup(player);
		ArcSnapshot next = {};
		next.points[0] = startPos;
		next.pointCount = 1;
		next.chargeRatio = chargeRatio;
		next.speed = speed;
		next.dashPhase = s_preview.dashPhase;
		next.colorR = colorR;
		next.colorG = colorG;
		next.colorB = colorB;

		Vec3 pos = startPos;
		for (UInt32 step = 1; step < ThrowArcLogic::kMaxArcPoints; ++step) {
			const Vec3 prevPos = pos;
			ThrowArcLogic::StepBallistic(pos, vel, gravity, damping, kStepDt);

			RayCastData ray = {};
			ray.pos0[0] = prevPos.x * kHavokScale;
			ray.pos0[1] = prevPos.y * kHavokScale;
			ray.pos0[2] = prevPos.z * kHavokScale;
			ray.pos1[0] = pos.x * kHavokScale;
			ray.pos1[1] = pos.y * kHavokScale;
			ray.pos1[2] = pos.z * kHavokScale;
			ray.hitFraction = 1.0f;
			ray.unk44[0] = 0xFFFFFFFF;
			ray.unk44[6] = 0xFFFFFFFF;
			ray.layerType = kRayLayer_Projectile;
			ray.group = playerGroup;

			bool hitSolid = false;
			if (Engine::TESPickObject(&ray, true) && !ray.budgetSpent && ray.cdBody && std::isfinite(ray.hitFraction) && ray.hitFraction >= 0.0f && ray.hitFraction < 1.0f) {
				if (!hkpBody || HkpWorldObjectFromCollidableRoot(ray.cdBody) != hkpBody)
					hitSolid = true;
			}

			if (hitSolid) {
				const Vec3 hitPos = ThrowArcLogic::Lerp(prevPos, pos, ray.hitFraction);
				Vec3 hitNormal = {ray.normal[0], ray.normal[1], ray.normal[2]};
				if (!ThrowArcLogic::Normalize(hitNormal))
					hitNormal = {0.0f, 0.0f, 1.0f};
				next.points[next.pointCount++] = hitPos;
				next.hitPos = hitPos;
				next.hitNormal = hitNormal;
				next.hasHit = true;
				break;
			}

			next.points[next.pointCount++] = pos;
		}

		next.valid = next.pointCount >= 2;
		s_snapshot = next;
	}

	void ClearState()
	{
		ResetPreview();
	}
}
