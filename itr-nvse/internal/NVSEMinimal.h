//minimal nvse interface structs for handlers that don't need full nvse headers
//relies on nvse/prefix.h being force-included (provides UInt8, UInt16, UInt32, etc.)
#pragma once

class TESForm;
class TESObjectREFR;
class TESObjectWEAP;
class Actor;
class Script;
class ScriptEventList;

struct CommandInfo;
struct ParamInfo;

using PluginHandle = UInt32;
constexpr PluginHandle kPluginHandle_Invalid = 0xFFFFFFFF;

struct PluginInfo {
    enum { kInfoVersion = 1 };
    UInt32      infoVersion;
    const char* name;
    UInt32      version;
};

struct NVSEInterface {
    UInt32  nvseVersion;
    UInt32  runtimeVersion;
    UInt32  editorVersion;
    UInt32  isEditor;

    bool    (*RegisterCommand)(CommandInfo* info);
    void    (*SetOpcodeBase)(UInt32 opcode);
    void*   (*QueryInterface)(UInt32 id);
    PluginHandle (*GetPluginHandle)(void);
    bool    (*RegisterTypedCommand)(CommandInfo* info, UInt32 retnType);
    const char* (*GetRuntimeDirectory)(void);
};

enum {
    kInterface_Serialization = 0,
    kInterface_Console,
    kInterface_Messaging,
    kInterface_CommandTable,
    kInterface_StringVar,
    kInterface_ArrayVar,
    kInterface_Script,
    kInterface_Data,
};

enum { kRetnType_Default = 0 };

struct NVSEArrayVarInterface {
    struct Element {
        UInt8 pad[16];
    };
};

struct NVSEScriptInterface {
    enum { kVersion = 1 };

    bool    (*CallFunction)(Script* funcScript, TESObjectREFR* callingObj,
                TESObjectREFR* container, NVSEArrayVarInterface::Element* result,
                UInt8 numArgs, ...);
    int     (*GetFunctionParams)(Script* funcScript, UInt8* paramTypesOut);
    bool    (*ExtractArgsEx)(ParamInfo* paramInfo, void* scriptDataIn,
                UInt32* scriptDataOffset, Script* scriptObj, ScriptEventList* eventList, ...);
    bool    (*ExtractFormatStringArgs)(UInt32 fmtStringPos, char* buffer,
                ParamInfo* paramInfo, void* scriptDataIn, UInt32* scriptDataOffset,
                Script* scriptObj, ScriptEventList* eventList, UInt32 maxParams, ...);
    bool    (*CallFunctionAlt)(Script* funcScript, TESObjectREFR* callingObj,
                UInt8 numArgs, ...);
    Script* (*CompileScript)(const char* scriptText);
    Script* (*CompileExpression)(const char* expression);
    size_t  (__stdcall *pDecompileToBuffer)(Script* pScript, void* pStream, char* pBuffer);
};

struct NVSEDataInterface {
    enum { kVersion = 1 };
    UInt32  version;
    void*   (*GetSingleton)(UInt32 singletonID);
    enum {
        kNVSEData_InventoryReferenceCreate = 1,
        kNVSEData_InventoryReferenceGetForRefID,
        kNVSEData_InventoryReferenceGetRefBySelf,
        kNVSEData_ArrayVarMapDeleteBySelf,
        kNVSEData_StringVarMapDeleteBySelf,
        kNVSEData_LambdaDeleteAllForScript,
        kNVSEData_InventoryReferenceCreateEntry,
        kNVSEData_LambdaSaveVariableList,
        kNVSEData_LambdaUnsaveVariableList,
    };
    void*   (*GetFunc)(UInt32 funcID);
    void*   (*GetData)(UInt32 dataID);
};

struct NVSEConsoleInterface {
    enum { kVersion = 1 };
    UInt32  version;
    bool    (*RunScriptLine)(const char* buf, TESObjectREFR* object);
};

using _CaptureLambdaVars = void (*)(Script* scriptLambda);
using _UncaptureLambdaVars = void (*)(Script* scriptLambda);

#define COMMAND_ARGS        ParamInfo* paramInfo, void* scriptData, TESObjectREFR* thisObj, \
                            TESObjectREFR* containingObj, Script* scriptObj, ScriptEventList* eventList, \
                            double* result, UInt32* opcodeOffsetPtr

#define EXTRACT_ARGS_EX     paramInfo, scriptData, opcodeOffsetPtr, scriptObj, eventList

using CommandExecuteFunc = bool (*)(COMMAND_ARGS);
using CommandParseFunc = bool (*)(UInt32, void*, void*, void*);
using CommandEvalFunc = bool (*)(TESObjectREFR*, void*, void*, double*);

struct ParamInfo {
    const char* typeStr;
    UInt32      typeID;
    UInt32      isOptional;
};

struct CommandInfo {
    const char*         longName;
    const char*         shortName;
    UInt32              opcode;
    const char*         helpText;
    UInt16              needsParent;
    UInt16              numParams;
    ParamInfo*          params;
    CommandExecuteFunc  execute;
    CommandParseFunc    parse;
    CommandEvalFunc     eval;
    UInt32              flags;
};

enum ParamType {
    kParamType_String       = 0x00,
    kParamType_Integer      = 0x01,
    kParamType_Float        = 0x02,
    kParamType_ObjectID     = 0x03,
    kParamType_ObjectRef    = 0x04,
    kParamType_ActorValue   = 0x05,
    kParamType_Actor        = 0x06,
    kParamType_AnyForm      = 0x3D,
};

enum FormType {
    kFormType_Script = 0x11,
};

#define DEFINE_COMMAND_PLUGIN(name, desc, needsParent, numParams, params) \
    extern bool Cmd_##name##_Execute(COMMAND_ARGS); \
    static CommandInfo kCommandInfo_##name = { \
        #name, "", 0, desc, needsParent, numParams, params, \
        Cmd_##name##_Execute, nullptr, nullptr, 0 \
    }

namespace SafeWrite {
    inline void Write8(UInt32 addr, UInt8 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt8*)addr = data;
        VirtualProtect((void*)addr, 1, oldProtect, &oldProtect);
    }

    inline void Write32(UInt32 addr, UInt32 data) {
        DWORD oldProtect;
        VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(UInt32*)addr = data;
        VirtualProtect((void*)addr, 4, oldProtect, &oldProtect);
    }

    inline void WriteRelCall(UInt32 src, UInt32 dst) {
        Write8(src, 0xE8);
        Write32(src + 1, dst - src - 5);
    }

    inline void WriteRelJump(UInt32 src, UInt32 dst) {
        Write8(src, 0xE9);
        Write32(src + 1, dst - src - 5);
    }

    inline UInt32 GetRelJumpTarget(UInt32 src) {
        return *(UInt32*)(src + 1) + src + 5;
    }
}
