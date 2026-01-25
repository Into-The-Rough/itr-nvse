//tests for internal/StringMatch.h

#include "test.h"
#include "../internal/StringMatch.h"

using namespace StringMatch;

TEST(StringMatch_NullHaystack)
{
	ASSERT(!ContainsSubstringCI(nullptr, "test"));
	return true;
}

TEST(StringMatch_NullNeedle)
{
	ASSERT(ContainsSubstringCI("hello world", nullptr));
	return true;
}

TEST(StringMatch_EmptyNeedle)
{
	ASSERT(ContainsSubstringCI("hello world", ""));
	return true;
}

TEST(StringMatch_WildcardNeedle)
{
	ASSERT(ContainsSubstringCI("hello world", "*"));
	return true;
}

TEST(StringMatch_ExactMatch)
{
	ASSERT(ContainsSubstringCI("hello", "hello"));
	return true;
}

TEST(StringMatch_SubstringAtStart)
{
	ASSERT(ContainsSubstringCI("hello world", "hello"));
	return true;
}

TEST(StringMatch_SubstringAtEnd)
{
	ASSERT(ContainsSubstringCI("hello world", "world"));
	return true;
}

TEST(StringMatch_SubstringInMiddle)
{
	ASSERT(ContainsSubstringCI("hello world", "lo wo"));
	return true;
}

TEST(StringMatch_CaseInsensitive)
{
	ASSERT(ContainsSubstringCI("Hello World", "HELLO"));
	ASSERT(ContainsSubstringCI("HELLO WORLD", "hello"));
	ASSERT(ContainsSubstringCI("HeLLo WoRLd", "ello"));
	return true;
}

TEST(StringMatch_NotFound)
{
	ASSERT(!ContainsSubstringCI("hello world", "xyz"));
	return true;
}

TEST(StringMatch_EmptyHaystack)
{
	ASSERT(!ContainsSubstringCI("", "test"));
	return true;
}

TEST(StringMatch_BothEmpty)
{
	ASSERT(ContainsSubstringCI("", ""));
	return true;
}

TEST(StringMatch_NeedleLongerThanHaystack)
{
	ASSERT(!ContainsSubstringCI("hi", "hello"));
	return true;
}
