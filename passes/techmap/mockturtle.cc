/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2026  Emil J. Tywoniak <emil@tywoniak.eu>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// [[CITE]] mockturtle
// EPFL Logic Synthesis Libraries, mockturtle: C++ logic network library
// https://github.com/lsils/mockturtle

#include <mockturtle/networks/aig.hpp>
#include <mockturtle/networks/klut.hpp>
#include <mockturtle/views/mapping_view.hpp>
#include <mockturtle/views/topo_view.hpp>
#include <mockturtle/views/cell_view.hpp>
#include <mockturtle/networks/block.hpp>
#include <mockturtle/algorithms/aig_balancing.hpp>
#include <mockturtle/algorithms/rewrite.hpp>
#include <mockturtle/algorithms/node_resynthesis/xag_npn.hpp>
#include <mockturtle/algorithms/resubstitution.hpp>
#include <mockturtle/algorithms/aig_resub.hpp>
#include <mockturtle/algorithms/cleanup.hpp>
#include <mockturtle/algorithms/refactoring.hpp>
#include <mockturtle/algorithms/node_resynthesis/sop_factoring.hpp>
#include <mockturtle/algorithms/choices.hpp>
#include <mockturtle/views/choice_view.hpp>
#include <mockturtle/algorithms/lut_mapper.hpp>
#include <mockturtle/algorithms/emap.hpp>
#include <mockturtle/io/genlib_reader.hpp>
#include <mockturtle/utils/tech_library.hpp>
#include <lorina/genlib.hpp>

#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include "kernel/gzip.h"
#include "libparse.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

using Aig = mockturtle::aig_network;
using AigSignal = Aig::signal;
using AigNode = Aig::node;
using MappedAig = mockturtle::mapping_view<Aig, true>;
using BlockNet = mockturtle::cell_view<mockturtle::block_network>;
using BlockNode = mockturtle::block_network::node;
using BlockSignal = mockturtle::block_network::signal;
using TechLibrary = mockturtle::tech_library<6>;
using NpnResynthesis = mockturtle::xag_npn_resynthesis<Aig, Aig, mockturtle::xag_npn_db_kind::aig_complete>;
using RewriteLibrary = mockturtle::exact_library<Aig>;

// lut_map_inplace takes the cost model as a type, so the table is global
struct LutCost {
	static std::vector<std::pair<uint32_t, uint32_t>> table;

	std::pair<uint32_t, uint32_t> operator()(uint32_t num_leaves) const
	{
		if (num_leaves == 0)
			return {0, 0};
		log_assert(num_leaves <= table.size());
		return table[num_leaves - 1];
	}

	std::pair<uint32_t, uint32_t> operator()(const kitty::dynamic_truth_table &tt) const
	{
		return (*this)(tt.num_vars());
	}
};
std::vector<std::pair<uint32_t, uint32_t>> LutCost::table;

struct AigNodeCollector {
	std::vector<AigNode> &out;
	void operator()(AigNode n) const { out.push_back(n); }
};

struct BlockNodeCollector {
	std::vector<BlockNode> &out;
	void operator()(BlockNode n) const { out.push_back(n); }
};

struct BlockFaninCollector {
	std::vector<BlockSignal> &out;
	void operator()(const BlockSignal &f) const { out.push_back(f); }
};

// std::cerr is where mockturtle reports; route it through the log
struct CerrCapture {
	std::stringstream buf;
	std::streambuf *saved;
	CerrCapture() : saved(std::cerr.rdbuf(buf.rdbuf())) {}
	~CerrCapture()
	{
		std::cerr.rdbuf(saved);
		std::string line;
		int count = 0;
		while (std::getline(buf, line))
			if (++count <= 20 || line.find("ERROR") != std::string::npos)
				log("mockturtle: %s\n", line);
		if (count > 20)
			log("mockturtle: (%d more lines)\n", count - 20);
	}
};

// timing of cells that stay in place, in ns: clock-to-output arrival and input setup
struct CellTiming {
	dict<std::pair<IdString, IdString>, double> arrival, setup;
};

struct CellMapOptions {
	bool area_oriented = false;
	bool multioutput = false;
	bool load_aware = true;
	bool choices_keep_delay = true;
	int choice_cut_limit = 16;
	double wire_load = 0, fanout_load = 0;
	int fanout_limit = 8;
	double period = 0, io_delay = 0;
	const CellTiming *timing = nullptr;
};

static uint64_t block_key(BlockNode n, uint32_t pin)
{
	return (uint64_t(n) << 8) | pin;
}

static const char *const_gate_name(bool value)
{
	return value ? "$__mockturtle_const1" : "$__mockturtle_const0";
}

// constants inside the mapping become plain constant drivers, like ABC leaves them for hilomap
static void add_const_gates(std::vector<mockturtle::gate> &gates)
{
	std::string genlib;
	for (bool value : {false, true})
		genlib += stringf("GATE %s 0 Y=CONST%d;\n", const_gate_name(value), value);
	std::istringstream f(genlib);
	if (lorina::read_genlib(f, mockturtle::genlib_reader(gates)) != lorina::return_code::success)
		log_abort();
}

static bool is_gate_type(IdString type)
{
	return type.in(ID($_BUF_), ID($_NOT_), ID($_AND_), ID($_NAND_), ID($_OR_), ID($_NOR_),
			ID($_XOR_), ID($_XNOR_), ID($_ANDNOT_), ID($_ORNOT_), ID($_MUX_), ID($_NMUX_),
			ID($_AOI3_), ID($_OAI3_), ID($_AOI4_), ID($_OAI4_), ID($buf));
}

struct Worker {
	Module *module;
	SigMap sigmap;
	bool keep_names;
	int uid;

	Aig aig, extracted;
	pool<Cell *> logic_cells;
	dict<SigBit, std::pair<Cell *, int>> logic_driver;
	dict<SigBit, std::pair<IdString, IdString>> ext_driver;
	dict<SigBit, double> co_required;
	dict<SigBit, AigSignal> bit2sig;
	std::vector<SigBit> ci_bits, co_bits;
	pool<SigBit> co_set;
	dict<uint64_t, int> po_of_signal;
	std::vector<int> co_po;
	std::vector<SigBit> co_direct;
	int wire_count = 0, cell_count = 0;
	std::map<IdString, int> cell_stats;

	Worker(Module *module, bool keep_names) :
		module(module), sigmap(module), keep_names(keep_names), uid(autoidx++) {}

	bool is_ci(SigBit bit) const
	{
		if (bit.wire->port_input && !bit.wire->port_output)
			return true;
		return !logic_driver.count(bit);
	}

	void index_logic()
	{
		for (auto cell : module->cells()) {
			if (cell->type == ID($input_port)) {
				logic_cells.insert(cell);
				continue;
			}
			if (!is_gate_type(cell->type) || cell->has_keep_attr()) {
				for (auto &conn : cell->connections())
					if (cell->port_dir(conn.first) == RTLIL::PD_OUTPUT)
						for (auto bit : sigmap(conn.second))
							if (bit.wire)
								ext_driver[bit] = {cell->type, conn.first};
				continue;
			}
			logic_cells.insert(cell);
			SigSpec y = sigmap(cell->getPort(ID::Y));
			for (int i = 0; i < GetSize(y); i++)
				if (y[i].wire)
					logic_driver[y[i]] = {cell, i};
		}
	}

