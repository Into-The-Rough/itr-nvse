# OnWitnessed / GetWitnesses — Engine Verification

All addresses verified in IDA against `C:\Games\GOG\Fallout New Vegas\FalloutNV.exe` (FNV 1.4.0.525).
Do not ship code that references anything on this page without re-checking against the
current IDB — memory is not a source of truth.

Mental hex/decimal conversion is forbidden. Use `echo "obase=16; N" | bc` (or the reverse).

## Engine plumbing overview

Five dedicated alarm functions on `Actor`, one per crime type:

| Crime        | Function                | Address    | Sig (__thiscall)                                                                 | retN |
|--------------|-------------------------|------------|----------------------------------------------------------------------------------|------|
| Steal        | `Actor::StealAlarm`     | `0x8BFA40` | `(Actor* toDetect, Actor* reporter, TESForm* item, int qty, TESForm* a4, TESForm* owner)` | `14h` |
| Pickpocket   | `Actor::PickpocketAlarm`| `0x8C00E0` | `(Actor* perp, Actor* victim, TESForm* form, UInt32 data)` (bail if `this != player`)     | `0Ch` |
| Attack       | `Actor::AttackAlarm`    | `0x8C0460` | `(Actor* victim, Actor* attacker, bool abMinorCrime, ?)`                                  | `0Ch` |
| Murder       | `Actor::MurderAlarm`    | `0x8C09E0` | `(Actor* victim, Actor* killer)`                                                          | `04h` |
| Trespass     | `Actor::TrespassAlarm`  | `0x8C0EC0` | `(Actor* this, TESObjectREFR* apRef, TESForm* apOwnership, ?)`                            | `0Ch` |

The first four funnel into the `Crime` system. **Trespass does not** — it iterates
nearby actors directly via `TESDataHandler::GetFactions` + `Actor::GetDetectionValue`
and never allocates a `Crime` object.

### Steal / Pickpocket / Attack / Murder flow

```
Actor::*Alarm(...)
  Crime::Crime(eType, apTarget, apActor, apObject, aiNumber, apOwnership)   // heap alloc
    [configures Crime fields]
  ProcessLists::SendCrimetoHighList(crime)       // returns BSSimpleList* of candidates
  for each candidate actor in list:
    if actor sees perp (via vtable detection check):
      Crime::AddtoActorKnowList(crime, witness)  // dedupes into Crime.kWitnesses
      [vtable dispatch: react / combat / pursue / dialogue topic]
  Crime::HasWitnesses(crime)                      // if > 0, apply faction rep consequences
```

### Trespass flow

```
Actor::TrespassAlarm(...)
  BSSpinLock::Lock(ProcessLists::kNearbyActorLock)
  actors = TESDataHandler::GetFactions()
  count  = MissileProjectile::GetImpactResult()       // actual nearby-actor count getter
  for i in 0..count:
    det = Actor::GetDetectionValue(actors[i], player)
    if det > 0 && ref is owned by actor/faction:
      [vtable dispatch: react]
      Actor::HandleMinorCrimeFactionReputations(actor, 1, 1)
  BSSpinLock::Unlock(ProcessLists::kNearbyActorLock)
```

## Crime struct — 60 bytes (source: `get_struct("Crime")`)

```
offset  field              type
+0x00   uiWitnessCount     DWORD           // incremented by AddtoActorKnowList
+0x04   eCrimeType         DWORD           // see table below
+0x08   pCrimeTarget       TESObjectREFR*  // victim / stolen ref
+0x0C   pCriminal          Actor*          // perpetrator
+0x10   bReported          bool
+0x14   pStolenObject      TESBoundObject* // stolen form (steal/pickpocket only)
+0x18   uiNumberStolen     UInt32
+0x1C   kWitnesses         BSSimpleList*void (8 bytes)
+0x24   pOwnership         TESForm*
+0x28   uiCrimeNumber      DWORD
+0x2C   dword2C            DWORD
+0x30   uiCrimeStamp       UInt32
+0x34   kCrimeTimeStamp    AITimeStamp     (4 bytes)
+0x38   dword38            DWORD
// size = 0x3C (60)
```

### Crime::Crime constructor `0x9EB500`

`retn 18h` = 6 args after `this`:
```
arg0  aeCrime       int          -> +0x04
arg1  apTarget      Actor*       -> +0x08   (calls SetTargeted if non-null)
arg2  apActor       Actor*       -> +0x0C
arg3  apObject      TESForm*     -> +0x14
arg4  aiNumber      int          -> +0x18
arg5  apOwnership   TESForm*     -> +0x24
```

