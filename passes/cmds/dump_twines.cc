/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Inspect the design-level name and src pools.
 */

#include "kernel/register.h"
#include "kernel/rtlil.h"
#include "kernel/twine.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct DumpTwinesPass : public Pass {
	DumpTwinesPass() : Pass("dump_twines", "dump the design-level name and src pools") { }

	void help() override
	{
		log("\n");
		log("    dump_twines [-flat]\n");
		log("\n");
		log("Print every node in design->twines and design->srcs. Leaves show\n");
		log("their literal string, sets show their child id list. With -flat\n");
		log("each node is additionally rendered as the string a backend would\n");
		log("emit.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		bool flat = false;
		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-flat") {
				flat = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		const TwinePool &pool = design->twines;
		log("twine pool: %zu local nodes\n", pool.size());
		for (size_t idx = 0; idx < pool.backing.size(); ++idx) {
			IdString id = STATIC_TWINE_END + idx;
			const Twine &n = pool.backing[idx];
			if (n.is_leaf()) {
				log("  @%zu leaf \"%s\"", (size_t)id, n.leaf().c_str());
			} else if (n.is_suffix()) {
				log("  @%zu suffix @%zu + \"%s\"", (size_t)id,
						(size_t)n.suffix().prefix, n.suffix().tail.c_str());
			} else {
				log("  @%zu dead", (size_t)id);
			}
			if (flat)
				log(" -> \"%s\"", pool.str(id).c_str());
			log("\n");
		}

		const SrcPool &srcs = design->srcs;
		log("src pool: %zu nodes\n", srcs.size());
		for (size_t idx = 0; idx < srcs.backing.size(); ++idx) {
			const Src &n = srcs.backing[idx];
			if (n.is_dead()) {
				log("  @%zu dead", idx);
			} else {
				std::string members;
				for (IdString m : n.members()) {
					if (!members.empty())
						members += ", ";
					members += "@" + std::to_string(m.value);
				}
				log("  @%zu set [%s]", idx, members.c_str());
			}
			if (flat)
				log(" -> \"%s\"", srcs.str(SrcRef(idx)).c_str());
			log("\n");
		}
	}
} DumpTwinesPass;

struct GcTwinesPass : public Pass {
	GcTwinesPass() : Pass("gc_twines", "reap unreferenced entries from the name and src pools") { }

	void help() override
	{
		log("\n");
		log("    gc_twines\n");
		log("\n");
		log("Walk the design, collect every name and src handle reachable from\n");
		log("a live object, and drop every pool node that nothing refers to.\n");
		log("Surviving nodes keep their ids, so the design is unchanged.\n");
		log("\n");
		log("Useful after long opt_merge / techmap runs that leave intermediate\n");
		log("src sets orphaned: each merge splices a previous set's members\n");
		log("into the new node, so the prior set becomes unreferenced as soon\n");
		log("as the surviving cell's src is rewritten.\n");
		log("\n");
	}

	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		extra_args(args, 1, design);
		size_t before = design->twines.size() + design->srcs.size();
		size_t freed = design->gc_twines();
		log("twine gc: %zu nodes -> %zu (%zu freed)\n",
				before, design->twines.size() + design->srcs.size(), freed);
	}
} GcTwinesPass;

PRIVATE_NAMESPACE_END