	void add_co(SigBit bit, double required)
	{
		bit = sigmap(bit);
		if (!bit.wire || is_ci(bit))
			return;
		if (co_set.insert(bit).second)
			co_bits.push_back(bit);
		auto it = co_required.find(bit);
		if (it == co_required.end())
			co_required[bit] = required;
		else
			it->second = std::min(it->second, required);
	}

	bool is_buf_driven(SigBit bit)
	{
		auto it = logic_driver.find(sigmap(bit));
		if (it == logic_driver.end())
			return false;
		return it->second.first->type.in(ID($buf), ID($_BUF_));
	}

	void collect_cos(const CellMapOptions &opts)
	{
		for (auto &port : module->ports) {
			Wire *w = module->wire(port);
			if (w->port_output)
				for (auto bit : SigSpec(w))
					add_co(bit, opts.period - opts.io_delay);
		}

		std::vector<Cell *> cells = module->cells().to_vector();
		std::sort(cells.begin(), cells.end(), IdString::compare_ptr_by_name<Cell>());
		for (auto cell : cells) {
			if (logic_cells.count(cell))
				continue;
			for (auto &conn : cell->connections()) {
				if (cell->port_dir(conn.first) == RTLIL::PD_OUTPUT)
					continue;
				double setup = 0;
				if (opts.timing) {
					auto it = opts.timing->setup.find({cell->type, conn.first});
					if (it != opts.timing->setup.end())
						setup = it->second;
				}
				for (auto bit : conn.second)
					add_co(bit, opts.period - setup);
			}
		}

		std::vector<Wire *> wires = module->wires().to_vector();
		std::sort(wires.begin(), wires.end(), IdString::compare_ptr_by_name<Wire>());
		for (auto w : wires) {
			bool keep = w->get_bool_attribute(ID::keep) || (keep_names && w->name.isPublic());
			for (auto bit : SigSpec(w))
				if (keep || (w->name.isPublic() && is_buf_driven(bit)))
					add_co(bit, opts.period);
		}
	}

	double ci_arrival(SigBit bit, const CellMapOptions &opts) const
	{
		if (bit.wire->port_input)
			return opts.io_delay;
		auto drv = ext_driver.find(bit);
		if (drv == ext_driver.end() || !opts.timing)
			return 0;
		auto it = opts.timing->arrival.find(drv->second);
		return it == opts.timing->arrival.end() ? 0 : it->second;
	}

	std::vector<SigBit> gate_inputs(Cell *cell, int offset)
	{
		if (cell->type == ID($buf))
			return {sigmap(cell->getPort(ID::A)[offset])};
		std::vector<SigBit> bits;
		for (auto port : {ID::A, ID::B, ID::C, ID::D, ID::S})
			if (cell->hasPort(port))
				bits.push_back(sigmap(cell->getPort(port)[0]));
		return bits;
	}

	AigSignal eval_gate(Cell *cell, const std::vector<AigSignal> &in)
	{
		IdString t = cell->type;
		if (t.in(ID($buf), ID($_BUF_)))
			return in[0];
		if (t == ID($_NOT_))
			return !in[0];
		if (t == ID($_AND_))
			return aig.create_and(in[0], in[1]);
		if (t == ID($_NAND_))
			return aig.create_nand(in[0], in[1]);
		if (t == ID($_OR_))
			return aig.create_or(in[0], in[1]);
		if (t == ID($_NOR_))
			return aig.create_nor(in[0], in[1]);
		if (t == ID($_XOR_))
			return aig.create_xor(in[0], in[1]);
		if (t == ID($_XNOR_))
			return aig.create_xnor(in[0], in[1]);
		if (t == ID($_ANDNOT_))
			return aig.create_and(in[0], !in[1]);
		if (t == ID($_ORNOT_))
			return aig.create_or(in[0], !in[1]);
		if (t == ID($_MUX_))
			return aig.create_ite(in[2], in[1], in[0]);
		if (t == ID($_NMUX_))
			return !aig.create_ite(in[2], in[1], in[0]);
		if (t == ID($_AOI3_))
			return aig.create_nor(aig.create_and(in[0], in[1]), in[2]);
		if (t == ID($_OAI3_))
			return aig.create_nand(aig.create_or(in[0], in[1]), in[2]);
		if (t == ID($_AOI4_))
			return aig.create_nor(aig.create_and(in[0], in[1]), aig.create_and(in[2], in[3]));
		if (t == ID($_OAI4_))
			return aig.create_nand(aig.create_or(in[0], in[1]), aig.create_or(in[2], in[3]));
		log_abort();
	}

	AigSignal signal_of(SigBit root)
	{
		root = sigmap(root);
		std::vector<SigBit> stack = {root};
		pool<SigBit> expanding;
		while (!stack.empty()) {
			SigBit bit = stack.back();
			if (bit2sig.count(bit)) {
				stack.pop_back();
				continue;
			}
			if (!bit.wire) {
				bit2sig[bit] = aig.get_constant(bit.data == State::S1);
				stack.pop_back();
				continue;
			}
			if (is_ci(bit)) {
				bit2sig[bit] = aig.create_pi();
				ci_bits.push_back(bit);
				stack.pop_back();
				continue;
			}
			auto [cell, offset] = logic_driver.at(bit);
			std::vector<SigBit> inputs = gate_inputs(cell, offset);
			bool ready = true;
			for (auto in_bit : inputs) {
				if (bit2sig.count(in_bit))
					continue;
				if (expanding.count(in_bit))
					log_error("Combinational loop through %s in module %s.\n", log_signal(in_bit), log_id(module));
				stack.push_back(in_bit);
				ready = false;
			}
			if (!ready) {
				expanding.insert(bit);
				continue;
			}
			std::vector<AigSignal> in_sigs;
			for (auto in_bit : inputs)
				in_sigs.push_back(bit2sig.at(in_bit));
			bit2sig[bit] = eval_gate(cell, in_sigs);
			expanding.erase(bit);
			stack.pop_back();
		}
		return bit2sig.at(root);
	}

	void build(const CellMapOptions &opts)
	{
		index_logic();
		collect_cos(opts);
		for (auto bit : co_bits) {
			AigSignal f = signal_of(bit);
			AigNode n = aig.get_node(f);
			if (aig.is_constant(n)) {
				co_po.push_back(-1);
				co_direct.push_back(aig.is_complemented(f) ? State::S1 : State::S0);
				continue;
			}
			if (aig.is_pi(n) && !aig.is_complemented(f)) {
				co_po.push_back(-1);
				co_direct.push_back(ci_bits[aig.pi_index(n)]);
				continue;
			}
			auto r = po_of_signal.insert({f.data, GetSize(po_of_signal)});
			if (r.second)
				aig.create_po(f);
			co_po.push_back(r.first->second);
			co_direct.push_back(SigBit());
		}
		log("Extracted %d AND gates from module `%s' to a network with %d inputs and %d outputs.\n",
				int(aig.num_gates()), log_id(module), int(aig.num_pis()), int(aig.num_pos()));
	}

	void optimize(const std::vector<std::string> &steps, RewriteLibrary &rewrite_lib, bool keep_extracted)
	{
		if (keep_extracted)
			extracted = mockturtle::cleanup_dangling(aig);
		for (auto &step : steps) {
			if (step == "balance") {
				mockturtle::aig_balance(aig);
			} else if (step == "rewrite") {
				mockturtle::rewrite_params ps;
				mockturtle::rewrite(aig, rewrite_lib, ps);
			} else if (step == "resub") {
				mockturtle::resubstitution_params ps;
				mockturtle::aig_resubstitution(aig, ps);
			} else {
				log_abort();
			}
			aig = mockturtle::cleanup_dangling(aig);
			log("After %s: %d AND gates.\n", step, int(aig.num_gates()));
		}
	}