### Crime type constants — read from actual call sites

| Symbolic              | Value | Source                                       |
|-----------------------|-------|----------------------------------------------|
| `CRIME_STEAL`         | `0`   | `StealAlarm` `0x8BFCFE` push 0 -> `Crime::Crime` |
| `CRIME_PICKPOCKET`    | `1`   | `PickpocketAlarm` `0x8C021B` push 1              |
| `CRIME_ATTACK`        | `3`   | `AttackAlarm` `0x8C054F` push 3                  |
| `CRIME_MURDER`        | `4`   | `MurderAlarm` `0x8C0B12` push 4                  |
| `CRIME_TRESPASS` (syn)| `2`   | synthetic — engine has no `Crime` for trespass   |

Trespass gets value `2` because it does not pass through `Crime::Crime` at all. This is an itr-nvse-side label, not an engine value. If future investigation finds the engine using `2` for something else, pick a different vacant slot and update.

## `Crime::AddtoActorKnowList` — the hook target

- **Address:** `0x9EB9C0`
- **Size:** 63 bytes (`0x9EB9C0` → `0x9EB9FF`)
- **Signature:** `void __thiscall(Crime* this, Actor* witness)`
- **Behaviour:** `BSSimpleList::IsInList` check on `this+0x1C`; if absent, `AddHead` and
  `++this->uiWitnessCount`. Dedupes automatically.
- `retn 4`.

### Prologue bytes (for ValidateBytes)

```
0x9EB9C0  55          push ebp                (1)
0x9EB9C1  8B EC       mov ebp, esp            (2)
0x9EB9C3  51          push ecx                (1)
0x9EB9C4  89 4D FC    mov [ebp-4], ecx        (3)    ← first clean boundary ≥5 is 7
0x9EB9C7  8D 45 08    lea eax, [ebp+8]        (3)
```

Expected byte signature: `55 8B EC 51 89 4D FC` (7 bytes).
Pass `size = 7` to `Detours::JumpDetour::WriteRelJump`.

### All `AddtoActorKnowList` callers (verified)

| Caller                          | Address     | Note                                 |
|---------------------------------|-------------|--------------------------------------|
| `ExtraDataList::InitLoadGameBGS`| `0x42D979`  | **save-load rebuild — MUST SUPPRESS**|
| `Actor::InitiateAlarm`          | `0x8BF45E`  | upstream dispatcher                  |
| `Actor::StealAlarm`             | `0x8BFDCA`  | per witness in steal iteration       |
| `Actor::PickpocketAlarm`        | `0x8C034C`  | per witness in pickpocket iteration  |
| `Actor::AttackAlarm`            | `0x8C05DE`, `0x8C072E` | per witness in attack loop |
| `Actor::MurderAlarm`            | `0x8C0B80`, `0x8C0C1D`, `0x8C0CA0` | per witness in murder loop |

(Trespass notably absent — see dedicated section below.)

## Save-load false positive — MUST HANDLE

`ExtraDataList::InitLoadGameBGS` calls `AddtoActorKnowList` during save load to
rebuild each restored `Crime`'s witness list. Without a guard, `ITR:OnWitnessed`
will fire a flood of false events the moment a save is loaded.

itr-nvse current state (`ITR.cpp:337-338`):
- `kMessage_PostLoadGame` is handled (for music reset)
- `kMessage_PreLoadGame` is NOT handled
- No `g_isLoadingSave` / equivalent flag exists

Plan:
- Add `bool g_isLoadingSave` in `internal/globals.h`
- Set `true` on `kMessage_PreLoadGame`
- Clear on `kMessage_PostLoadGame`
- Hook wrapper returns early without dispatching while `g_isLoadingSave == true`

The original `AddtoActorKnowList` must still run during save load — only the event
dispatch is suppressed.

## Actor / BaseProcess / Cell offsets (from `get_struct`)

### `Actor` struct (partial, size 436)

Actor is `MobileObject + Magic* + ActorValue* + ...`. The `baseProcess` field lives
inside the embedded `MobileObject`, verified at `Actor+0x68` from `Actor::GetDetectionValue`
decomp:

```
0x8a823c  cmp dword ptr [eax+68h], 0   // Actor*->baseProcess
```

Other confirmed `Actor` field offsets used by this feature:

| Offset | Field                  | Type              | Purpose                              |
|--------|------------------------|-------------------|--------------------------------------|
| +0x68  | `baseProcess`          | `BaseProcess*`    | Gate for `GetDetectionData`          |
| +0xC0  | `pKiller`              | `Actor*`          | (informational)                      |
| +0xC4  | `bMurderAlarm`         | `bool`            | (informational)                      |
| +0x13C | `uiMinorCrimeCount`    | `UInt32`          | (informational)                      |
| +0x144 | `bIgnoreCrime`         | `bool`            | Honour this when scanning witnesses  |
| +0x18D | `bIsTeammate`          | `bool`            | **Skip teammates from witness list** |
| +0x190 | `pActorMover`          | `ActorMover*`     | (unrelated)                          |

### `BaseProcess` struct (size 48, header-only — real class is larger via vtable)

| Offset | Field              | Type                           |
|--------|--------------------|--------------------------------|
| +0x00  | `__vftable`        | `BaseProcess_vtbl*`            |
| +0x28  | `uiProcessLevel`   | `ProcessLists::ProcessLevel`   |

The virtual we care about is `GetDetectionData(Actor* target, int index)` at vtable
byte-offset `0x504` (from `Actor::GetDetectionValue` at `0x8a825d`: `mov eax, [edx+504h]`).

**Don't call the virtual directly.** Use `Actor::GetDetectionValue` (`0x8A8230`) as a
typed `thiscall` wrapper — it already handles null `baseProcess`, null `DetectionData`,
and returns `-100` for "not detected". Signature:
```cpp
SInt32 __thiscall Actor::GetDetectionValue(Actor* this, Actor* target);
```

### `TESObjectCELL` struct (size 224)

| Offset | Field            | Type                  | Purpose                                |
|--------|------------------|-----------------------|----------------------------------------|
| +0x80  | `kSpinLock`      | `BSSpinLock` (32)     | **Take when iterating kReferences**    |
| +0xAC  | `kReferences`    | `BSSimpleList*void` (8) | Ref iteration list for `GetWitnesses` |

## `DetectionData` struct (inferred from `DetectionData::CopyFrom` at `0x8D6FC0`)

Not a named type in the IDB. Layout reverse-engineered from the copy routine and
matches what `WitnessSystemNVSE` ships with.

```cpp
struct DetectionData {
    Actor*   actor;              // +0x00
    UInt32   detectionState;     // +0x04  (packed byte flags)
    SInt32   detectionValue;     // +0x08  ← the number to threshold on
    NiPoint3 location;           // +0x0C  (12 bytes)
    float    fTimestamp;         // +0x18  (-1.0f sentinel = location invalid)
    UInt8    bForceResetLOSBuf;  // +0x1C
    UInt8    byte1D;             // +0x1D
    bool     inLOS;              // +0x1E
    UInt8    byte1F;             // +0x1F
    SInt32   detectionModSneak;  // +0x20
};  // size = 0x24 (36)
```

Lift this into `internal/EngineFunctions.h` under a namespace or class alias. Don't
put `struct DetectionData` at global scope — if any header elsewhere drags in a
future official definition the duplicate-type error will be painful.

## Hook plan

### Primary: `Crime::AddtoActorKnowList` prologue hook

Covers steal, pickpocket, attack, murder — all four of the alarm paths that route
through the `Crime` object.

```cpp
// pseudocode — not final
namespace OnWitnessedHandler {
    Detours::JumpDetour g_addKnowHook;

    using AddKnow_t = void (__thiscall*)(Crime*, Actor*);
    AddKnow_t Original_AddKnow = nullptr;

    void __fastcall Hook_AddKnow(Crime* crime, void* /*edx*/, Actor* witness)
    {
        if (!g_isLoadingSave && crime && witness && g_eventManagerInterface) {
            int             type     = crime->eCrimeType;
            Actor*          criminal = crime->pCriminal;
            TESObjectREFR*  target   = crime->pCrimeTarget;

            SInt32 detVal = ThisStdCall<SInt32>(0x8A8230, witness, criminal);

            g_eventManagerInterface->DispatchEvent("ITR:OnWitnessed",
                /*thisObj*/ reinterpret_cast<TESObjectREFR*>(witness),
                witness, criminal, type, target, detVal);
        }

        Original_AddKnow(crime, witness);
    }

    bool Install()
    {
        UInt8* tramp = nullptr;
        if (!g_addKnowHook.WriteRelJump(0x9EB9C0,
                                        reinterpret_cast<UInt32>(&Hook_AddKnow),
                                        7, &tramp))
            return false;
        Original_AddKnow = reinterpret_cast<AddKnow_t>(tramp);
        return true;
    }
}
```

