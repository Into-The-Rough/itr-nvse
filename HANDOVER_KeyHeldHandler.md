# KeyHeld Handler - Implementation Plan

## Goal
Create a KeyHeld event system for NVSE. KeyDown fires once, KeyHeld fires continuously while held past a threshold.

## Files to Create
- `/mnt/d/plugins/itr-nvse/itr-nvse/KeyHeldHandler.h`
- `/mnt/d/plugins/itr-nvse/itr-nvse/KeyHeldHandler.cpp`

## Data Structures

```cpp
struct KeyHeldHandler {
    UInt32 id;
    UInt32 key;              // DIK scancode or control code
    float holdThreshold;     // Seconds before held events start
    float repeatInterval;    // Seconds between dispatches (0 = every frame)
    bool useControlCode;     // true = game control, false = raw key
    Script* callback;        // NVSE callback script
};

struct KeyState {
    bool isDown;
    float downTime;          // When key went down (game time or real time)
    float lastDispatch;      // When we last fired held event
};
```

## NVSE Commands to Register

| Command | Params | Returns | Opcode |
|---------|--------|---------|--------|
| `RegisterKeyHeld` | keycode:int, threshold:float, interval:float, callback:ref | handlerID:int | 0x3B0B |
| `RegisterControlHeld` | controlCode:int, threshold:float, interval:float, callback:ref | handlerID:int | 0x3B0C |
| `UnregisterKeyHeld` | handlerID:int | success:bool | 0x3B0D |
| `UnregisterControlHeld` | handlerID:int | success:bool | 0x3B0E |

## Callback Signature
```
begin function {int keycode, float duration}
    ; keycode = the key or control code
    ; duration = how long held in seconds
end
```

## Hook Point
- Register for `kMessage_MainGameLoop` via NVSEMessagingInterface
- Call `KHH_Update(deltaTime)` every frame
- deltaTime from game's frame time or calculate from GetTickCount

## Update Logic (called every frame)

```cpp
void KHH_Update(float deltaTime) {
    float currentTime = GetCurrentTime();  // accumulated time

    for (each handler in handlers) {
        bool keyDown = handler.useControlCode
            ? IsControlPressed(handler.key)
            : IsKeyPressed(handler.key);

        KeyState& state = keyStates[handler.key];

        if (keyDown && !state.isDown) {
            // Just pressed
            state.isDown = true;
            state.downTime = currentTime;
            state.lastDispatch = 0;
        }
        else if (keyDown && state.isDown) {
            // Still held
            float heldDuration = currentTime - state.downTime;

            if (heldDuration >= handler.holdThreshold) {
                bool shouldDispatch = false;

                if (handler.repeatInterval <= 0) {
                    // Every frame
                    shouldDispatch = true;
                } else {
                    // Check interval
                    if (currentTime - state.lastDispatch >= handler.repeatInterval) {
                        shouldDispatch = true;
                    }
                }

                if (shouldDispatch) {
                    DispatchCallback(handler.callback, handler.key, heldDuration);
                    state.lastDispatch = currentTime;
                }
            }
        }
        else if (!keyDown && state.isDown) {
            // Just released
            state.isDown = false;
        }
    }
}
```

## Key Detection Methods

**Raw keys (DIK scancodes):**
```cpp
// Use GetAsyncKeyState or DIHookControl
bool IsKeyPressed(UInt32 keycode) {
    // GetAsyncKeyState for VK codes, or
    // DIHookControl::GetSingleton().IsKeyPressed(keycode)
    return (GetAsyncKeyState(MapDIKToVK(keycode)) & 0x8000) != 0;
}
```

**Game controls:**
```cpp
// Use OSInputGlobals
bool IsControlPressed(UInt32 controlCode) {
    void* inputGlobals = *(void**)0x11F35CC;  // OSInputGlobals
    typedef bool (*GetControlState_t)(void*, UInt32, UInt32);
    auto GetControlState = (GetControlState_t)0xA24660;
    return GetControlState(inputGlobals, controlCode, 0);  // 0 = isHeld
}
```

## Integration with main.cpp

1. Add `#include "KeyHeldHandler.h"`
2. In `NVSEPlugin_Load`: call `KHH_Init((void*)nvse)`
3. In `MessageHandler` case `kMessage_MainGameLoop`: call `KHH_Update(deltaTime)`

## Example Usage

```
; Register: fire callback every 0.1s after 0.5s hold on key 42
int handlerID
set handlerID to RegisterKeyHeld 42 0.5 0.1 MyHeldCallback

scn MyHeldCallback
begin function {int key, float duration}
    print "Key " + $key + " held for " + $duration + " seconds"
end

; Later, to unregister:
UnregisterKeyHeld handlerID
```

## Notes
- Multiple handlers can be registered for the same key
- Handler IDs are unique and incrementing
- Log file: `Data\NVSE\Plugins\KeyHeldHandler.log`