	// one more optimization pass on a copy, roughly ABC's compress2
	Aig compress(const Aig &src, RewriteLibrary &rewrite_lib)
	{
		Aig v = mockturtle::cleanup_dangling(src);
		mockturtle::rewrite_params rps;
		rps.allow_zero_gain = true;
		mockturtle::rewrite(v, rewrite_lib, rps);
		v = mockturtle::cleanup_dangling(v);
		mockturtle::refactoring_params fps;
		fps.allow_zero_gain = true;
		mockturtle::sop_factoring<Aig> resyn;
		mockturtle::refactoring(v, resyn, fps);
		v = mockturtle::cleanup_dangling(v);
		mockturtle::aig_balance(v);
		return mockturtle::cleanup_dangling(v);
	}

	std::vector<Aig> build_variants(int count, RewriteLibrary &rewrite_lib)
	{
		std::vector<Aig> variants;
		if (count >= 4) {
			mockturtle::aig_balance(extracted);
			variants.push_back(mockturtle::cleanup_dangling(extracted));
		}
		variants.push_back(aig);
		for (int i = 1; i < std::min(count, 3); i++)
			variants.push_back(compress(variants.back(), rewrite_lib));
		for (int i = 0; i < GetSize(variants); i++)
			log("Variant %d: %d AND gates.\n", i, int(variants[i].num_gates()));
		return variants;
	}

	void map_cells_with_choices(int count, RewriteLibrary &rewrite_lib, const TechLibrary &lib, const CellMapOptions &opts, const mockturtle::choices_params &cps)
	{
		std::vector<Aig> variants = build_variants(count, rewrite_lib);
		mockturtle::choice_view<Aig> choice_ntk;
		mockturtle::choices_stats st;
		{
			CerrCapture capture;
			mockturtle::compute_choices(variants, choice_ntk, cps, &st);
		}
		log("Choices: %u AND gates in the union of %d variants, %u in the choice network.\n", st.union_size, GetSize(variants), st.result_size);
		log("Choices: %u classes with %u choices, %u constants; merged %u, SAT %u, UNSAT %u, timeouts %u (%.2f s: SAT %.2f s, simulation %.2f s).\n",
				st.num_classes, st.num_choices, st.num_constants, st.num_merged, st.num_sat, st.num_unsat, st.num_timeouts,
				mockturtle::to_seconds(st.time_total), mockturtle::to_seconds(st.time_sat), mockturtle::to_seconds(st.time_sim));
		if (st.num_rejected_tfi || st.num_rejected_used)
			log("Choices: rejected %u members with the representative in their fanin and %u already used ones.\n", st.num_rejected_tfi, st.num_rejected_used);
		map_cells(choice_ntk, lib, opts);
	}

	std::string new_name(const char *kind)
	{
		return stringf("$mockturtle$%d$%s%d", uid, kind, kind[0] == 'n' ? ++wire_count : ++cell_count);
	}

	SigBit make_lut(const SigSpec &inputs, const Const &lut)
	{
		Wire *y = module->addWire(new_name("n"));
		Cell *cell = module->addLut(new_name("lut"), inputs, y, lut);
		cell_stats[cell->type]++;
		return y;
	}

	SigBit make_lut(const SigSpec &inputs, const kitty::dynamic_truth_table &tt)
	{
		if (GetSize(inputs) == 0)
			return kitty::get_bit(tt, 0) ? State::S1 : State::S0;
		Const lut(State::S0, 1 << GetSize(inputs));
		for (int i = 0; i < GetSize(lut); i++)
			if (kitty::get_bit(tt, i))
				lut.set(i, State::S1);
		return make_lut(inputs, lut);
	}

	SigBit make_not(SigBit a)
	{
		return make_lut(a, Const::from_string("01"));
	}

	void map_luts(int width, bool area_oriented, int required_delay)
	{
		MappedAig mapped{aig};
		mockturtle::lut_map_params ps;
		ps.cut_enumeration_ps.cut_size = width;
		ps.area_oriented_mapping = area_oriented;
		ps.required_delay = required_delay;
		mockturtle::lut_map_stats st;
		mockturtle::lut_map_inplace<MappedAig, true, LutCost>(mapped, ps, &st);

		std::vector<AigNode> order;
		mockturtle::topo_view<MappedAig> topo{mapped};
		topo.foreach_node(AigNodeCollector{order});

		std::vector<uint8_t> need(aig.size(), 0);
		for (uint32_t i = 0; i < aig.num_pos(); i++) {
			AigSignal f = aig.po_at(i);
			need[aig.get_node(f)] |= aig.is_complemented(f) ? 2 : 1;
		}
		for (auto n : order) {
			if (!mapped.is_cell_root(n))
				continue;
			std::vector<AigNode> leaves;
			mapped.foreach_cell_fanin(n, AigNodeCollector{leaves});
			for (auto l : leaves)
				need[l] |= 1;
		}

		dict<AigNode, SigBit> pos, neg;
		for (auto n : order) {
			if (aig.is_constant(n)) {
				pos[n] = State::S0;
				neg[n] = State::S1;
				continue;
			}
			if (aig.is_pi(n)) {
				pos[n] = ci_bits[aig.pi_index(n)];
				if (need[n] & 2)
					neg[n] = make_not(pos[n]);
				continue;
			}
			if (!mapped.is_cell_root(n) || need[n] == 0)
				continue;
			std::vector<AigNode> leaves;
			mapped.foreach_cell_fanin(n, AigNodeCollector{leaves});
			kitty::dynamic_truth_table tt = mapped.cell_function(n);
			if (tt.num_vars() != leaves.size()) {
				for (uint32_t i = leaves.size(); i < tt.num_vars(); i++)
					if (kitty::has_var(tt, i))
						log_error("LUT function of node %d depends on a variable outside its %d leaves.\n", int(n), int(leaves.size()));
				if (tt.num_vars() < leaves.size())
					log_error("LUT function of node %d has fewer variables than its %d leaves.\n", int(n), int(leaves.size()));
				tt = kitty::shrink_to(tt, leaves.size());
			}
			SigSpec inputs;
			for (auto l : leaves)
				inputs.append(pos.at(l));
			if (need[n] & 1)
				pos[n] = make_lut(inputs, tt);
			if (need[n] & 2)
				neg[n] = make_lut(inputs, ~tt);
		}

		std::vector<SigBit> po_sig;
		for (uint32_t i = 0; i < aig.num_pos(); i++) {
			AigSignal f = aig.po_at(i);
			AigNode n = aig.get_node(f);
			po_sig.push_back(aig.is_complemented(f) ? neg.at(n) : pos.at(n));
		}
		connect_cos(po_sig);

		log("MOCKTURTLE RESULTS:   mapped area: %8u   depth: %8u   edges: %8u\n", st.area, st.delay, st.edges);
	}

