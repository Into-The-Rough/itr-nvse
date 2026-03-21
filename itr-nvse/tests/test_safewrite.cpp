//tests for internal/SafeWrite.h

#include "test.h"
#include <cstring>

typedef unsigned int UInt32;
typedef unsigned short UInt16;
typedef unsigned char UInt8;
typedef signed int SInt32;

#include "../internal/SafeWrite.h"

//test buffer for write operations
static UInt8 g_testBuf[64];

static void ClearBuf() {
	memset(g_testBuf, 0xCC, sizeof(g_testBuf));
}

TEST(SafeWrite_Write8)
{
	ClearBuf();
	SafeWrite::Write8((UInt32)&g_testBuf[0], 0x42);
	ASSERT_EQ(g_testBuf[0], 0x42);
	ASSERT_EQ(g_testBuf[1], 0xCC); //unchanged
	return true;
}

TEST(SafeWrite_Write32)
{
	ClearBuf();
	SafeWrite::Write32((UInt32)&g_testBuf[0], 0xDEADBEEF);
	ASSERT_EQ(g_testBuf[0], 0xEF);
	ASSERT_EQ(g_testBuf[1], 0xBE);
	ASSERT_EQ(g_testBuf[2], 0xAD);
	ASSERT_EQ(g_testBuf[3], 0xDE);
	ASSERT_EQ(g_testBuf[4], 0xCC); //unchanged
	return true;
}

TEST(SafeWrite_WriteBuf)
{
	ClearBuf();
	const UInt8 src[] = { 0x10, 0x20, 0x30, 0x40, 0x50 };
	SafeWrite::WriteBuf((UInt32)&g_testBuf[4], src, sizeof(src));
	for (int i = 0; i < 5; i++) {
		ASSERT_EQ(g_testBuf[4 + i], src[i]);
	}
	ASSERT_EQ(g_testBuf[3], 0xCC);
	ASSERT_EQ(g_testBuf[9], 0xCC);
	return true;
}

TEST(SafeWrite_WriteBuf_ZeroSize)
{
	ClearBuf();
	const UInt8 src[] = { 0x11, 0x22, 0x33 };
	SafeWrite::WriteBuf((UInt32)&g_testBuf[0], src, 0);
	for (int i = 0; i < 3; i++) {
		ASSERT_EQ(g_testBuf[i], 0xCC);
	}
	return true;
}

TEST(SafeWrite_WriteRelCall_Opcode)
{
	ClearBuf();
	//call from 0x1000 to 0x2000
	//offset = 0x2000 - 0x1000 - 5 = 0xFFB
	UInt32 src = (UInt32)&g_testBuf[0];
	UInt32 dst = src + 0x1000;
	SafeWrite::WriteRelCall(src, dst);
	ASSERT_EQ(g_testBuf[0], 0xE8); //call opcode
	return true;
}

TEST(SafeWrite_WriteRelCall_Offset)
{
	ClearBuf();
	UInt32 src = (UInt32)&g_testBuf[0];
	UInt32 dst = src + 0x1000;
	SafeWrite::WriteRelCall(src, dst);
	//offset stored at bytes 1-4
	SInt32 offset = *(SInt32*)&g_testBuf[1];
	//expected: dst - src - 5 = 0x1000 - 5 = 0xFFB
	ASSERT_EQ(offset, 0xFFB);
	return true;
}

TEST(SafeWrite_WriteRelCall_NegativeOffset)
{
	ClearBuf();
	UInt32 src = (UInt32)&g_testBuf[32];
	UInt32 dst = (UInt32)&g_testBuf[0]; //backward jump
	SafeWrite::WriteRelCall(src, dst);
	ASSERT_EQ(g_testBuf[32], 0xE8);
	SInt32 offset = *(SInt32*)&g_testBuf[33];
	//expected: dst - src - 5 = -32 - 5 = -37
	ASSERT_EQ(offset, -37);
	return true;
}

TEST(SafeWrite_WriteRelJump_Opcode)
{
	ClearBuf();
	UInt32 src = (UInt32)&g_testBuf[0];
	UInt32 dst = src + 0x500;
	SafeWrite::WriteRelJump(src, dst);
	ASSERT_EQ(g_testBuf[0], 0xE9); //jmp opcode
	return true;
}

TEST(SafeWrite_WriteRelJump_Offset)
{
	ClearBuf();
	UInt32 src = (UInt32)&g_testBuf[0];
	UInt32 dst = src + 0x500;
	SafeWrite::WriteRelJump(src, dst);
	SInt32 offset = *(SInt32*)&g_testBuf[1];
	ASSERT_EQ(offset, 0x4FB); //0x500 - 5
	return true;
}

TEST(SafeWrite_WriteNop_Single)
{
	ClearBuf();
	SafeWrite::WriteNop((UInt32)&g_testBuf[0], 1);
	ASSERT_EQ(g_testBuf[0], 0x90);
	ASSERT_EQ(g_testBuf[1], 0xCC); //unchanged
	return true;
}

TEST(SafeWrite_WriteNop_Multiple)
{
	ClearBuf();
	SafeWrite::WriteNop((UInt32)&g_testBuf[0], 6);
	for (int i = 0; i < 6; i++) {
		ASSERT_EQ(g_testBuf[i], 0x90);
	}
	ASSERT_EQ(g_testBuf[6], 0xCC); //unchanged
	return true;
}

TEST(SafeWrite_WriteNop_ZeroSize)
{
	ClearBuf();
	SafeWrite::WriteNop((UInt32)&g_testBuf[0], 0);
	ASSERT_EQ(g_testBuf[0], 0xCC);
	ASSERT_EQ(g_testBuf[1], 0xCC);
	return true;
}

TEST(SafeWrite_GetRelJumpTarget_Forward)
{
	ClearBuf();
	UInt32 src = (UInt32)&g_testBuf[0];
	UInt32 dst = src + 0x1000;
	SafeWrite::WriteRelJump(src, dst);
	UInt32 decoded = SafeWrite::GetRelJumpTarget(src);
	ASSERT_EQ(decoded, dst);
	return true;
}

TEST(SafeWrite_GetRelJumpTarget_Backward)
{
	ClearBuf();
	UInt32 src = (UInt32)&g_testBuf[32];
	UInt32 dst = (UInt32)&g_testBuf[0];
	SafeWrite::WriteRelJump(src, dst);
	UInt32 decoded = SafeWrite::GetRelJumpTarget(src);
	ASSERT_EQ(decoded, dst);
	return true;
}

TEST(SafeWrite_WriteRelCall_ThenDecode)
{
	ClearBuf();
	UInt32 src = (UInt32)&g_testBuf[0];
	UInt32 dst = src + 0x2000;
	SafeWrite::WriteRelCall(src, dst);
	//GetRelJumpTarget works for calls too (same encoding)
	UInt32 decoded = SafeWrite::GetRelJumpTarget(src);
	ASSERT_EQ(decoded, dst);
	return true;
}
