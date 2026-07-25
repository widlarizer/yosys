#include <gtest/gtest.h>

#include "kernel/rtlil.h"
#include "kernel/yosys.h"
#include "tests/unit/yosysSetupEnv.h"

YOSYS_NAMESPACE_BEGIN

TEST(TwinePublicityTest, LeafEscapeParsing)
{
	TwinePool pool;
	IdString pub = pool.add(std::string("\\foo"));
	IdString priv = pool.add(std::string("$foo"));

	EXPECT_TRUE(twine_is_public(pub));
	EXPECT_FALSE(twine_is_public(priv));
	EXPECT_EQ(pool.str(pub), "\\foo");
	EXPECT_EQ(pool.unescaped_str(pub), "foo");
	EXPECT_EQ(pool.str(priv), "$foo");
	EXPECT_EQ(pool.unescaped_str(priv), "$foo");
}

TEST(TwinePublicityTest, EscapedDollarStaysDistinct)
{
	// Verilog escaped identifier `\$foo` (public, content "$foo") must not
	// collide with the private name `$foo` as a dict key.
	TwinePool pool;
	IdString pub = pool.add(std::string("\\$foo"));
	IdString priv = pool.add(std::string("$foo"));

	EXPECT_EQ(twine_untag(pub), twine_untag(priv)); // shared content node
	EXPECT_NE(pub, priv);                           // distinct handles
	EXPECT_EQ(pool.str(pub), "\\$foo");
	EXPECT_EQ(pool.str(priv), "$foo");
}

TEST(TwinePublicityTest, InterningIsStableAcrossTags)
{
	TwinePool pool;
	IdString a = pool.add(std::string("\\foo"));
	IdString b = pool.add(std::string("\\foo"));
	EXPECT_EQ(a, b);
}

TEST(TwinePublicityTest, SuffixInheritsPublicity)
{
	TwinePool pool;
	IdString pub = pool.add(std::string("\\base"));
	IdString priv = pool.add(std::string("$base"));

	IdString pub_sfx = pool.add(Twine{Twine::Suffix{pub, "_1"}});
	IdString priv_sfx = pool.add(Twine{Twine::Suffix{priv, "_1"}});

	EXPECT_TRUE(twine_is_public(pub_sfx));
	EXPECT_FALSE(twine_is_public(priv_sfx));
	EXPECT_EQ(pool.str(pub_sfx), "\\base_1");
	EXPECT_EQ(pool.str(priv_sfx), "$base_1");
}

TEST(TwinePublicityTest, StaticHandlesAreTagged)
{
	TwinePool pool;
	EXPECT_TRUE(twine_is_public(ID::A));
	EXPECT_EQ(pool.str(ID::A), "\\A");
	EXPECT_EQ(pool.unescaped_str(ID::A), "A");
	EXPECT_FALSE(twine_is_public(ID($and)));
	EXPECT_EQ(pool.str(ID($and)), "$and");
}

TEST(TwinePublicityTest, LookupReturnsTaggedHandle)
{
	TwinePool pool;
	IdString pub = pool.add(std::string("\\net"));
	IdString priv = pool.add(std::string("$net"));

	TwineSearch search(&pool);
	EXPECT_EQ(search.find("\\net"), pub);
	EXPECT_EQ(search.find("$net"), priv);
	EXPECT_EQ(search.find("\\A"), ID::A);
	EXPECT_EQ(search.find("\\nonexistent"), Twine::Null);
}

TEST(TwinePublicityTest, CopyFromPreservesTag)
{
	TwinePool src, dst;
	IdString pub = src.add(std::string("\\xfer"));
	IdString copied = dst.copy_from(src, pub);
	EXPECT_TRUE(twine_is_public(copied));
	EXPECT_EQ(dst.str(copied), "\\xfer");
	// Static handles pass through tag and all.
	EXPECT_EQ(dst.copy_from(src, ID::A), ID::A);
}

TEST(TwinePublicityTest, GcKeepsTaggedRoots)
{
	TwinePool pool;
	IdString pub = pool.add(std::string("\\keep"));
	pool.add(std::string("\\drop"));
	std::vector<IdString> roots{pub};
	EXPECT_EQ(pool.gc(roots), 1u);
	EXPECT_EQ(pool.str(pub), "\\keep");
}

TEST(TwinePublicityTest, WireNameMasquerade)
{
	RTLIL::Design design;
	RTLIL::Module *mod = design.addModule(design.twines.add(std::string("\\top")));

	RTLIL::Wire *pub = mod->addWire(design.twines.add(std::string("\\sig")));
	RTLIL::Wire *priv = mod->addWire(design.twines.add(std::string("$sig")));

	EXPECT_TRUE(pub->name.isPublic());
	EXPECT_FALSE(priv->name.isPublic());
	EXPECT_EQ(pub->name.escaped(), "\\sig");
	EXPECT_EQ(pub->name.unescape(), "sig");
	EXPECT_EQ(pub->name.str(), "\\sig");
	EXPECT_EQ(priv->name.escaped(), "$sig");
	EXPECT_EQ(priv->name.unescape(), "$sig");

	// Distinct dict keys despite shared content.
	EXPECT_NE(pub, priv);
	TwineSearch search(&design.twines);
	EXPECT_EQ(mod->wire(search.find("\\sig")), pub);
	EXPECT_EQ(mod->wire(search.find("$sig")), priv);

	// uniquify keeps publicity.
	IdString uniq = mod->uniquify(pub->meta_->name);
	EXPECT_TRUE(twine_is_public(uniq));
	EXPECT_EQ(design.twines.str(uniq), "\\sig_1");
}

YOSYS_NAMESPACE_END