	// a node carries one function per used output; the cell's other outputs stay unconnected
	std::vector<SigBit> make_cell(const mockturtle::standard_cell &c, const SigSpec &inputs, const std::vector<kitty::dynamic_truth_table> &functions)
	{
		const mockturtle::gate &g = c.gates.front();
		for (bool value : {false, true})
			if (c.name == const_gate_name(value))
				return {SigBit(value ? State::S1 : State::S0)};
		if (GetSize(inputs) != GetSize(g.pins))
			log_error("Cell '%s' has %d pins but the mapped node has %d fanins.\n", c.name, GetSize(g.pins), GetSize(inputs));
		Cell *cell = module->addCell(new_name("g"), IdString(RTLIL::escape_id(c.name)));
		for (int i = 0; i < GetSize(inputs); i++)
			cell->setPort(IdString(RTLIL::escape_id(g.pins[i].name)), inputs[i]);
		std::vector<SigBit> outputs;
		pool<int> used;
		for (auto &f : functions) {
			int k = -1;
			for (int i = 0; i < GetSize(c.gates) && k < 0; i++)
				if (!used.count(i) && c.gates[i].function == f)
					k = i;
			if (k < 0)
				log_error("Cell '%s' has no output with the function of the mapped node.\n", c.name);
			used.insert(k);
			Wire *y = module->addWire(new_name("n"));
			cell->setPort(IdString(RTLIL::escape_id(c.gates[k].output_name)), y);
			outputs.push_back(y);
		}
		for (int i = 0; i < GetSize(c.gates); i++)
			if (!used.count(i))
				cell->setPort(IdString(RTLIL::escape_id(c.gates[i].output_name)), module->addWire(new_name("n")));
		cell_stats[cell->type]++;
		return outputs;
	}

	std::vector<kitty::dynamic_truth_table> node_functions(const BlockNet &res, BlockNode n)
	{
		std::vector<kitty::dynamic_truth_table> functions;
		for (uint32_t k = 0; k < res.num_outputs(n); k++)
			functions.push_back(res.node_function_pin(n, k));
		return functions;
	}

	template<class Net>
	void map_cells(const Net &net, const TechLibrary &lib, const CellMapOptions &opts)
	{
		mockturtle::emap_params ps;
		ps.area_oriented_mapping = opts.area_oriented;
		ps.map_multioutput = opts.multioutput;
		ps.load_aware_delay = opts.load_aware;
		ps.wire_load = opts.wire_load;
		ps.fanout_load = opts.fanout_load;
		ps.fanout_limit = opts.fanout_limit;
		ps.choices_keep_delay = opts.choices_keep_delay;
		ps.choice_cut_limit = opts.choice_cut_limit;
		if (opts.area_oriented) {
			ps.required_time = std::numeric_limits<float>::max();
		} else if (opts.period > 0) {
			for (auto bit : ci_bits)
				ps.arrival_times.push_back(ci_arrival(bit, opts));
			ps.required_times.assign(net.num_pos(), opts.period);
			for (int i = 0; i < GetSize(co_bits); i++)
				if (co_po[i] >= 0)
					ps.required_times[co_po[i]] = std::min(ps.required_times[co_po[i]], co_required.at(co_bits[i]));
		}
		mockturtle::emap_stats st;
		std::unique_ptr<BlockNet> res;
		{
			CerrCapture capture;
			res = std::make_unique<BlockNet>(mockturtle::emap<6>(net, lib, ps, &st));
		}
		if (st.mapping_error)
			log_error("Technology mapping of module %s failed.\n", log_id(module));

		dict<uint64_t, SigBit> sig;
		BlockNode c0 = res->get_node(res->get_constant(false));
		BlockNode c1 = res->get_node(res->get_constant(true));
		sig[block_key(c0, 0)] = res->has_cell(c0) ? make_cell(res->get_cell(c0), SigSpec(), node_functions(*res, c0)).front() : SigBit(State::S0);
		sig[block_key(c1, 0)] = res->has_cell(c1) ? make_cell(res->get_cell(c1), SigSpec(), node_functions(*res, c1)).front() : SigBit(State::S1);
		for (uint32_t i = 0; i < res->num_pis(); i++)
			sig[block_key(res->pi_at(i), 0)] = ci_bits[i];

		std::vector<BlockNode> order;
		mockturtle::topo_view<BlockNet> topo{*res};
		topo.foreach_node(BlockNodeCollector{order});
		for (auto n : order) {
			if (res->is_constant(n) || res->is_pi(n))
				continue;
			if (!res->has_cell(n))
				log_error("Mapped node %d in module %s has no cell binding.\n", int(n), log_id(module));
			std::vector<BlockSignal> fanins;
			res->foreach_fanin(n, BlockFaninCollector{fanins});
			SigSpec inputs;
			for (auto f : fanins) {
				if (res->is_complemented(f))
					log_error("Mapped node %d in module %s has a complemented fanin.\n", int(n), log_id(module));
				inputs.append(sig.at(block_key(res->get_node(f), res->get_output_pin(f))));
			}
			std::vector<SigBit> outputs = make_cell(res->get_cell(n), inputs, node_functions(*res, n));
			for (int k = 0; k < GetSize(outputs); k++)
				sig[block_key(n, k)] = outputs[k];
		}

		std::vector<SigBit> po_sig;
		for (uint32_t i = 0; i < res->num_pos(); i++) {
			BlockSignal f = res->po_at(i);
			if (res->is_complemented(f))
				log_error("Output %d of module %s is complemented.\n", int(i), log_id(module));
			po_sig.push_back(sig.at(block_key(res->get_node(f), res->get_output_pin(f))));
		}
		connect_cos(po_sig);

		log("MOCKTURTLE RESULTS:   mapped area: %8.2f   delay: %8.2f   multi-output cells: %8u\n", res->compute_area(), st.delay, st.multioutput_gates);
		if (st.choice_cuts)
			log("MOCKTURTLE RESULTS:   cells implementing a choice cut: %8u\n", st.choice_cuts);
	}

	void connect_cos(const std::vector<SigBit> &po_sig)
	{
		for (int i = 0; i < GetSize(co_bits); i++)
			module->connect(co_bits[i], co_po[i] < 0 ? co_direct[i] : po_sig[co_po[i]]);
	}

	void finish()
	{
		if (aig.num_pos() == 0)
			connect_cos({});
		for (auto cell : logic_cells)
			module->remove(cell);
		for (auto &it : cell_stats)
			log("MOCKTURTLE RESULTS:   %15s cells: %8d\n", log_id(it.first), it.second);
		log("MOCKTURTLE RESULTS:           input signals: %8d\n", GetSize(ci_bits));
		log("MOCKTURTLE RESULTS:          output signals: %8d\n", GetSize(co_bits));
	}
};

static std::vector<std::pair<uint32_t, uint32_t>> lut_costs_from_file(const std::string &path)
{
	std::ifstream f(path);
	if (!f.is_open())
		log_error("Can't open LUT library file `%s'.\n", path);

	std::map<int, std::pair<uint32_t, uint32_t>> entries;
	std::string line;
	while (std::getline(f, line)) {
		size_t hash = line.find('#');
		if (hash != std::string::npos)
			line.erase(hash);
		auto tok = split_tokens(line);
		if (tok.empty())
			continue;
		if (tok.size() < 2)
			log_error("Malformed LUT library line `%s'.\n", line);
		int k = atoi(tok[0].c_str());
		if (k < 1 || k > 16)
			log_error("Unsupported LUT size %d in `%s'.\n", k, path);
		uint32_t delay = 0;
		for (size_t i = 2; i < tok.size(); i++)
			delay = std::max(delay, uint32_t(atoi(tok[i].c_str())));
		entries[k] = {uint32_t(atoi(tok[1].c_str())), delay ? delay : 1};
	}
	if (entries.empty())
		log_error("LUT library `%s' has no entries.\n", path);

	std::vector<std::pair<uint32_t, uint32_t>> table(entries.rbegin()->first);
	for (int k = GetSize(table); k >= 1; k--)
		table[k - 1] = entries.lower_bound(k)->second;
	return table;
}

