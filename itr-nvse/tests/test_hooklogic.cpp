//tests for testable C++ logic called by asm hooks

#include "test.h"
#include "internal/ConsoleCommand.h"
#include "internal/FallDamageLogic.h"
#include "internal/FormatLogic.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>

typedef unsigned int UInt32;
typedef unsigned short UInt16;
typedef unsigned char UInt8;
typedef signed int SInt32;

#include "internal/PickpocketHookLogic.h"

namespace
{
	void* g_pickpocketSelection = nullptr;
	SInt32 g_pickpocketCount = 0;
	void* g_pickpocketActor = nullptr;
	SInt32 g_pickpocketItemValue = 0;

	bool __fastcall CapturePickpocketArgs(void* selection, SInt32 count, void* actor, SInt32 itemValue)
	{
		g_pickpocketSelection = selection;
		g_pickpocketCount = count;
		g_pickpocketActor = actor;
		g_pickpocketItemValue = itemValue;
		return true;
	}
}

TEST(PickpocketHook_ForwardsAllFastcallArguments)
{
	void* selection = reinterpret_cast<void*>(0x11223344);
	void* actor = reinterpret_cast<void*>(0x55667788);
	ASSERT(PickpocketHookLogic::ForwardTryPickpocket(CapturePickpocketArgs, selection, 3, actor, 9876));
	ASSERT_EQ(g_pickpocketSelection, selection);
	ASSERT_EQ(g_pickpocketCount, 3);
	ASSERT_EQ(g_pickpocketActor, actor);
	ASSERT_EQ(g_pickpocketItemValue, 9876);
	return true;
}

TEST(FormatFileSize_Bytes)
{
	char buf[32];
	FormatLogic::FormatFileSize(512, buf, sizeof(buf));
	ASSERT_STREQ(buf, "512 B");
	return true;
}

TEST(FormatFileSize_ZeroBytes)
{
	char buf[32];
	FormatLogic::FormatFileSize(0, buf, sizeof(buf));
	ASSERT_STREQ(buf, "0 B");
	return true;
}

