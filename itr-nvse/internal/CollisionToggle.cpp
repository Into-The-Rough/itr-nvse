#include "CollisionToggle.h"

#include "common/ITypes.h"
#include "nvse/GameObjects.h"

namespace
{
	constexpr UInt8 kRefFlag_DisableCollision = 1 << 1;

#define ITR_ADDR_RETURN_TRUE 0x8D0360
#define ITR_ADDR_RETURN_THIS 0x6815C0

	__declspec(naked) void __fastcall NiNodeToggleCollision(void* /*node*/, UInt8 /*flag*/)
	{
		__asm
		{
			mov		eax, [ecx+0x1C]
			test	eax, eax
			jz		noColObj
			mov		eax, [eax+0x10]
			test	eax, eax
			jz		noColObj
			push	ecx
			mov		ecx, [eax+8]
			and     byte ptr [ecx+0x2D], 0xBF
			or      [ecx+0x2D], dl
			push	edx
			mov		ecx, eax
			mov		eax, [ecx]
			call	dword ptr [eax+0xC4]
			pop		edx
			pop		ecx
		noColObj:
			movzx	eax, word ptr [ecx+0xA6]
			test	eax, eax
			jz		done
			push	esi
			push	edi
			mov		esi, [ecx+0xA0]
			mov		edi, eax
		iterHead:
			dec		edi
			js		iterEnd
			mov		ecx, [esi]
			add		esi, 4
			test	ecx, ecx
			jz		iterHead
			mov		eax, [ecx]
			cmp		dword ptr [eax+0xC], ITR_ADDR_RETURN_THIS
			jnz		iterHead
			call	NiNodeToggleCollision
			jmp		iterHead
		iterEnd:
			pop		edi
			pop		esi
		done:
			retn
		}
	}

	__declspec(naked) void __fastcall ReferenceToggleCollision(TESObjectREFR* /*ref*/, bool /*enabled*/)
	{
		__asm
		{
			dec		dl
			and		dl, 0x40
			mov		eax, [ecx]
			cmp		dword ptr [eax+0x100], ITR_ADDR_RETURN_TRUE
			jnz		notActor
			mov		eax, [ecx+0x68]
			test	eax, eax
			jz		notActor
			cmp		byte ptr [eax+0x28], 1
			ja		notActor
			mov		eax, [eax+0x138]
			test	eax, eax
			jz		notActor
			mov		eax, [eax+0x594]
			test	eax, eax
			jz		notActor
			mov		eax, [eax+8]
			test	eax, eax
			jz		notActor
			and     byte ptr [eax+0x2D], 0xBF
			or      [eax+0x2D], dl
		notActor:
			mov		eax, [ecx+0x64]
			test	eax, eax
			jz		done
			mov		ecx, [eax+0x14]
			test	ecx, ecx
			jz		done
			jmp		NiNodeToggleCollision
		done:
			retn
		}
	}

#undef ITR_ADDR_RETURN_TRUE
#undef ITR_ADDR_RETURN_THIS
}

namespace CollisionToggle
{
	bool IsDisabled(TESObjectREFR* ref)
	{
		return ref && (*reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(ref) + 0x5F) & kRefFlag_DisableCollision) != 0;
	}

	void SetEnabled(TESObjectREFR* ref, bool enabled)
	{
		if (!ref)
			return;

		const bool isEnabled = !IsDisabled(ref);
		if (isEnabled != enabled)
			*reinterpret_cast<UInt8*>(reinterpret_cast<UInt8*>(ref) + 0x5F) ^= kRefFlag_DisableCollision;

		ReferenceToggleCollision(ref, enabled);
	}
}