static std::vector<std::pair<uint32_t, uint32_t>> lut_costs_from_widths(const std::string &lut_arg, const std::string &luts_arg)
{
	std::vector<int> lut_costs;
	if (!lut_arg.empty()) {
		size_t pos = lut_arg.find_first_of(':');
		int lut_mode = 0, lut_mode2 = 0;
		if (pos != std::string::npos) {
			lut_mode = atoi(lut_arg.substr(0, pos).c_str());
			lut_mode2 = atoi(lut_arg.substr(pos + 1).c_str());
		} else {
			lut_mode = atoi(lut_arg.c_str());
			lut_mode2 = lut_mode;
		}
		for (int i = 0; i < lut_mode; i++)
			lut_costs.push_back(1);
		for (int i = lut_mode; i < lut_mode2; i++)
			lut_costs.push_back(2 << (i - lut_mode));
	}
	if (!luts_arg.empty()) {
		lut_costs.clear();
		for (auto &tok : split_tokens(luts_arg, ",")) {
			auto parts = split_tokens(tok, ":");
			if (GetSize(parts) == 0 && !lut_costs.empty())
				lut_costs.push_back(lut_costs.back());
			else if (GetSize(parts) == 1)
				lut_costs.push_back(atoi(parts.at(0).c_str()));
			else if (GetSize(parts) == 2)
				while (GetSize(lut_costs) < atoi(parts.at(0).c_str()))
					lut_costs.push_back(atoi(parts.at(1).c_str()));
			else
				log_cmd_error("Invalid -luts syntax.\n");
		}
	}
	if (lut_costs.empty() || GetSize(lut_costs) > 16)
		log_cmd_error("LUT width must be between 1 and 16.\n");

	std::vector<std::pair<uint32_t, uint32_t>> table;
	for (int cost : lut_costs)
		table.push_back({uint32_t(cost), 1});
	return table;
}

static std::string unquote(std::string s)
{
	if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
		s = s.substr(1, s.size() - 2);
	return s;
}

static std::string genlib_expr(const LibertyExpression &e)
{
	std::string op;
	switch (e.kind) {
	case LibertyExpression::Kind::PIN:
		return e.name;
	case LibertyExpression::Kind::NOT:
		return "!" + genlib_expr(e.children[0]);
	case LibertyExpression::Kind::AND:
		op = "*";
		break;
	case LibertyExpression::Kind::OR:
		op = "+";
		break;
	case LibertyExpression::Kind::XOR:
		op = "^";
		break;
	case LibertyExpression::Kind::EMPTY:
		return "";
	}
	std::string s = "(";
	for (int i = 0; i < GetSize(e.children); i++)
		s += (i ? op : "") + genlib_expr(e.children[i]);
	return s + ")";
}

struct LibertyUnits {
	double time_scale = 1;
	double cap_scale = 1;
	dict<std::string, int> load_axis;
};

// delay = d0 + k * load, in ns and fF
struct LibertyArc {
	double d0 = 0, k = 0;
	bool valid = false;
};

static double liberty_time_scale(const LibertyAst *library)
{
	const LibertyAst *unit = library->find("time_unit");
	if (!unit)
		return 1.0;
	std::string u = unquote(unit->value);
	double scale = atof(u.c_str());
	if (u.find("ps") != std::string::npos)
		return scale / 1000.0;
	if (u.find("us") != std::string::npos)
		return scale * 1000.0;
	return scale;
}

static LibertyUnits liberty_units(const LibertyAst *library)
{
	LibertyUnits units;
	units.time_scale = liberty_time_scale(library);
	const LibertyAst *cap = library->find("capacitive_load_unit");
	if (cap && cap->args.size() == 2) {
		units.cap_scale = atof(cap->args[0].c_str());
		std::string u = unquote(cap->args[1]);
		if (u == "pf" || u == "pF" || u == "PF")
			units.cap_scale *= 1000.0;
	}
	for (auto child : library->children) {
		if (child->id != "lu_table_template" || child->args.empty())
			continue;
		int axis = 0;
		for (int i = 1; i <= 3; i++) {
			const LibertyAst *var = child->find(stringf("variable_%d", i));
			if (var && unquote(var->value).find("capacitance") != std::string::npos)
				axis = i;
		}
		units.load_axis[child->args[0]] = axis;
	}
	return units;
}

static std::vector<double> liberty_numbers(const std::string &s)
{
	std::vector<double> v;
	for (auto &tok : split_tokens(unquote(s), ", \t"))
		v.push_back(atof(tok.c_str()));
	return v;
}

