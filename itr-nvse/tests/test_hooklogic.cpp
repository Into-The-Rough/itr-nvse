//tests for testable C++ logic called by asm hooks

#include "test.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>

typedef unsigned int UInt32;
typedef unsigned short UInt16;
typedef unsigned char UInt8;
typedef signed int SInt32;

//============================================================
// FormatFileSize (from SaveFileSizeHandler.cpp)
//============================================================

static void FormatFileSize(unsigned long long bytes, char* out, size_t outSize)
{
	if (bytes >= 1048576ULL)
		sprintf_s(out, outSize, "%.1f MB", bytes / 1048576.0);
	else if (bytes >= 1024ULL)
		sprintf_s(out, outSize, "%.1f KB", bytes / 1024.0);
	else
		sprintf_s(out, outSize, "%llu B", bytes);
}

TEST(FormatFileSize_Bytes)
{
	char buf[32];
	FormatFileSize(512, buf, sizeof(buf));
	ASSERT_STREQ(buf, "512 B");
	return true;
}

TEST(FormatFileSize_ZeroBytes)
{
	char buf[32];
	FormatFileSize(0, buf, sizeof(buf));
	ASSERT_STREQ(buf, "0 B");
	return true;
}

TEST(FormatFileSize_OneByte)
{
	char buf[32];
	FormatFileSize(1, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1 B");
	return true;
}

TEST(FormatFileSize_Kilobytes)
{
	char buf[32];
	FormatFileSize(1024, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1.0 KB");
	return true;
}

TEST(FormatFileSize_Kilobytes_Fractional)
{
	char buf[32];
	FormatFileSize(1536, buf, sizeof(buf));  //1.5 KB
	ASSERT_STREQ(buf, "1.5 KB");
	return true;
}

TEST(FormatFileSize_JustUnderMB)
{
	char buf[32];
	FormatFileSize(1048575, buf, sizeof(buf));  //1 MB - 1 byte
	ASSERT_STREQ(buf, "1024.0 KB");
	return true;
}

TEST(FormatFileSize_Megabytes)
{
	char buf[32];
	FormatFileSize(1048576, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1.0 MB");
	return true;
}

TEST(FormatFileSize_Megabytes_Large)
{
	char buf[32];
	FormatFileSize(10485760, buf, sizeof(buf));  //10 MB
	ASSERT_STREQ(buf, "10.0 MB");
	return true;
}

TEST(FormatFileSize_Megabytes_Fractional)
{
	char buf[32];
	FormatFileSize(1572864, buf, sizeof(buf));  //1.5 MB
	ASSERT_STREQ(buf, "1.5 MB");
	return true;
}

//============================================================
// FormatReputationMessage (from ELMO.cpp)
//============================================================

static char g_msgBuffer[512];

static const char* FormatReputationMessage(const char* factionName, const char* repTitle, const char* repDesc)
{
	sprintf_s(g_msgBuffer, "%s - %s. %s", factionName, repTitle, repDesc);
	return g_msgBuffer;
}

TEST(FormatReputationMessage_Basic)
{
	const char* result = FormatReputationMessage("NCR", "Liked", "They appreciate your help");
	ASSERT_STREQ(result, "NCR - Liked. They appreciate your help");
	return true;
}

TEST(FormatReputationMessage_EmptyDesc)
{
	const char* result = FormatReputationMessage("Legion", "Vilified", "");
	ASSERT_STREQ(result, "Legion - Vilified. ");
	return true;
}

//============================================================
// GetFallDamageMultForActor (from FallDamageHandler.cpp)
//============================================================

static float g_globalFallDamageMult = 1.0f;
static std::unordered_map<UInt32, float> g_actorFallDamageMults;

static float GetFallDamageMultForActor(UInt32 refID)
{
	if (refID && !g_actorFallDamageMults.empty())
	{
		auto it = g_actorFallDamageMults.find(refID);
		if (it != g_actorFallDamageMults.end())
			return it->second;
	}
	return g_globalFallDamageMult;
}

TEST(FallDamageMult_DefaultGlobal)
{
	g_globalFallDamageMult = 1.0f;
	g_actorFallDamageMults.clear();
	ASSERT_NEAR(GetFallDamageMultForActor(0x12345), 1.0f, 0.001f);
	return true;
}

TEST(FallDamageMult_CustomGlobal)
{
	g_globalFallDamageMult = 0.5f;
	g_actorFallDamageMults.clear();
	ASSERT_NEAR(GetFallDamageMultForActor(0x12345), 0.5f, 0.001f);
	g_globalFallDamageMult = 1.0f;
	return true;
}

TEST(FallDamageMult_PerActorOverride)
{
	g_globalFallDamageMult = 1.0f;
	g_actorFallDamageMults.clear();
	g_actorFallDamageMults[0x12345] = 0.0f;  //immune
	ASSERT_NEAR(GetFallDamageMultForActor(0x12345), 0.0f, 0.001f);
	g_actorFallDamageMults.clear();
	return true;
}

TEST(FallDamageMult_PerActorNoMatch)
{
	g_globalFallDamageMult = 1.0f;
	g_actorFallDamageMults.clear();
	g_actorFallDamageMults[0x12345] = 0.0f;
	//different actor should get global
	ASSERT_NEAR(GetFallDamageMultForActor(0x99999), 1.0f, 0.001f);
	g_actorFallDamageMults.clear();
	return true;
}

TEST(FallDamageMult_ZeroRefID)
{
	g_globalFallDamageMult = 2.0f;
	g_actorFallDamageMults.clear();
	g_actorFallDamageMults[0] = 0.0f;
	//zero refID should still return global
	ASSERT_NEAR(GetFallDamageMultForActor(0), 2.0f, 0.001f);
	g_globalFallDamageMult = 1.0f;
	g_actorFallDamageMults.clear();
	return true;
}

//============================================================
// GetExtraDataByType (from VATSLimbFix.cpp)
//============================================================

struct BSExtraData {
	void** vtbl;
	UInt8 type;
	UInt8 pad[3];
	BSExtraData* next;
};

struct BaseExtraList {
	void** vtbl;
	BSExtraData* head;
	UInt8 presentBits[0x15];
	UInt8 pad1D[3];
};

static BSExtraData* GetExtraDataByType(BaseExtraList* list, UInt8 type) {
	if (!list || !list->head) return nullptr;
	UInt8 byteIndex = type >> 3;
	UInt8 bitMask = 1 << (type & 7);
	if (byteIndex < sizeof(list->presentBits) && !(list->presentBits[byteIndex] & bitMask))
		return nullptr;
	for (BSExtraData* data = list->head; data; data = data->next) {
		if (data->type == type)
			return data;
	}
	return nullptr;
}

TEST(ExtraData_NullList)
{
	ASSERT(GetExtraDataByType(nullptr, 1) == nullptr);
	return true;
}

TEST(ExtraData_EmptyList)
{
	BaseExtraList list = {};
	list.head = nullptr;
	ASSERT(GetExtraDataByType(&list, 1) == nullptr);
	return true;
}

TEST(ExtraData_SingleItemFound)
{
	BSExtraData data = {};
	data.type = 5;
	data.next = nullptr;

	BaseExtraList list = {};
	list.head = &data;
	memset(list.presentBits, 0, sizeof(list.presentBits));
	list.presentBits[0] = (1 << 5);  //type 5 present

	ASSERT(GetExtraDataByType(&list, 5) == &data);
	return true;
}

TEST(ExtraData_SingleItemNotFound)
{
	BSExtraData data = {};
	data.type = 5;
	data.next = nullptr;

	BaseExtraList list = {};
	list.head = &data;
	memset(list.presentBits, 0, sizeof(list.presentBits));
	list.presentBits[0] = (1 << 5);  //type 5 present

	ASSERT(GetExtraDataByType(&list, 3) == nullptr);
	return true;
}

TEST(ExtraData_ChainedItems)
{
	BSExtraData data1 = {};
	BSExtraData data2 = {};
	BSExtraData data3 = {};

	data1.type = 1;
	data1.next = &data2;
	data2.type = 5;
	data2.next = &data3;
	data3.type = 10;
	data3.next = nullptr;

	BaseExtraList list = {};
	list.head = &data1;
	memset(list.presentBits, 0xFF, sizeof(list.presentBits));  //all present

	ASSERT(GetExtraDataByType(&list, 1) == &data1);
	ASSERT(GetExtraDataByType(&list, 5) == &data2);
	ASSERT(GetExtraDataByType(&list, 10) == &data3);
	return true;
}

TEST(ExtraData_BitNotSet)
{
	BSExtraData data = {};
	data.type = 5;
	data.next = nullptr;

	BaseExtraList list = {};
	list.head = &data;
	memset(list.presentBits, 0, sizeof(list.presentBits));
	//bit not set, should return nullptr without traversing

	ASSERT(GetExtraDataByType(&list, 5) == nullptr);
	return true;
}

TEST(ExtraData_HighType)
{
	BSExtraData data = {};
	data.type = 0x5F;  //95 = dismembered limbs type
	data.next = nullptr;

	BaseExtraList list = {};
	list.head = &data;
	memset(list.presentBits, 0, sizeof(list.presentBits));
	//type 0x5F = byte 11 (0x5F >> 3), bit 7 (0x5F & 7)
	list.presentBits[11] = (1 << 7);

	ASSERT(GetExtraDataByType(&list, 0x5F) == &data);
	return true;
}
