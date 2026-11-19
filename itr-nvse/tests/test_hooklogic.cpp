#include "test.h"
#include "internal/ConsoleCommand.h"
#include "internal/RadioInjectionLogic.h"

TEST(RadioInjection_BuildsDataSoundPath)
{
	char path[256];
	ASSERT(RadioInjectionLogic::BuildEnginePath("fx\\Stingers\\Cave\\1.wav", path, sizeof(path)));
	ASSERT_STREQ(path, "data\\sound\\fx\\Stingers\\Cave\\1.wav");
	return true;
}

TEST(RadioInjection_PreservesDataSoundPath)
{
	char path[256];
	ASSERT(RadioInjectionLogic::BuildEnginePath("Data\\Sound\\fx\\Stingers\\Cave\\1.wav", path, sizeof(path)));
	ASSERT_STREQ(path, "Data\\Sound\\fx\\Stingers\\Cave\\1.wav");
	return true;
}

TEST(RadioInjection_RejectsTruncatedEnginePath)
{
	char path[16];
	ASSERT(!RadioInjectionLogic::BuildEnginePath("fx\\Stingers\\Cave\\1.wav", path, sizeof(path)));
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