static double median(std::vector<double> v)
{
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

// delay over load at the median input slew; a table without a load axis yields one sample
static void liberty_table_samples(const LibertyAst *table, const LibertyUnits &units, std::vector<std::pair<double, double>> &samples)
{
	const LibertyAst *values = table->find("values");
	if (!values)
		return;
	std::vector<std::vector<double>> rows;
	std::vector<double> flat;
	for (auto &arg : values->args) {
		rows.push_back(liberty_numbers(arg));
		flat.insert(flat.end(), rows.back().begin(), rows.back().end());
	}
	if (flat.empty())
		return;

	const LibertyAst *i1 = table->find("index_1"), *i2 = table->find("index_2");
	int axis = 0;
	if (!table->args.empty() && units.load_axis.count(table->args[0]))
		axis = units.load_axis.at(table->args[0]);
	else if (i2)
		axis = 2;
	else if (i1)
		axis = 1;
	std::vector<double> loads;
	if (axis == 1 && i1 && !i1->args.empty())
		loads = liberty_numbers(i1->args[0]);
	if (axis == 2 && i2 && !i2->args.empty())
		loads = liberty_numbers(i2->args[0]);
	if (loads.empty()) {
		samples.push_back({0, median(flat)});
		return;
	}

	if (i1 && i2 && rows.size() > 1 && rows[0].size() > 1) {
		if (axis == 2) {
			auto &row = rows[rows.size() / 2];
			for (size_t j = 0; j < std::min(loads.size(), row.size()); j++)
				samples.push_back({loads[j], row[j]});
		} else {
			size_t col = rows[0].size() / 2;
			for (size_t i = 0; i < std::min(loads.size(), rows.size()); i++)
				if (col < rows[i].size())
					samples.push_back({loads[i], rows[i][col]});
		}
		return;
	}
	for (size_t i = 0; i < std::min(loads.size(), flat.size()); i++)
		samples.push_back({loads[i], flat[i]});
}

static LibertyArc liberty_fit(const std::vector<std::pair<double, double>> &samples, const LibertyUnits &units)
{
	LibertyArc arc;
	if (samples.empty())
		return arc;
	arc.valid = true;
	double n = samples.size(), sx = 0, sy = 0, sxx = 0, sxy = 0;
	for (auto &[x, y] : samples) {
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
	}
	double var = sxx - sx * sx / n;
	arc.k = var > 0 ? (sxy - sx * sy / n) / var : 0;
	if (arc.k < 0)
		arc.k = 0;
	arc.d0 = std::max(0.0, (sy - arc.k * sx) / n);
	arc.d0 *= units.time_scale;
	arc.k *= units.time_scale / units.cap_scale;
	return arc;
}

static bool liberty_timing_matches(const LibertyAst *timing, const std::string &related_pin, const std::vector<std::string> &types)
{
	const LibertyAst *related = timing->find("related_pin");
	if (!related)
		return false;
	auto pins = split_tokens(unquote(related->value));
	if (std::find(pins.begin(), pins.end(), related_pin) == pins.end())
		return false;
	const LibertyAst *type = timing->find("timing_type");
	std::string t = type ? unquote(type->value) : "";
	return std::find(types.begin(), types.end(), t) != types.end();
}

static LibertyArc liberty_pin_arc(const LibertyAst *out_pin, const std::string &in_pin, const std::vector<std::string> &types, const LibertyUnits &units)
{
	LibertyArc arc;
	for (auto timing : out_pin->children) {
		if (timing->id != "timing" || !liberty_timing_matches(timing, in_pin, types))
			continue;
		for (auto table : timing->children) {
			if (table->id != "cell_rise" && table->id != "cell_fall")
				continue;
			std::vector<std::pair<double, double>> samples;
			liberty_table_samples(table, units, samples);
			LibertyArc fit = liberty_fit(samples, units);
			if (!fit.valid)
				continue;
			arc.valid = true;
			arc.d0 = std::max(arc.d0, fit.d0);
			arc.k = std::max(arc.k, fit.k);
		}
	}
	return arc;
}

static bool liberty_setup(const LibertyAst *in_pin, const LibertyUnits &units, double &setup)
{
	std::vector<double> values;
	for (auto timing : in_pin->children) {
		if (timing->id != "timing")
			continue;
		const LibertyAst *type = timing->find("timing_type");
		if (!type || unquote(type->value).compare(0, 5, "setup") != 0)
			continue;
		for (auto table : timing->children) {
			if (table->id != "rise_constraint" && table->id != "fall_constraint")
				continue;
			const LibertyAst *table_values = table->find("values");
			if (!table_values)
				continue;
			for (auto &arg : table_values->args)
				for (double v : liberty_numbers(arg))
					values.push_back(v);
		}
	}
	if (values.empty())
		return false;
	setup = median(values) * units.time_scale;
	return true;
}

static double liberty_pin_load(const LibertyAst *pin, const char *attr, const LibertyUnits &units)
{
	const LibertyAst *c = pin->find(attr);
	return c ? atof(c->value.c_str()) * units.cap_scale : 0;
}

struct LibertyCellInfo {
	std::vector<const LibertyAst *> inputs, outputs;
	bool sequential = false;
};

static bool liberty_cell_pins(const LibertyAst *cell, LibertyCellInfo &info)
{
	for (auto child : cell->children) {
		if (child->id == "ff" || child->id == "latch" || child->id == "ff_bank" || child->id == "latch_bank")
			info.sequential = true;
		if (child->id == "bus" || child->id == "bundle")
			return false;
		if (child->id != "pin" || child->args.size() != 1)
			continue;
		if (child->find("three_state"))
			return false;
		const LibertyAst *dir = child->find("direction");
		std::string d = dir ? unquote(dir->value) : "";
		if (d == "input")
			info.inputs.push_back(child);
		else if (d == "output")
			info.outputs.push_back(child);
		else
			return false;
	}
	return true;
}

static void liberty_cell_timing(const LibertyAst *cell, const LibertyUnits &units, double fanout_load, CellTiming &timing)
{
	LibertyCellInfo info;
	if (!liberty_cell_pins(cell, info))
		return;
	IdString type(RTLIL::escape_id(cell->args[0]));
	for (auto in : info.inputs) {
		double setup;
		if (liberty_setup(in, units, setup))
			timing.setup[{type, IdString(RTLIL::escape_id(in->args[0]))}] = setup;
	}
	for (auto out : info.outputs) {
		LibertyArc arc;
		for (auto clk : info.inputs) {
			LibertyArc a = liberty_pin_arc(out, clk->args[0], {"rising_edge", "falling_edge"}, units);
			if (!a.valid)
				continue;
			arc.valid = true;
			arc.d0 = std::max(arc.d0, a.d0);
			arc.k = std::max(arc.k, a.k);
		}
		if (arc.valid)
			timing.arrival[{type, IdString(RTLIL::escape_id(out->args[0]))}] = arc.d0 + arc.k * fanout_load;
	}
}

static double liberty_average_input_load(const std::vector<const LibertyAst *> &cells, const LibertyUnits &units)
{
	double sum = 0;
	int count = 0;
	for (auto cell : cells) {
		LibertyCellInfo info;
		if (!liberty_cell_pins(cell, info) || info.sequential)
			continue;
		for (auto in : info.inputs) {
			sum += liberty_pin_load(in, "capacitance", units);
			count++;
		}
	}
	return count ? sum / count : 0;
}

// combinational cells become one genlib gate per output pin; the reader groups them into a multi-output cell
static bool liberty_cell_to_genlib(const LibertyAst *cell, const LibertyUnits &units, std::string &out)
{
	LibertyCellInfo info;
	if (!liberty_cell_pins(cell, info) || info.sequential || info.outputs.empty())
		return false;

	std::vector<std::string> exprs;
	std::unordered_set<std::string> names;
	for (auto out_pin : info.outputs) {
		const LibertyAst *f = out_pin->find("function");
		if (!f)
			return false;
		std::string func = unquote(f->value);
		std::string trimmed = func;
		trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(), isspace), trimmed.end());
		if (trimmed == "0" || trimmed == "1") {
			exprs.push_back("CONST" + trimmed);
			continue;
		}
		LibertyExpression::Lexer lexer(func);
		LibertyExpression e = LibertyExpression::parse(lexer);
		if (e.kind == LibertyExpression::Kind::EMPTY)
			return false;
		e.get_pin_names(names);
		exprs.push_back(genlib_expr(e));
	}
	int used = 0;
	for (auto in : info.inputs)
		if (names.count(in->args[0]))
			used++;
	if (used != GetSize(names))
		return false;

	double area = 1;
	if (const LibertyAst *ar = cell->find("area"))
		area = atof(ar->value.c_str());

	for (int i = 0; i < GetSize(info.outputs); i++) {
		const LibertyAst *out_pin = info.outputs[i];
		out += stringf("GATE %s %g %s=%s;", cell->args[0], area, out_pin->args[0], exprs[i]);
		double max_load = liberty_pin_load(out_pin, "max_capacitance", units);
		for (auto in : info.inputs) {
			if (!names.count(in->args[0]))
				continue;
			LibertyArc arc = liberty_pin_arc(out_pin, in->args[0], {"", "combinational", "combinational_rise", "combinational_fall"}, units);
			if (!arc.valid)
				arc.d0 = 1.0;
			out += stringf(" PIN %s UNKNOWN %g %g %g %g %g %g", in->args[0], liberty_pin_load(in, "capacitance", units), max_load, arc.d0, arc.k, arc.d0, arc.k);
		}
		out += "\n";
	}
	return true;
}

static bool liberty_dont_use(const LibertyAst *cell, const std::vector<std::string> &dont_use)
{
	const LibertyAst *dn = cell->find("dont_use");
	if (dn && unquote(dn->value) == "true")
		return true;
	for (auto &pat : dont_use)
		if (patmatch(pat.c_str(), cell->args[0].c_str()))
			return true;
	return false;
}

