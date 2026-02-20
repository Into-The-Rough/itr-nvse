//kNVSE EventManager interface types - shared across handlers
//include AFTER your NVSE headers (NVSEMinimal.h or nvse/PluginAPI.h)
#pragma once

class TESObjectREFR;
class TESForm;

namespace EventManager {

struct ResultElement
{
	enum Type : unsigned char
	{
		kType_Invalid = 0,
		kType_Numeric = 1,
		kType_Form = 2,
		kType_String = 3,
		kType_Array = 4
	};

	union
	{
		unsigned int raw;
		char* str;
		void* arr;
		TESForm* form;
		double num;
	};
	unsigned char type;

	bool IsValid() const { return type != kType_Invalid; }
};

struct Interface
{
	enum DispatchReturn : int
	{
		kRetn_UnknownEvent = -2,
		kRetn_GenericError = -1,
		kRetn_Normal = 0,
		kRetn_EarlyBreak = 1,
		kRetn_Deferred = 2
	};

	using DispatchCallback = bool (*)(ResultElement& result, void* anyData);

	bool (*RegisterEvent)(const char* name, unsigned char numParams, unsigned char* paramTypes, unsigned int flags);
	bool (*DispatchEvent)(const char* eventName, TESObjectREFR* thisObj, ...);
	DispatchReturn (*DispatchEventAlt)(const char* eventName, DispatchCallback resultCallback, void* anyData, TESObjectREFR* thisObj, ...);
};

enum ParamType : unsigned char
{
	kParam_Anything = 0,
	kParam_Float = 1,
	kParam_Form = 2,
	kParam_Int = 3,
	kParam_String = 4,
	kParam_Array = 5
};

constexpr unsigned int kFlag_FlushOnLoad = 1;
constexpr unsigned int kInterface_EventManager = 8;

}