### Secondary: `Actor::TrespassAlarm` prologue hook

Trespass bypasses `Crime::*` entirely. Cover it with a dedicated hook at the entry
of `TrespassAlarm` (`0x8C0EC0`). At entry:

1. Take `this` (victim / trespass-target actor) + args
2. Run our own cell-based witness scan (same helper used by `GetWitnesses` command)
3. For each scanned witness with `detectionValue >= threshold`, fire
   `ITR:OnWitnessed` with `crimeType = CRIME_TRESPASS (2)`
4. Return to original `TrespassAlarm`

Prologue:
```
0x8c0ec0  55           push ebp
0x8c0ec1  8b ec        mov ebp, esp
0x8c0ec3  83 ec 24     sub esp, 24h
0x8c0ec6  89 4d e0     mov [ebp-20h], ecx
```
First clean boundary ≥5 = **9 bytes** (`55 8B EC 83 EC 24 89 4D E0`). Pass
`size = 9` to `WriteRelJump`.

Signature (partial — retn 0Ch = 3 args after this):
```cpp
void __thiscall Actor::TrespassAlarm(
    Actor* this,              // the trespass-target actor (NPC)
    TESObjectREFR* apRef,     // trespassed ref
    TESForm* apOwnership,     // owning faction/NPC
    /* arg3 unresolved */);
```

Witness semantics for trespass: the player (always the perpetrator on this path —
the first ProcessLists branch hard-codes `PlayerCharacter::pSingleton` as the
detection target) is visible to some nearby faction-guard actor. Our hook fires
`OnWitnessed` per matching actor with `perpetrator = player, victim = apRef`.

### `GetWitnesses` command (poll-based)

Independent of the hooks. Uses the shared witness-scan helper. Signature:
```
(actor:ref).GetWitnesses [radius:float] [detectionThreshold:int] [x:float] [y:float] [z:float]
  -> array of actor refs
```

Defaults: `radius = 2048.0`, `threshold = 25`, location = perpetrator pos. Ignores
teammates and dead actors. Iterates `parentCell->kReferences` under the cell's
`kSpinLock`. Calls `Actor::GetDetectionValue(candidate, perpetrator)` via the
`0x8A8230` wrapper.

Bonus command `GetWitnessDetectionLevel <observer> <target> -> int` — thin
wrapper around the same call, returns raw `DetectionData.detectionValue` (range
`-100` .. `500`).

## Event: `ITR:OnWitnessed`

```cpp
using P = NVSEEventManagerInterface::ParamType;
static P witnessedParams[] = {
    P::eParamType_AnyForm,  // 0: witness       (filterable: ref / formlist / faction)
    P::eParamType_AnyForm,  // 1: perpetrator   (filterable)
    P::eParamType_Int,      // 2: crimeType     (filterable equality)
    P::eParamType_AnyForm,  // 3: victim/target (filterable)
    P::eParamType_Int,      // 4: detectionValue (payload only)
};
g_eventManagerInterface->RegisterEvent("ITR:OnWitnessed", 5, witnessedParams,
                                       F::kFlag_FlushOnLoad);
```

`thisObj` passed to `DispatchEvent` is **the witness**. That's the most useful
narrowing for quest/AI scripts that care about a specific NPC. Modders filter
with `SetEventHandler "ITR:OnWitnessed" fn 1::player` etc.; no filter code lives
in itr-nvse — xNVSE handles it.

## Crime type values exposed to modders

Document in `FEATURES.md` and `wiki_pages/`:
```
CRIME_STEAL      = 0
CRIME_PICKPOCKET = 1
CRIME_TRESPASS   = 2   (itr-nvse synthetic)
CRIME_ATTACK     = 3
CRIME_MURDER     = 4
```

## Opcodes

Assigned from the `0x4088-0x40FF` unused ITR extended block (per `/mnt/d/plugins/opcodes.txt`):

| Opcode  | Command                       |
|---------|-------------------------------|
| `0x408D`| `GetWitnesses`                |
| `0x408E`| `GetWitnessDetectionLevel`    |

## Files to be added / modified

### New
```
itr-nvse/commands/WitnessCommands.cpp
itr-nvse/commands/WitnessCommands.h
itr-nvse/handlers/OnWitnessedHandler.cpp
itr-nvse/handlers/OnWitnessedHandler.h
itr-nvse/internal/WitnessScan.cpp
itr-nvse/internal/WitnessScan.h
```