static void read_genlib(std::istream &f, const std::string &name, std::vector<mockturtle::gate> &gates)
{
	if (lorina::read_genlib(f, mockturtle::genlib_reader(gates)) != lorina::return_code::success)
		log_error("Failed to parse genlib library `%s'.\n", name);
}

struct MockturtlePass : public Pass {
	MockturtlePass() : Pass("mockturtle", "logic optimization and technology mapping using mockturtle") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    mockturtle [options] [selection]\n");
		log("\n");
		log("This pass uses the mockturtle library [1] in-process to optimize the gate-level\n");
		log("logic of the selected modules and map it to LUTs or to a standard cell library.\n");
		log("Logic is extracted from the simple gate cells ($_AND_, $_NOT_, $_MUX_, ...) and\n");
		log("$buf cells; all other cells, and gate cells with a (* keep *) attribute, stay\n");
		log("in place and delimit the mapped logic. Wires driving them, module outputs and\n");
		log("(* keep *) wires keep their names.\n");
		log("\n");
		log("    -lut <width>\n");
		log("        generate netlist using luts of (max) the specified width.\n");
		log("\n");
		log("    -lut <w1>:<w2>\n");
		log("        generate netlist using luts of (max) the specified width <w2>. All\n");
		log("        luts with width <= <w1> have constant cost. for luts larger than <w1>\n");
		log("        the area cost doubles with each additional input bit. the delay cost\n");
		log("        is still constant for all lut widths.\n");
		log("\n");
		log("    -lut <file>\n");
		log("        read a LUT library from the file (as written by 'abc9_ops -write_lut'):\n");
		log("        one line per LUT size with the format '<width> <area> <delay>...'.\n");
		log("\n");
		log("    -luts <cost1>,<cost2>,<cost3>,<sizeN>:<cost4-N>,..\n");
		log("        generate netlist using luts. Use the specified costs for luts with 1,\n");
		log("        2, 3, .. inputs.\n");
		log("\n");
		log("    -genlib <file>\n");
		log("        map to the standard cells described in the genlib file. this option\n");
		log("        can be used multiple times.\n");
		log("\n");
		log("    -liberty <file>\n");
		log("        map to the combinational cells described in the Liberty file. this\n");
		log("        option can be used multiple times. pin delays are fitted as a linear\n");
		log("        function of the output load from the NLDM tables at the median input\n");
		log("        slew. cells with several output functions become multi-output cells\n");
		log("        (see -multioutput).\n");
		log("\n");
		log("    -dont_use <cell_name>\n");
		log("        avoid usage of the technology cell <cell_name> when mapping the design.\n");
		log("        this option can be used multiple times. only supported with Liberty\n");
		log("        cell libraries.\n");
		log("\n");
		log("    -D <delay>\n");
		log("        set delay target, in the delay units of the LUT library or in\n");
		log("        picoseconds for cell libraries. for cell libraries this is the clock\n");
		log("        period: with a Liberty library the required time at the inputs of\n");
		log("        the cells that stay in place is reduced by their setup time, and their\n");
		log("        outputs arrive after their clock-to-output delay (at a fanout of two\n");
		log("        average pins).\n");
		log("\n");
		log("    -io_delay <delay>\n");
		log("        delay in picoseconds outside the module at its input and output\n");
		log("        ports, subtracted from the -D budget on paths through them.\n");
		log("\n");
		log("    -multioutput\n");
		log("        also map to multi-output cells such as full adders (slower).\n");
		log("\n");
		log("    -noload\n");
		log("        ignore the fanout delay of the cells. by default the delay of a cell\n");
		log("        grows with the estimated load of its output: its fanout count times\n");
		log("        the average input pin load of the library, plus -wire_load.\n");
		log("\n");
		log("    -wire_load <femtofarads>\n");
		log("        load added to every net in the load model (default: 0).\n");
		log("\n");
		log("    -fanout_load <femtofarads>\n");
		log("        load of one fanout pin in the load model (default: the average input\n");
		log("        pin load of the library).\n");
		log("\n");
		log("    -fanout_limit <n>\n");
		log("        count at most <n> fanouts in the load model, assuming larger nets\n");
		log("        get buffered later (default: 8, 0 for no limit).\n");
		log("\n");
		log("    -write_genlib <file>\n");
		log("        write the gates converted from the Liberty files to a genlib file.\n");
		log("\n");
		log("    -area\n");
		log("        perform area-oriented instead of delay-oriented mapping, ignoring any\n");
		log("        delay target. the default can also be set with the 'mockturtle.area'\n");
		log("        scratchpad variable.\n");
		log("\n");
		log("    -opt <steps>\n");
		log("        comma-separated logic optimization steps to run before mapping. the\n");
		log("        available steps are 'balance', 'rewrite', 'resub' and 'none'. the\n");
		log("        default is 'balance,rewrite,resub,balance' and can also be set with\n");
		log("        the 'mockturtle.opt' scratchpad variable.\n");
		log("\n");
		log("    -choices [<n>]\n");
		log("        map with structural choices (cell libraries only): derive <n> (1 to 4,\n");
		log("        default 4) differently optimized variants of the logic, prove their\n");
		log("        nodes equivalent by SAT sweeping and let the mapper pick the best\n");
		log("        structure for each cell, like ABC's 'dch'. the variants are the\n");
		log("        optimized logic, two rounds of further rewriting on it and, with\n");
		log("        <n> = 4, a balanced version of the unoptimized logic. not supported\n");
		log("        with -multioutput.\n");
		log("\n");
		log("    -keep_names\n");
		log("        also preserve public wires driven by mapped logic, at the cost of\n");
		log("        forcing a mapped cell output on each of them.\n");
		log("\n");
		log("[1] https://github.com/lsils/mockturtle\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing MOCKTURTLE pass (logic optimization and technology mapping).\n");
		log_push();

