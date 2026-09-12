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
#include <mockturtle/views/binding_view.hpp>
#include <mockturtle/algorithms/aig_balancing.hpp>
#include <mockturtle/algorithms/rewrite.hpp>
#include <mockturtle/algorithms/node_resynthesis/xag_npn.hpp>
#include <mockturtle/algorithms/resubstitution.hpp>
#include <mockturtle/algorithms/aig_resub.hpp>
#include <mockturtle/algorithms/cleanup.hpp>
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
using Klut = mockturtle::klut_network;
using BoundKlut = mockturtle::binding_view<Klut>;
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

struct KlutNodeCollector {
	std::vector<Klut::node> &out;
	void operator()(Klut::node n) const { out.push_back(n); }
};

struct KlutFaninCollector {
	std::vector<Klut::signal> &out;
	void operator()(const Klut::signal &f) const { out.push_back(f); }
};

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

	Aig aig;
	pool<Cell *> logic_cells;
	dict<SigBit, std::pair<Cell *, int>> logic_driver;
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
			if (!is_gate_type(cell->type) || cell->has_keep_attr())
				continue;
			logic_cells.insert(cell);
			SigSpec y = sigmap(cell->getPort(ID::Y));
			for (int i = 0; i < GetSize(y); i++)
				if (y[i].wire)
					logic_driver[y[i]] = {cell, i};
		}
	}

	void add_co(SigBit bit)
	{
		bit = sigmap(bit);
		if (!bit.wire || is_ci(bit))
			return;
		if (co_set.insert(bit).second)
			co_bits.push_back(bit);
	}

	bool is_buf_driven(SigBit bit)
	{
		auto it = logic_driver.find(sigmap(bit));
		if (it == logic_driver.end())
			return false;
		return it->second.first->type.in(ID($buf), ID($_BUF_));
	}

	void collect_cos()
	{
		for (auto &port : module->ports) {
			Wire *w = module->wire(port);
			if (w->port_output)
				for (auto bit : SigSpec(w))
					add_co(bit);
		}

		std::vector<Cell *> cells = module->cells().to_vector();
		std::sort(cells.begin(), cells.end(), IdString::compare_ptr_by_name<Cell>());
		for (auto cell : cells) {
			if (logic_cells.count(cell))
				continue;
			for (auto &conn : cell->connections())
				if (cell->port_dir(conn.first) != RTLIL::PD_OUTPUT)
					for (auto bit : conn.second)
						add_co(bit);
		}

		std::vector<Wire *> wires = module->wires().to_vector();
		std::sort(wires.begin(), wires.end(), IdString::compare_ptr_by_name<Wire>());
		for (auto w : wires) {
			bool keep = w->get_bool_attribute(ID::keep) || (keep_names && w->name.isPublic());
			for (auto bit : SigSpec(w))
				if (keep || (w->name.isPublic() && is_buf_driven(bit)))
					add_co(bit);
		}
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

	void build()
	{
		index_logic();
		collect_cos();
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

	void optimize(const std::vector<std::string> &steps, RewriteLibrary &rewrite_lib)
	{
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

	SigBit make_gate(const mockturtle::gate &g, const SigSpec &inputs)
	{
		if (GetSize(inputs) != GetSize(g.pins))
			log_error("Gate '%s' has %d pins but the mapped node has %d fanins.\n", g.name, GetSize(g.pins), GetSize(inputs));
		Cell *cell = module->addCell(new_name("g"), IdString(RTLIL::escape_id(g.name)));
		for (int i = 0; i < GetSize(inputs); i++)
			cell->setPort(IdString(RTLIL::escape_id(g.pins[i].name)), inputs[i]);
		Wire *y = module->addWire(new_name("n"));
		cell->setPort(IdString(RTLIL::escape_id(g.output_name)), y);
		cell_stats[cell->type]++;
		return y;
	}

	void map_cells(const TechLibrary &lib, bool area_oriented, double required_time)
	{
		mockturtle::emap_params ps;
		ps.area_oriented_mapping = area_oriented;
		ps.required_time = area_oriented ? std::numeric_limits<float>::max() : required_time;
		mockturtle::emap_stats st;
		BoundKlut res = mockturtle::emap_klut<6>(aig, lib, ps, &st);
		if (st.mapping_error)
			log_error("Technology mapping of module %s failed.\n", log_id(module));

		dict<Klut::node, SigBit> sig;
		Klut::node c0 = res.get_node(res.get_constant(false));
		Klut::node c1 = res.get_node(res.get_constant(true));
		sig[c0] = res.has_binding(c0) ? make_gate(res.get_binding(c0), SigSpec()) : SigBit(State::S0);
		sig[c1] = res.has_binding(c1) ? make_gate(res.get_binding(c1), SigSpec()) : SigBit(State::S1);
		for (uint32_t i = 0; i < res.num_pis(); i++)
			sig[res.pi_at(i)] = ci_bits[i];

		std::vector<Klut::node> order;
		mockturtle::topo_view<BoundKlut> topo{res};
		topo.foreach_node(KlutNodeCollector{order});
		for (auto n : order) {
			if (res.is_constant(n) || res.is_pi(n))
				continue;
			if (!res.has_binding(n))
				log_error("Mapped node %d in module %s has no gate binding.\n", int(n), log_id(module));
			std::vector<Klut::signal> fanins;
			res.foreach_fanin(n, KlutFaninCollector{fanins});
			SigSpec inputs;
			for (auto f : fanins)
				inputs.append(sig.at(res.get_node(f)));
			sig[n] = make_gate(res.get_binding(n), inputs);
		}

		std::vector<SigBit> po_sig;
		for (uint32_t i = 0; i < res.num_pos(); i++)
			po_sig.push_back(sig.at(res.get_node(res.po_at(i))));
		connect_cos(po_sig);

		log("MOCKTURTLE RESULTS:   mapped area: %8.2f   delay: %8.2f\n", res.compute_area(), res.compute_worst_delay());
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

static double liberty_pin_delay(const LibertyAst *out_pin, const std::string &in_pin)
{
	std::vector<double> values;
	for (auto timing : out_pin->children) {
		if (timing->id != "timing")
			continue;
		const LibertyAst *related = timing->find("related_pin");
		if (!related)
			continue;
		auto related_pins = split_tokens(unquote(related->value));
		if (std::find(related_pins.begin(), related_pins.end(), in_pin) == related_pins.end())
			continue;
		for (auto table : timing->children) {
			if (table->id != "cell_rise" && table->id != "cell_fall")
				continue;
			const LibertyAst *table_values = table->find("values");
			if (!table_values)
				continue;
			for (auto &arg : table_values->args)
				for (auto &tok : split_tokens(unquote(arg), ", \t"))
					values.push_back(atof(tok.c_str()));
		}
	}
	if (values.empty())
		return 1.0;
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}

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

static bool liberty_cell_to_genlib(const LibertyAst *cell, double time_scale, std::string &out)
{
	std::vector<std::string> inputs;
	const LibertyAst *out_pin = nullptr;
	std::string func;
	for (auto child : cell->children) {
		if (child->id == "ff" || child->id == "latch" || child->id == "ff_bank" || child->id == "latch_bank" ||
				child->id == "bus" || child->id == "bundle")
			return false;
		if (child->id != "pin" || child->args.size() != 1)
			continue;
		if (child->find("three_state"))
			return false;
		const LibertyAst *dir = child->find("direction");
		std::string d = dir ? unquote(dir->value) : "";
		if (d == "input") {
			inputs.push_back(child->args[0]);
		} else if (d == "output") {
			const LibertyAst *f = child->find("function");
			if (!f || out_pin)
				return false;
			out_pin = child;
			func = unquote(f->value);
		} else {
			return false;
		}
	}
	if (!out_pin)
		return false;

	double area = 1;
	if (const LibertyAst *ar = cell->find("area"))
		area = atof(ar->value.c_str());

	std::vector<std::string> used;
	std::string expr;
	std::string trimmed = func;
	trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(), isspace), trimmed.end());
	if (trimmed == "0" || trimmed == "1") {
		expr = "CONST" + trimmed;
	} else {
		LibertyExpression::Lexer lexer(func);
		LibertyExpression e = LibertyExpression::parse(lexer);
		if (e.kind == LibertyExpression::Kind::EMPTY)
			return false;
		std::unordered_set<std::string> names;
		e.get_pin_names(names);
		for (auto &in : inputs)
			if (names.count(in))
				used.push_back(in);
		if (GetSize(used) != GetSize(names))
			return false;
		expr = genlib_expr(e);
	}

	out += stringf("GATE %s %g %s=%s;", cell->args[0], area, out_pin->args[0], expr);
	for (auto &in : used) {
		double d = liberty_pin_delay(out_pin, in) * time_scale;
		out += stringf(" PIN %s UNKNOWN 1 999 %g 0 %g 0", in, d, d);
	}
	out += "\n";
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
		log("        map to the combinational single-output cells described in the Liberty\n");
		log("        file. this option can be used multiple times.\n");
		log("\n");
		log("    -dont_use <cell_name>\n");
		log("        avoid usage of the technology cell <cell_name> when mapping the design.\n");
		log("        this option can be used multiple times. only supported with Liberty\n");
		log("        cell libraries.\n");
		log("\n");
		log("    -D <delay>\n");
		log("        set delay target, in the delay units of the LUT library or in\n");
		log("        picoseconds for cell libraries. Liberty pin delays are taken as the\n");
		log("        median of the timing tables (the mapper has no load model).\n");
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
		double delay_target = 0;
		bool area = design->scratchpad_get_bool("mockturtle.area", false);
		bool keep_names = false;
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
				area = true;
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
			break;
		}
		extra_args(args, argidx, design);

		bool lut_mode = !lut_arg.empty() || !luts_arg.empty() || !lut_file.empty();
		bool cell_mode = !genlib_files.empty() || !liberty_files.empty();
		if (lut_mode == cell_mode)
			log_cmd_error("Exactly one of -lut/-luts or -genlib/-liberty must be given.\n");
		if (!dont_use.empty() && liberty_files.empty())
			log_cmd_error("-dont_use is only supported with -liberty.\n");

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
					double time_scale = liberty_time_scale(p.ast);
					for (auto cell : merged.cells) {
						if (liberty_dont_use(cell, dont_use))
							continue;
						if (liberty_cell_to_genlib(cell, time_scale, genlib))
							converted.push_back(cell->args[0]);
						else
							ignored.push_back(cell->args[0]);
					}
				}
				log("Converted %d Liberty cells to genlib gates.\n", GetSize(converted));
				std::string ignored_names;
				for (auto &name : ignored)
					ignored_names += " " + name;
				log("Ignored %d non-combinational or multi-output cells:%s\n", GetSize(ignored), ignored_names);
				std::istringstream f(genlib);
				read_genlib(f, "<liberty>", gates);
				if (GetSize(gates) != GetSize(converted)) {
					pool<std::string> loaded;
					for (auto &g : gates)
						loaded.insert(g.name);
					for (auto &name : converted)
						if (!loaded.count(name))
							log_warning("Cell %s was rejected by the genlib reader.\n", name);
				}
			}
			if (gates.empty())
				log_error("No usable gates in the cell library.\n");
			log("Loaded %d gates.\n", GetSize(gates));
			lib = std::make_unique<TechLibrary>(gates);
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
			worker.build();
			if (worker.aig.num_pos() > 0) {
				worker.optimize(steps, rewrite_lib);
				if (lut_mode)
					worker.map_luts(GetSize(LutCost::table), area, int(delay_target));
				else
					worker.map_cells(*lib, area, delay_target / 1000.0);
			}
			worker.finish();
		}

		log_pop();
	}
} MockturtlePass;

PRIVATE_NAMESPACE_END
