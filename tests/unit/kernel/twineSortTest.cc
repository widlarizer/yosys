#include <gtest/gtest.h>

#include <chrono>

#include "kernel/rtlil.h"
#include "kernel/yosys.h"
#include "tests/unit/yosysSetupEnv.h"

YOSYS_NAMESPACE_BEGIN

namespace {

std::vector<IdString> bench_refs(TwinePool &twines, int count)
{
	IdString prefix = twines.add(std::string("$sortbench"));
	std::vector<IdString> refs;
	refs.reserve(count);
	for (int i = 0; i < count; i++)
		refs.push_back(twines.add(Twine::Suffix{prefix, stringf("$%08d", (count - i) * 7919 % count)}));
	return refs;
}

TEST(TwineSortTest, OrdersByRenderedString)
{
	TwinePool twines;
	std::vector<IdString> refs = {
		twines.add(std::string("$c")),
		twines.add(std::string("$a")),
		twines.add(std::string("$b")),
	};
	std::sort(refs.begin(), refs.end(), RTLIL::sort_by_twine_str_expensive(twines));
	EXPECT_EQ(twines.str(refs[0]), "$a");
	EXPECT_EQ(twines.str(refs[1]), "$b");
	EXPECT_EQ(twines.str(refs[2]), "$c");
}

// Two handles onto one content node must not be collapsed by the memo: the
// rendered forms differ by the '\' escape, so they order strictly.
TEST(TwineSortTest, PublicAndPrivateHandlesOrderStrictly)
{
	TwinePool twines;
	IdString pub = twines.add(std::string("\\same"));
	IdString priv = pub.tag(false);

	ASSERT_EQ(twines.str(pub), "\\same");
	ASSERT_EQ(twines.str(priv), "same");

	RTLIL::sort_by_twine_str_expensive less(twines);
	EXPECT_NE(less(pub, priv), less(priv, pub));
	EXPECT_TRUE(less(pub, priv));
}

TEST(TwineSortTest, SortIsAStrictWeakOrdering)
{
	TwinePool twines;
	std::vector<IdString> refs = bench_refs(twines, 500);
	RTLIL::sort_by_twine_str_expensive less(twines);
	std::sort(refs.begin(), refs.end(), less);
	for (size_t i = 1; i < refs.size(); i++)
		ASSERT_FALSE(less(refs[i], refs[i - 1])) << "at " << i;
}

// Harness rather than an assertion of speed: the memoised comparator flattens
// each ref once per sort instead of twice per comparison, so this should stay
// far below the n log n flattening it replaced.
TEST(TwineSortTest, BenchmarkSort)
{
	constexpr int kNames = 50000;

	TwinePool twines;
	std::vector<IdString> refs = bench_refs(twines, kNames);

	auto t0 = std::chrono::steady_clock::now();
	std::sort(refs.begin(), refs.end(), RTLIL::sort_by_twine_str_expensive(twines));
	auto t1 = std::chrono::steady_clock::now();

	size_t flattens = 0;
	std::vector<IdString> naive = bench_refs(twines, kNames);
	auto t2 = std::chrono::steady_clock::now();
	std::sort(naive.begin(), naive.end(), [&](IdString a, IdString b) {
		flattens += 2;
		return twines.str(a) < twines.str(b);
	});
	auto t3 = std::chrono::steady_clock::now();

	auto ms = [](auto a, auto b) {
		return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
	};
	std::cerr << "[ BENCH    ] " << kNames << " refs: memoised " << ms(t0, t1)
		  << " ms vs per-comparison " << ms(t2, t3) << " ms ("
		  << flattens << " flattens vs " << kNames << ")\n";
	RecordProperty("memoised_ms", std::to_string(ms(t0, t1)));
	RecordProperty("naive_ms", std::to_string(ms(t2, t3)));

	EXPECT_LT(ms(t0, t1), ms(t2, t3));
}

} // namespace

YOSYS_NAMESPACE_END
