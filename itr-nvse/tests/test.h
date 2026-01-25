#pragma once
//minimal test framework for nvse plugin unit tests

#include <cstdio>
#include <cstring>
#include <vector>
#include <functional>
#include <cmath>

namespace Test {

struct TestCase {
	const char* name;
	std::function<bool()> fn;
};

inline std::vector<TestCase>& GetTests() {
	static std::vector<TestCase> tests;
	return tests;
}

inline int g_passed = 0;
inline int g_failed = 0;

struct TestRegistrar {
	TestRegistrar(const char* name, std::function<bool()> fn) {
		GetTests().push_back({name, fn});
	}
};

#define TEST(name) \
	static bool test_##name(); \
	static Test::TestRegistrar _reg_##name(#name, test_##name); \
	static bool test_##name()

#define ASSERT(cond) \
	do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); return false; } } while(0)

#define ASSERT_EQ(a, b) \
	do { if ((a) != (b)) { printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); return false; } } while(0)

#define ASSERT_STREQ(a, b) \
	do { if (strcmp((a), (b)) != 0) { printf("  FAIL: \"%s\" != \"%s\" (line %d)\n", (a), (b), __LINE__); return false; } } while(0)

#define ASSERT_NEAR(a, b, eps) \
	do { if (fabs((a) - (b)) > (eps)) { printf("  FAIL: %f != %f (line %d)\n", (double)(a), (double)(b), __LINE__); return false; } } while(0)

inline int RunAll()
{
	auto& tests = GetTests();
	printf("Running %zu tests...\n\n", tests.size());
	for (auto& t : tests) {
		printf("[%s]\n", t.name);
		if (t.fn()) {
			printf("  PASS\n");
			g_passed++;
		} else {
			g_failed++;
		}
	}
	printf("\n%d passed, %d failed\n", g_passed, g_failed);
	return g_failed;
}

}