TEST(FormatFileSize_OneByte)
{
	char buf[32];
	FormatLogic::FormatFileSize(1, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1 B");
	return true;
}

TEST(FormatFileSize_Kilobytes)
{
	char buf[32];
	FormatLogic::FormatFileSize(1024, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1.0 KB");
	return true;
}

TEST(FormatFileSize_Kilobytes_Fractional)
{
	char buf[32];
	FormatLogic::FormatFileSize(1536, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1.5 KB");
	return true;
}

TEST(FormatFileSize_JustUnderMB)
{
	char buf[32];
	FormatLogic::FormatFileSize(1048575, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1024.0 KB");
	return true;
}

TEST(FormatFileSize_Megabytes)
{
	char buf[32];
	FormatLogic::FormatFileSize(1048576, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1.0 MB");
	return true;
}

TEST(FormatFileSize_Megabytes_Large)
{
	char buf[32];
	FormatLogic::FormatFileSize(10485760, buf, sizeof(buf));
	ASSERT_STREQ(buf, "10.0 MB");
	return true;
}

TEST(FormatFileSize_Megabytes_Fractional)
{
	char buf[32];
	FormatLogic::FormatFileSize(1572864, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1.5 MB");
	return true;
}

TEST(FormatFileSize_OneGigabyteStillUsesMB)
{
	char buf[32];
	FormatLogic::FormatFileSize(1073741824ULL, buf, sizeof(buf));
	ASSERT_STREQ(buf, "1024.0 MB");
	return true;
}

TEST(FormatFileSize_NullOutputIgnored)
{
	FormatLogic::FormatFileSize(1024, nullptr, 0);
	return true;
}

static char g_msgBuffer[512];

static const char* FormatReputationMessage(const char* factionName, const char* repTitle, const char* repDesc)
{
	return FormatLogic::FormatReputationMessage(g_msgBuffer, sizeof(g_msgBuffer), factionName, repTitle, repDesc);
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

TEST(FormatReputationMessage_NullFields)
{
	const char* result = FormatReputationMessage(nullptr, nullptr, nullptr);
	ASSERT_STREQ(result, " - . ");
	return true;
}

TEST(ConsoleCommand_ExtractsSimpleCommand)
{
	char buf[64];
	ASSERT(ConsoleCommand::ExtractCommandName("tgm", buf, sizeof(buf)));
	ASSERT_STREQ(buf, "tgm");
	return true;
}

TEST(ConsoleCommand_LowercasesCommand)
{
	char buf[64];
	ASSERT(ConsoleCommand::ExtractCommandName("TCL", buf, sizeof(buf)));
	ASSERT_STREQ(buf, "tcl");
	return true;
}

TEST(ConsoleCommand_StripsRefPrefix)
{
	char buf[64];
	ASSERT(ConsoleCommand::ExtractCommandName("player.additem 0000000F 1", buf, sizeof(buf)));
	ASSERT_STREQ(buf, "additem");
	return true;
}

TEST(ConsoleCommand_StripsArrowRefPrefix)
{
	char buf[64];
	ASSERT(ConsoleCommand::ExtractCommandName("someRef->disable", buf, sizeof(buf)));
	ASSERT_STREQ(buf, "disable");
	return true;
}

TEST(ConsoleCommand_IgnoresEmptyAndComments)
{
	char buf[64] = "unchanged";
	ASSERT(!ConsoleCommand::ExtractCommandName("   ; comment", buf, sizeof(buf)));
	ASSERT_STREQ(buf, "");
	return true;
}

TEST(ConsoleCommand_TruncatesToOutputBuffer)
{
	char buf[5];
	ASSERT(ConsoleCommand::ExtractCommandName("VeryLongCommand arg", buf, sizeof(buf)));
	ASSERT_STREQ(buf, "very");
	return true;
}

TEST(ConsoleCommand_OneByteOutputBuffer)
{
	char buf[1] = {'x'};
	ASSERT(ConsoleCommand::ExtractCommandName("tgm", buf, sizeof(buf)));
	ASSERT_STREQ(buf, "");
	return true;
}

TEST(ConsoleCommand_NullLineLeavesOutputUntouched)
{
	char buf[16] = "unchanged";
	ASSERT(!ConsoleCommand::ExtractCommandName(nullptr, buf, sizeof(buf)));
	ASSERT_STREQ(buf, "unchanged");
	return true;
}

TEST(ConsoleCommand_NullOutputRejected)
{
	ASSERT(!ConsoleCommand::ExtractCommandName("tgm", nullptr, 16));
	return true;
}

TEST(ConsoleCommand_StripsLastDottedPrefix)
{
	char buf[64];
	ASSERT(ConsoleCommand::ExtractCommandName("foo.bar.baz 1", buf, sizeof(buf)));
	ASSERT_STREQ(buf, "baz");
	return true;
}

static float g_globalFallDamageMult = 1.0f;
static std::unordered_map<UInt32, float> g_actorFallDamageMults;

static float GetFallDamageMultForActor(UInt32 refID)
{
	return FallDamageLogic::ResolveMultiplier(refID, g_globalFallDamageMult, g_actorFallDamageMults);
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

TEST(FallDamageMult_ClampNegative)
{
	ASSERT_NEAR(FallDamageLogic::ClampMultiplier(-2.0f), 0.0f, 0.001f);
	ASSERT_NEAR(FallDamageLogic::ClampMultiplier(1.25f), 1.25f, 0.001f);
	return true;
}

TEST(FallDamageMult_OverrideStorageRules)
{
	ASSERT(!FallDamageLogic::StoresActorOverride(1.0f));
	ASSERT(FallDamageLogic::StoresActorOverride(0.0f));
	ASSERT(FallDamageLogic::StoresActorOverride(1.25f));
	return true;
}