		std::string lut_arg, luts_arg, lut_file;
		std::vector<std::string> genlib_files, liberty_files, dont_use;
		std::string write_genlib;
		double delay_target = 0;
		CellMapOptions cell_opts;
		cell_opts.area_oriented = design->scratchpad_get_bool("mockturtle.area", false);
		bool keep_names = false;
		int choices = 0;
		mockturtle::choices_params cps;
		cps.conflict_limit = design->scratchpad_get_int("mockturtle.choices.conflict_limit", cps.conflict_limit);
		cps.max_clauses = design->scratchpad_get_int("mockturtle.choices.max_clauses", cps.max_clauses);
		cps.num_patterns = design->scratchpad_get_int("mockturtle.choices.patterns", cps.num_patterns);
		cps.max_patterns = design->scratchpad_get_int("mockturtle.choices.max_patterns", cps.max_patterns);
		cell_opts.choices_keep_delay = design->scratchpad_get_bool("mockturtle.choices.keep_delay", true);
		cell_opts.choice_cut_limit = design->scratchpad_get_int("mockturtle.choices.cut_limit", 16);
		std::string opt = design->scratchpad_get_string("mockturtle.opt", "balance,rewrite,resub,balance");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			std::string arg = args[argidx];
			if (arg == "-lut" && argidx+1 < args.size()) {
				arg = args[++argidx];
				if (arg.find_first_not_of("0123456789:") == std::string::npos)
					lut_arg = arg;
				else {
					lut_file = arg;
					rewrite_filename(lut_file);
				}
				continue;
			}
			if (arg == "-luts" && argidx+1 < args.size()) {
				luts_arg = args[++argidx];
				continue;
			}
			if (arg == "-genlib" && argidx+1 < args.size()) {
				rewrite_filename(args[argidx+1]);
				genlib_files.push_back(args[++argidx]);
				continue;
			}
			if (arg == "-liberty" && argidx+1 < args.size()) {
				rewrite_filename(args[argidx+1]);
				liberty_files.push_back(args[++argidx]);
				continue;
			}
			if (arg == "-dont_use" && argidx+1 < args.size()) {
				dont_use.push_back(args[++argidx]);
				continue;
			}
			if (arg == "-D" && argidx+1 < args.size()) {
				delay_target = atof(args[++argidx].c_str());
				continue;
			}
			if (arg == "-area") {
				cell_opts.area_oriented = true;
				continue;
			}
			if (arg == "-io_delay" && argidx+1 < args.size()) {
				cell_opts.io_delay = atof(args[++argidx].c_str()) / 1000.0;
				continue;
			}
			if (arg == "-multioutput") {
				cell_opts.multioutput = true;
				continue;
			}
			if (arg == "-noload") {
				cell_opts.load_aware = false;
				continue;
			}
			if (arg == "-wire_load" && argidx+1 < args.size()) {
				cell_opts.wire_load = atof(args[++argidx].c_str());
				continue;
			}
			if (arg == "-fanout_load" && argidx+1 < args.size()) {
				cell_opts.fanout_load = atof(args[++argidx].c_str());
				continue;
			}
			if (arg == "-fanout_limit" && argidx+1 < args.size()) {
				cell_opts.fanout_limit = atoi(args[++argidx].c_str());
				continue;
			}
			if (arg == "-write_genlib" && argidx+1 < args.size()) {
				write_genlib = args[++argidx];
				rewrite_filename(write_genlib);
				continue;
			}
			if (arg == "-opt" && argidx+1 < args.size()) {
				opt = args[++argidx];
				continue;
			}
			if (arg == "-keep_names") {
				keep_names = true;
				continue;
			}
			if (arg == "-choices") {
				choices = 4;
				if (argidx+1 < args.size() && !args[argidx+1].empty() && args[argidx+1].find_first_not_of("0123456789") == std::string::npos)
					choices = std::clamp(atoi(args[++argidx].c_str()), 1, 4);
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		bool lut_mode = !lut_arg.empty() || !luts_arg.empty() || !lut_file.empty();
		bool cell_mode = !genlib_files.empty() || !liberty_files.empty();
		if (lut_mode == cell_mode)
			log_cmd_error("Exactly one of -lut/-luts or -genlib/-liberty must be given.\n");
		if (!dont_use.empty() && liberty_files.empty())
			log_cmd_error("-dont_use is only supported with -liberty.\n");
		if (choices && lut_mode)
			log_cmd_error("-choices is only supported with cell libraries.\n");
		if (choices && cell_opts.multioutput)
			log_cmd_error("-choices is not supported together with -multioutput.\n");

		std::vector<std::string> steps;
		for (auto &step : split_tokens(opt, ",")) {
			if (step == "none")
				continue;
			if (step != "balance" && step != "rewrite" && step != "resub")
				log_cmd_error("Unknown optimization step `%s'.\n", step);
			steps.push_back(step);
		}

		std::vector<mockturtle::gate> gates;
		std::unique_ptr<TechLibrary> lib;
		CellTiming timing;
		if (lut_mode) {
			if (!lut_file.empty())
				LutCost::table = lut_costs_from_file(lut_file);
			else
				LutCost::table = lut_costs_from_widths(lut_arg, luts_arg);
			log("Using LUTs of up to %d inputs.\n", GetSize(LutCost::table));
		} else {
			for (auto &path : genlib_files) {
				std::ifstream f(path);
				if (!f.is_open())
					log_error("Can't open genlib file `%s'.\n", path);
				read_genlib(f, path, gates);
			}
			if (!liberty_files.empty()) {
				std::string genlib;
				std::vector<std::string> converted, ignored;
				for (auto &path : liberty_files) {
					std::istream *f = uncompressed(path);
					LibertyParser p(*f, path);
					LibertyMergedCells merged;
					merged.merge(p);
					delete f;
					LibertyUnits units = liberty_units(p.ast);
					double fanout_load = 2 * liberty_average_input_load(merged.cells, units);
					for (auto cell : merged.cells) {
						liberty_cell_timing(cell, units, fanout_load, timing);
						if (liberty_dont_use(cell, dont_use))
							continue;
						if (liberty_cell_to_genlib(cell, units, genlib))
							converted.push_back(cell->args[0]);
						else
							ignored.push_back(cell->args[0]);
					}
				}
				log("Converted %d Liberty cells to genlib gates.\n", GetSize(converted));
				std::string ignored_names;
				for (auto &name : ignored)
					ignored_names += " " + name;
				log("Ignored %d non-combinational cells:%s\n", GetSize(ignored), ignored_names);
				if (!write_genlib.empty()) {
					std::ofstream out(write_genlib);
					if (!out.is_open())
						log_error("Can't open genlib file `%s' for writing.\n", write_genlib);
					out << genlib;
				}
				std::istringstream f(genlib);
				read_genlib(f, "<liberty>", gates);
				pool<std::string> loaded;
				for (auto &g : gates)
					loaded.insert(g.name);
				for (auto &name : converted)
					if (!loaded.count(name))
						log_warning("Cell %s was rejected by the genlib reader.\n", name);
			}
			if (gates.empty())
				log_error("No usable gates in the cell library.\n");
			log("Loaded %d gates.\n", GetSize(gates));
			add_const_gates(gates);
			mockturtle::tech_library_params lib_ps;
			lib_ps.load_multioutput_gates = cell_opts.multioutput;
			lib_ps.load_multioutput_gates_single = cell_opts.multioutput;
			CerrCapture capture;
			lib = std::make_unique<TechLibrary>(gates, lib_ps);
			if (cell_opts.multioutput)
				log("Loaded %u multi-output cells into the library.\n", lib->num_multioutput_gates());
		}

		NpnResynthesis resyn;
		RewriteLibrary rewrite_lib(resyn);

		for (auto module : design->selected_modules()) {
			if (module->processes.size() > 0) {
				log("Skipping module %s as it contains processes.\n", log_id(module));
				continue;
			}
			if (!design->selected_whole_module(module))
				log_error("Can't handle partially selected module %s!\n", log_id(module));

			log_header(design, "Mapping module %s.\n", log_id(module));
			Worker worker(module, keep_names);
			CellMapOptions opts = cell_opts;
			opts.period = lut_mode ? 0 : delay_target / 1000.0;
			opts.timing = liberty_files.empty() ? nullptr : &timing;
			worker.build(opts);
			if (worker.aig.num_pos() > 0) {
				worker.optimize(steps, rewrite_lib, choices >= 4);
				if (lut_mode)
					worker.map_luts(GetSize(LutCost::table), cell_opts.area_oriented, int(delay_target));
				else if (choices > 0)
					worker.map_cells_with_choices(choices, rewrite_lib, *lib, opts, cps);
				else
					worker.map_cells(worker.aig, *lib, opts);
			}
			worker.finish();
		}

		log_pop();
	}
} MockturtlePass;

PRIVATE_NAMESPACE_END