### Modified
```
itr-nvse/internal/EngineFunctions.h    (+ DetectionData layout)
itr-nvse/internal/globals.h            (+ g_isLoadingSave flag)
itr-nvse/internal/EventDispatch.cpp    (+ ITR:OnWitnessed registration)
itr-nvse/commands/CommandTable.cpp     (+ opcode wiring)
itr-nvse/ITR.cpp                       (+ PreLoadGame handler, OnWitnessedHandler::Init)
itr-nvse/itr-nvse.ini                  (+ [OnWitnessed] section)
itr-nvse/FEATURES.md                   (+ new command + event docs)
itr-nvse/docs/HOOK_REGISTRY.md         (+ 0x9EB9C0, 0x8C0EC0)
```

## Open items before coding

- [ ] Verify `kMessage_PreLoadGame` symbol spelling in current xNVSE header (may be
      `kMessage_PreLoadGame` or `kMessage_PreLoad`).
- [ ] Confirm that firing a disposable ref through `DispatchEvent` as `thisObj`
      (first arg cast) doesn't break xNVSE's reflike handling when witness is an
      `Actor*` not a `TESObjectREFR*`. `Actor` inherits `TESObjectREFR` so the
      cast is valid.
- [ ] Confirm `Detours::JumpDetour` trampoline lives long enough — the `JumpDetour`
      instance must be a static/global that outlives game runtime. Standard itr-nvse
      pattern (see `OnStealHandler`).
- [ ] Double-check `Actor::GetDetectionValue` works correctly when called on a
      target actor that has `baseProcess == null` — disasm shows it returns `-100`
      which the threshold check filters out; benign.

## Provenance — all addresses verified against FNV 1.4.0.525

| Address     | Symbol                               | Verified via                           |
|-------------|--------------------------------------|----------------------------------------|
| `0x8A8230`  | `Actor::GetDetectionValue`           | `get_func_bounds`, `get_func_disasm`   |
| `0x8BED50`  | `Actor::InitiateAlarm`               | `get_func_bounds`                      |
| `0x8BFA40`  | `Actor::StealAlarm`                  | `get_func_bounds`, `get_func_disasm`   |
| `0x8C00E0`  | `Actor::PickpocketAlarm`             | `get_func_bounds`, `get_func_disasm`   |
| `0x8C0460`  | `Actor::AttackAlarm`                 | `get_func_bounds`, `get_func_disasm`   |
| `0x8C09E0`  | `Actor::MurderAlarm`                 | `get_func_bounds`, `get_func_disasm`   |
| `0x8C0EC0`  | `Actor::TrespassAlarm`               | `get_func_bounds`, `get_func_disasm`   |
| `0x8D6FC0`  | `DetectionData::CopyFrom`            | `get_func_disasm`                      |
| `0x9EB500`  | `Crime::Crime` (ctor)                | `get_func_disasm`                      |
| `0x9EB9C0`  | `Crime::AddtoActorKnowList`          | `get_func_disasm`, `get_bytes`         |
| `0x9EBAB0`  | `Crime::HasWitnesses`                | `get_func_disasm`                      |
| `+0x68`     | `Actor::baseProcess`                 | inferred from `GetDetectionValue` disasm |
| `+0x504`    | `BaseProcess::vtbl[GetDetectionData]`| inferred from `GetDetectionValue` disasm |
| `+0x08`     | `DetectionData::detectionValue`      | inferred from `GetDetectionValue` + `CopyFrom` |
| `+0x1C`     | `Crime::kWitnesses`                  | `get_struct("Crime")`                  |
| `+0x04`     | `Crime::eCrimeType`                  | `get_struct("Crime")`                  |
| `+0x0C`     | `Crime::pCriminal`                   | `get_struct("Crime")`                  |
| `+0x08`     | `Crime::pCrimeTarget`                | `get_struct("Crime")`                  |
| `+0xAC`     | `TESObjectCELL::kReferences`         | `get_struct("TESObjectCELL")`          |
| `+0x80`     | `TESObjectCELL::kSpinLock`           | `get_struct("TESObjectCELL")`          |
| `+0x18D`    | `Actor::bIsTeammate`                 | `get_struct("Actor")`                  |

Re-verify anything on this table against the current IDB before referencing it in
production code. This doc is a snapshot.
