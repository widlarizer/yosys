/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
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
 *  ---
 *
 *  AST -> RTLIL conversion for procedural blocks (always/initial).
 *  Hosts LookaheadRewriter and ProcessGenerator. The single-expression
 *  conversion entry point AstNode::genRTLIL lives in genrtlil.cc.
 *
 */

#include "kernel/log.h"
#include "kernel/utils.h"
#include "libs/sha1/sha1.h"
#include "ast.h"
#include "ast_typed.h"
#include "genrtlil_internal.h"

#include <sstream>
#include <stdarg.h>
#include <algorithm>

YOSYS_NAMESPACE_BEGIN

using namespace AST;
using namespace AST_INTERNAL;

// helper class for rewriting simple lookahead references in AST always blocks
struct AST_INTERNAL::LookaheadRewriter
{
	dict<IdString, pair<AstNode*, AstNode*>> lookaheadids;

	void collect_lookaheadids(AstNode *node)
	{
		if (node->lookahead) {
			log_assert(AstIdentifier::matches(node));
			if (!lookaheadids.count(node->str)) {
				auto wire = std::make_unique<AstNode>(node->location, AST_WIRE);
				for (auto& c : node->id2ast->children)
					wire->children.push_back(c->clone());
				wire->fixup_hierarchy_flags();
				wire->str = stringf("$lookahead%s$%d", node->str, autoidx++);
				wire->set_attribute(ID::nosync, AstNode::mkconst_int(node->location, 1, false));
				wire->is_logic = true;
				while (wire->simplify()) { }
				lookaheadids[node->str] = make_pair(node->id2ast, wire.get());
				wire->genRTLIL();
				current_ast_mod->children.push_back(std::move(wire));
			}
		}

		for (auto& child : node->children)
			collect_lookaheadids(child.get());
	}

	bool has_lookaheadids(AstNode *node)
	{
		if (AstIdentifier::matches(node) && lookaheadids.count(node->str) != 0)
			return true;

		for (auto& child : node->children)
			if (has_lookaheadids(child.get()))
				return true;

		return false;
	}

	bool has_nonlookaheadids(AstNode *node)
	{
		if (AstIdentifier::matches(node) && lookaheadids.count(node->str) == 0)
			return true;

		for (auto& child : node->children)
			if (has_nonlookaheadids(child.get()))
				return true;

		return false;
	}

	void rewrite_lookaheadids(AstNode *node, bool lhs = false)
	{
		if (auto le = AstAssignLe::cast(node))
		{
			AstNode *lhs_node = le->lhs().get();
			AstNode *rhs_node = le->rhs().get();
			if (has_lookaheadids(lhs_node))
			{
				if (has_nonlookaheadids(lhs_node))
					log_error("incompatible mix of lookahead and non-lookahead IDs in LHS expression.\n");

				rewrite_lookaheadids(lhs_node, true);
				node->type = AST_ASSIGN_EQ;
			}

			rewrite_lookaheadids(rhs_node, lhs);
			return;
		}

		if (AstIdentifier::matches(node) && (node->lookahead || lhs)) {
			AstNode *newwire = lookaheadids.at(node->str).second;
			node->str = newwire->str;
			node->id2ast = newwire;
			lhs = false;
		}

		for (auto& child : node->children)
			rewrite_lookaheadids(child.get(), lhs);
	}

	static void run(AstBlock block)
	{
		LookaheadRewriter self;
		auto loc = block.loc();

		self.collect_lookaheadids(block.raw());
		self.rewrite_lookaheadids(block.raw());

		for (auto it : self.lookaheadids)
		{
			auto ref_orig = std::make_unique<AstNode>(loc, AST_IDENTIFIER);
			ref_orig->str = it.second.first->str;
			ref_orig->id2ast = it.second.first;
			ref_orig->was_checked = true;

			auto ref_temp = std::make_unique<AstNode>(loc, AST_IDENTIFIER);
			ref_temp->str = it.second.second->str;
			ref_temp->id2ast = it.second.second;
			ref_temp->was_checked = true;

			auto init_assign = std::make_unique<AstNode>(loc, AST_ASSIGN_EQ, ref_temp->clone(), ref_orig->clone());
			auto final_assign = std::make_unique<AstNode>(loc, AST_ASSIGN_LE, std::move(ref_orig), std::move(ref_temp));

			block.raw()->children.insert(block.raw()->children.begin(), std::move(init_assign));
			block.raw()->children.push_back(std::move(final_assign));
		}
	}
};

// helper class for converting AST always nodes to RTLIL processes
struct AST_INTERNAL::ProcessGenerator
{
	// input and output structures
	std::unique_ptr<AstNode> always;
	RTLIL::SigSpec initSyncSignals;
	RTLIL::Process *proc;
	RTLIL::SigSpec outputSignals;

	// This always points to the RTLIL::CaseRule being filled at the moment
	RTLIL::CaseRule *current_case;

	// This map contains the replacement pattern to be used in the right hand side
	// of an assignment. E.g. in the code "foo = bar; foo = func(foo);" the foo in the right
	// hand side of the 2nd assignment needs to be replace with the temporary signal holding
	// the value assigned in the first assignment. So when the first assignment is processed
	// the according information is appended to subst_rvalue_from and subst_rvalue_to.
	stackmap<RTLIL::SigBit, RTLIL::SigBit> subst_rvalue_map;

	// This map contains the replacement pattern to be used in the left hand side
	// of an assignment. E.g. in the code "always @(posedge clk) foo <= bar" the signal bar
	// should not be connected to the signal foo. Instead it must be connected to the temporary
	// signal that is used as input for the register that drives the signal foo.
	stackmap<RTLIL::SigBit, RTLIL::SigBit> subst_lvalue_map;

	// The code here generates a number of temporary signal for each output register. This
	// map helps generating nice numbered names for all this temporary signals.
	std::map<RTLIL::Wire*, int> new_temp_count;

	// Buffer for generating the init action
	RTLIL::SigSpec init_lvalue, init_rvalue;

	// The most recently assigned $print or $check cell \PRIORITY.
	int last_effect_priority;

	// Track which signals have been assigned in current_case to avoid unnecessary removeSignalFromCaseTree calls
	pool<RTLIL::SigBit> current_case_assigned_bits;

	ProcessGenerator(std::unique_ptr<AstNode> a, RTLIL::SigSpec initSyncSignalsArg = RTLIL::SigSpec()) : always(std::move(a)), initSyncSignals(initSyncSignalsArg), last_effect_priority(0)
	{
		// rewrite lookahead references
		LookaheadRewriter::run(AstProcBase(always.get()).body());

		// generate process and simple root case
		proc = current_module->addProcess(stringf("$proc$%s:%d$%d", RTLIL::encode_filename(*always->location.begin.filename), always->location.begin.line, autoidx++));
		set_src_attr(proc, always.get());
		copy_const_attributes(proc, always.get());
		current_case = &proc->root_case;

		// create initial temporary signal for all output registers
		RTLIL::SigSpec subst_lvalue_from, subst_lvalue_to;
		collect_lvalues(subst_lvalue_from, always.get(), true, true);
		subst_lvalue_to = new_temp_signal(subst_lvalue_from);
		subst_lvalue_map = subst_lvalue_from.to_sigbit_map(subst_lvalue_to);

		bool found_global_syncs = false;
		bool found_anyedge_syncs = false;
		for (auto& child : always->children)
		{
			if (ClockedEdgeLike::accepts(child.get())) {
				if (auto id = sensitivity_operand_identifier(child.get());
						id && id->raw()->id2ast && AstWire::matches(id->raw()->id2ast) &&
						id->raw()->id2ast->get_bool_attribute(ID::gclk))
					found_global_syncs = true;
			}
			if (AstEdge::matches(child.get())) {
				auto id = sensitivity_operand_identifier(child.get());
				if (id && id->str() == "\\$global_clock")
					found_global_syncs = true;
				else
					found_anyedge_syncs = true;
			}
		}

		if (found_anyedge_syncs) {
			if (found_global_syncs)
				always->input_error("Found non-synthesizable event list!\n");
			log("Note: Assuming pure combinatorial block at %s in\n", always->loc_string());
			log("compliance with IEC 62142(E):2005 / IEEE Std. 1364.1(E):2002. Recommending\n");
			log("use of @* instead of @(...) for better match of synthesis and simulation.\n");
		}

		// create syncs for the process
		bool found_clocked_sync = false;
		for (auto& child : always->children)
			if (ClockedEdgeLike::accepts(child.get())) {
				if (auto id = sensitivity_operand_identifier(child.get());
						id && id->raw()->id2ast && AstWire::matches(id->raw()->id2ast) &&
						id->raw()->id2ast->get_bool_attribute(ID::gclk))
					continue;
				found_clocked_sync = true;
				if (found_global_syncs || found_anyedge_syncs)
					always->input_error("Found non-synthesizable event list!\n");
				RTLIL::SyncRule *syncrule = new RTLIL::SyncRule;
				syncrule->type = AstPosedge::matches(child.get()) ? RTLIL::STp : RTLIL::STn;
				syncrule->signal = sensitivity_operand(child.get())->genRTLIL();
				if (GetSize(syncrule->signal) != 1)
					always->input_error("Found posedge/negedge event on a signal that is not 1 bit wide!\n");
				addChunkActions(syncrule->actions, subst_lvalue_from, subst_lvalue_to, true);
				// Automatic (nosync) variables must not become flip-flops: remove
				// them from clocked sync rules so that proc_dff does not infer
				// an unnecessary register for a purely combinational temporary.
				syncrule->actions.erase(
					std::remove_if(syncrule->actions.begin(), syncrule->actions.end(),
						[](const RTLIL::SigSig &ss) {
							for (auto &chunk : ss.first.chunks())
								if (chunk.wire && chunk.wire->get_bool_attribute(ID::nosync))
									return true;
							return false;
						}),
					syncrule->actions.end());
				proc->syncs.push_back(syncrule);
			}
		if (proc->syncs.empty()) {
			RTLIL::SyncRule *syncrule = new RTLIL::SyncRule;
			syncrule->type = found_global_syncs ? RTLIL::STg : RTLIL::STa;
			syncrule->signal = RTLIL::SigSpec();
			addChunkActions(syncrule->actions, subst_lvalue_from, subst_lvalue_to, true);
			proc->syncs.push_back(syncrule);
		}

		// create initial assignments for the temporary signals
		if ((flag_nolatches || always->get_bool_attribute(ID::nolatches) || current_module->get_bool_attribute(ID::nolatches)) && !found_clocked_sync) {
			subst_rvalue_map = subst_lvalue_from.to_sigbit_dict(RTLIL::SigSpec(RTLIL::State::Sx, GetSize(subst_lvalue_from)));
		} else {
			addChunkActions(current_case->actions, subst_lvalue_to, subst_lvalue_from);
			// Track initial assignments
			for (auto &bit : subst_lvalue_to)
				if (bit.wire != NULL)
					current_case_assigned_bits.insert(bit);
		}

		// process the AST
		for (auto& child : always->children)
			if (AstBlock::matches(child.get()))
				processAst(child.get());

		for (auto sync: proc->syncs)
			processMemWrites(sync);

		if (initSyncSignals.size() > 0)
		{
			RTLIL::SyncRule *sync = new RTLIL::SyncRule;
			sync->type = RTLIL::SyncType::STi;
			proc->syncs.push_back(sync);

			log_assert(init_lvalue.size() == init_rvalue.size());

			int offset = 0;
			for (auto &init_lvalue_c : init_lvalue.chunks()) {
				RTLIL::SigSpec lhs = init_lvalue_c;
				RTLIL::SigSpec rhs = init_rvalue.extract(offset, init_lvalue_c.width);
				remove_unwanted_lvalue_bits(lhs, rhs);
				sync->actions.push_back(RTLIL::SigSig(lhs, rhs));
				offset += lhs.size();
			}
		}

		outputSignals = RTLIL::SigSpec(subst_lvalue_from);
	}

	void remove_unwanted_lvalue_bits(RTLIL::SigSpec &lhs, RTLIL::SigSpec &rhs)
	{
		RTLIL::SigSpec new_lhs, new_rhs;

		log_assert(GetSize(lhs) == GetSize(rhs));
		for (int i = 0; i < GetSize(lhs); i++) {
			if (lhs[i].wire == nullptr)
				continue;
			new_lhs.append(lhs[i]);
			new_rhs.append(rhs[i]);
		}

		lhs = new_lhs;
		rhs = new_rhs;
	}

	// create new temporary signals
	RTLIL::SigSpec new_temp_signal(RTLIL::SigSpec sig)
	{
		std::vector<RTLIL::SigChunk> chunks = sig.chunks();

		for (int i = 0; i < GetSize(chunks); i++)
		{
			RTLIL::SigChunk &chunk = chunks[i];
			if (chunk.wire == nullptr)
				continue;

			std::string wire_name;
			do {
				wire_name = stringf("$%d%s[%d:%d]", new_temp_count[chunk.wire]++,
						chunk.wire->name.c_str(), chunk.width+chunk.offset-1, chunk.offset);;
				if (chunk.wire->name.str().find('$') != std::string::npos)
					wire_name += stringf("$%d", autoidx++);
			} while (current_module->wires_.count(wire_name) > 0);

			RTLIL::Wire *wire = current_module->addWire(wire_name, chunk.width);
			set_src_attr(wire, always.get());

			chunk.wire = wire;
			chunk.offset = 0;
		}

		return chunks;
	}

	// recursively traverse the AST and collect all assigned signals
	void collect_lvalues(RTLIL::SigSpec &reg, AstNode *ast, bool type_eq, bool type_le, bool run_sort_and_unify = true)
	{
		if (auto cs = AstCase::cast(ast)) {
			for (auto it = cs->conditions_begin(); it != cs->conditions_end(); ++it) {
				log_assert(AstAnyCond::matches(it->get()));
				collect_lvalues(reg, it->get(), type_eq, type_le, false);
			}
		}
		else if (auto cnd = AstAnyCond::cast(ast)) {
			collect_lvalues(reg, cnd->body(), type_eq, type_le, false);
		}
		else if (auto pb = AstProcBase::cast(ast)) {
			collect_lvalues(reg, pb->body().raw(), type_eq, type_le, false);
		}
		else if (auto blk = AstBlock::cast(ast)) {
			for (auto& child : blk->raw()->children) {
				bool collect = (AstAssignEq::matches(child.get()) && type_eq) ||
				               (AstAssignLe::matches(child.get()) && type_le);
				if (collect)
					reg.append(AstAnyAssign(child.get()).lhs()->genRTLIL());
				if (ChildConstraint<AST_CASE, AST_BLOCK>::accepts(child.get()))
					collect_lvalues(reg, child.get(), type_eq, type_le, false);
			}
		}
		else {
			log_abort();
		}

		if (run_sort_and_unify) {
			std::set<RTLIL::SigBit> sorted_reg;
			for (auto bit : reg)
				if (bit.wire)
					sorted_reg.insert(bit);
			reg = RTLIL::SigSpec(sorted_reg);
		}
	}

	// remove all assignments to the given signal pattern in a case and all its children.
	// e.g. when the last statement in the code "a = 23; if (b) a = 42; a = 0;" is processed this
	// function is called to clean up the first two assignments as they are overwritten by
	// the third assignment.
	void removeSignalFromCaseTree(const pool<RTLIL::SigBit> &pattern_bits, RTLIL::CaseRule *cs)
	{
		// Optimization: check actions in reverse order and stop early if we've found all pattern bits
		pool<RTLIL::SigBit> remaining_bits = pattern_bits;

		for (auto it = cs->actions.rbegin(); it != cs->actions.rend(); ++it) {
			bool has_pattern = false;
			for (auto &bit : it->first) {
				if (bit.wire != NULL && remaining_bits.count(bit)) {
					has_pattern = true;
					remaining_bits.erase(bit);
				}
			}

			if (has_pattern) {
				it->first.remove2(pattern_bits, &it->second);
			}

			// Early exit if we've processed all bits in pattern
			if (remaining_bits.empty())
				break;
		}

		for (auto it = cs->switches.begin(); it != cs->switches.end(); it++)
			for (auto it2 = (*it)->cases.begin(); it2 != (*it)->cases.end(); it2++)
				removeSignalFromCaseTree(pattern_bits, *it2);
	}

	void removeSignalFromCaseTree(const RTLIL::SigSpec &pattern, RTLIL::CaseRule *cs)
	{
		pool<RTLIL::SigBit> pattern_bits;
		pattern_bits.reserve(pattern.size());
		for (auto &bit : pattern)
			if (bit.wire != NULL)
				pattern_bits.insert(bit);
		removeSignalFromCaseTree(pattern_bits, cs);
	}

	// add an assignment (aka "action") but split it up in chunks. this way huge assignments
	// are avoided and the generated $mux cells have a more "natural" size.
	void addChunkActions(std::vector<RTLIL::SigSig> &actions, RTLIL::SigSpec lvalue, RTLIL::SigSpec rvalue, bool inSyncRule = false)
	{
		if (inSyncRule && initSyncSignals.size() > 0) {
			init_lvalue.append(lvalue.extract(initSyncSignals));
			init_rvalue.append(lvalue.extract(initSyncSignals, &rvalue));
			lvalue.remove2(initSyncSignals, &rvalue);
		}
		log_assert(lvalue.size() == rvalue.size());

		int offset = 0;
		for (auto &lvalue_c : lvalue.chunks()) {
			RTLIL::SigSpec lhs = lvalue_c;
			RTLIL::SigSpec rhs = rvalue.extract(offset, lvalue_c.width);
			if (inSyncRule && lvalue_c.wire && lvalue_c.wire->get_bool_attribute(ID::nosync))
				rhs = RTLIL::SigSpec(RTLIL::State::Sx, rhs.size());
			remove_unwanted_lvalue_bits(lhs, rhs);
			actions.push_back(RTLIL::SigSig(lhs, rhs));
			offset += lhs.size();
		}
	}

	// recursively process the AST and fill the RTLIL::Process
	void processAst(AstNode *ast)
	{
		if (auto blk = AstBlock::cast(ast)) {
			for (auto& child : blk->raw()->children)
				processAst(child.get());
			return;
		}

		if (BlockingAssignLike::accepts(ast)) {
			AstAnyAssign assign(ast);
			RTLIL::SigSpec unmapped_lvalue = assign.lhs()->genRTLIL(), lvalue = unmapped_lvalue;
			RTLIL::SigSpec rvalue = assign.rhs()->genWidthRTLIL(lvalue.size(), true, &subst_rvalue_map.stdmap());

			pool<SigBit> lvalue_sigbits;
			for (int i = 0; i < GetSize(lvalue); i++) {
				if (lvalue_sigbits.count(lvalue[i]) > 0) {
					unmapped_lvalue.remove(i);
					lvalue.remove(i);
					rvalue.remove(i--);
				} else
					lvalue_sigbits.insert(lvalue[i]);
			}

			lvalue.replace(subst_lvalue_map.stdmap());

			if (AstAssignEq::matches(ast)) {
				for (int i = 0; i < GetSize(unmapped_lvalue); i++)
					subst_rvalue_map.set(unmapped_lvalue[i], rvalue[i]);
			}

			// Check if any bits in lvalue have been assigned before in current_case
			bool has_overlap = false;
			for (auto &bit : lvalue) {
				if (bit.wire != NULL && current_case_assigned_bits.count(bit)) {
					has_overlap = true;
					break;
				}
			}

			if (has_overlap)
				removeSignalFromCaseTree(lvalue, current_case);

			// Track newly assigned bits
			for (auto &bit : lvalue)
				if (bit.wire != NULL)
					current_case_assigned_bits.insert(bit);

			remove_unwanted_lvalue_bits(lvalue, rvalue);
			current_case->actions.push_back(RTLIL::SigSig(lvalue, rvalue));
			return;
		}

		if (auto cs = AstCase::cast(ast)) {
			int width_hint;
			bool sign_hint;
			ast->detectSignWidth(width_hint, sign_hint);

			RTLIL::SwitchRule *sw = new RTLIL::SwitchRule;
			set_src_attr(sw, ast);
			sw->signal = cs->selector()->genWidthRTLIL(width_hint, sign_hint, &subst_rvalue_map.stdmap());
			current_case->switches.push_back(sw);

			copy_const_attributes(sw, ast);

			RTLIL::SigSpec this_case_eq_lvalue;
			collect_lvalues(this_case_eq_lvalue, ast, true, false);

			RTLIL::SigSpec this_case_eq_ltemp = new_temp_signal(this_case_eq_lvalue);

			RTLIL::SigSpec this_case_eq_rvalue = this_case_eq_lvalue;
			this_case_eq_rvalue.replace(subst_rvalue_map.stdmap());

			RTLIL::CaseRule *default_case = nullptr;
			RTLIL::CaseRule *last_generated_case = nullptr;
			for (auto it = cs->conditions_begin(); it != cs->conditions_end(); ++it)
			{
				auto& child = *it;
				log_assert(AstAnyCond::matches(child.get()));

				subst_lvalue_map.save();
				subst_rvalue_map.save();

				for (int i = 0; i < GetSize(this_case_eq_lvalue); i++)
					subst_lvalue_map.set(this_case_eq_lvalue[i], this_case_eq_ltemp[i]);

				RTLIL::CaseRule *backup_case = current_case;
				current_case = new RTLIL::CaseRule;
				pool<RTLIL::SigBit> backup_assigned_bits = std::move(current_case_assigned_bits);
				current_case_assigned_bits.clear();
				set_src_attr(current_case, child.get());
				last_generated_case = current_case;
				addChunkActions(current_case->actions, this_case_eq_ltemp, this_case_eq_rvalue);
				// Track temp assignments
				for (auto &bit : this_case_eq_ltemp)
					if (bit.wire != NULL)
						current_case_assigned_bits.insert(bit);
				for (auto& node : child->children) {
					if (AstDefault::matches(node.get()))
						default_case = current_case;
					else if (AstBlock::matches(node.get()))
						processAst(node.get());
					else
						current_case->compare.push_back(node->genWidthRTLIL(width_hint, sign_hint, &subst_rvalue_map.stdmap()));
				}
				if (default_case != current_case)
					sw->cases.push_back(current_case);
				else
					log_assert(current_case->compare.size() == 0);
				current_case = backup_case;
				current_case_assigned_bits = std::move(backup_assigned_bits);

				subst_lvalue_map.restore();
				subst_rvalue_map.restore();
			}

			if (last_generated_case != nullptr && ast->get_bool_attribute(ID::full_case) && default_case == nullptr) {
		#if 0
				// this is a valid transformation, but as optimization it is premature.
				// better: add a default case that assigns 'x' to everything, and let later
				// optimizations take care of the rest
				last_generated_case->compare.clear();
		#else
				default_case = new RTLIL::CaseRule;
				addChunkActions(default_case->actions, this_case_eq_ltemp, SigSpec(State::Sx, GetSize(this_case_eq_rvalue)));
				sw->cases.push_back(default_case);
		#endif
			} else {
				if (default_case == nullptr) {
					default_case = new RTLIL::CaseRule;
					addChunkActions(default_case->actions, this_case_eq_ltemp, this_case_eq_rvalue);
				}
				sw->cases.push_back(default_case);
			}

			for (int i = 0; i < GetSize(this_case_eq_lvalue); i++)
				subst_rvalue_map.set(this_case_eq_lvalue[i], this_case_eq_ltemp[i]);

			this_case_eq_lvalue.replace(subst_lvalue_map.stdmap());

			// Check if any bits in lvalue have been assigned before in current_case
			bool has_overlap = false;
			for (auto &bit : this_case_eq_lvalue) {
				if (bit.wire != NULL && current_case_assigned_bits.count(bit)) {
					has_overlap = true;
					break;
				}
			}

			if (has_overlap)
				removeSignalFromCaseTree(this_case_eq_lvalue, current_case);

			addChunkActions(current_case->actions, this_case_eq_lvalue, this_case_eq_ltemp);
			// Track newly assigned bits
			for (auto &bit : this_case_eq_lvalue)
				if (bit.wire != NULL)
					current_case_assigned_bits.insert(bit);
			return;
		}

		if (AstWire::matches(ast))
			ast->input_error("Found reg declaration in block without label!\n");

		if (AstAssign::matches(ast))
			ast->input_error("Found continuous assignment in always/initial block!\n");

		if (AstAnyParamLike::matches(ast))
			ast->input_error("Found parameter declaration in block without label!\n");

		if (auto tc = AstTcall::cast(ast)) {
			if (tc->str() == "$display" || tc->str() == "$displayb" || tc->str() == "$displayh" || tc->str() == "$displayo" ||
		  tc->str() == "$write"   || tc->str() == "$writeb"   || tc->str() == "$writeh"   || tc->str() == "$writeo") {
				std::stringstream sstr;
				sstr << tc->str() << "$" << ast->location.begin.filename << ":" << ast->location.begin.line << "$" << (autoidx++);

				Wire *en = current_module->addWire(sstr.str() + "_EN", 1);
				set_src_attr(en, ast);
				proc->root_case.actions.push_back(SigSig(en, false));
				current_case->actions.push_back(SigSig(en, true));

				RTLIL::SigSpec triggers;
				RTLIL::Const::Builder polarity_builder;
				for (auto sync : proc->syncs) {
					if (sync->type == RTLIL::STp) {
						triggers.append(sync->signal);
						polarity_builder.push_back(RTLIL::S1);
					} else if (sync->type == RTLIL::STn) {
						triggers.append(sync->signal);
						polarity_builder.push_back(RTLIL::S0);
					}
				}
				RTLIL::Const polarity = polarity_builder.build();

				RTLIL::Cell *cell = current_module->addCell(sstr.str(), ID($print));
				set_src_attr(cell, ast);
				cell->setParam(ID::TRG_WIDTH, triggers.size());
				cell->setParam(ID::TRG_ENABLE, (AstInitial::matches(always.get())) || !triggers.empty());
				cell->setParam(ID::TRG_POLARITY, polarity);
				cell->setParam(ID::PRIORITY, --last_effect_priority);
				cell->setPort(ID::TRG, triggers);
				cell->setPort(ID::EN, en);

				int default_base = 10;
				if (tc->str().back() == 'b')
					default_base = 2;
				else if (tc->str().back() == 'o')
					default_base = 8;
				else if (tc->str().back() == 'h')
					default_base = 16;

				std::vector<VerilogFmtArg> args;
				for (auto& node : ast->children) {
					int width;
					bool is_signed;
					node->detectSignWidth(width, is_signed, nullptr);

					VerilogFmtArg arg = {};
					arg.filename = *node->location.begin.filename;
					arg.first_line = node->location.begin.line;
					if (auto c = AstConstant::cast(node.get()); c && c->is_string()) {
						arg.type = VerilogFmtArg::STRING;
						arg.str = node->bitsAsConst().decode_string();
						// and in case this will be used as an argument...
						arg.sig = node->bitsAsConst();
						arg.signed_ = false;
					} else if (AstIdentifier::matches(node.get()) && node->str == "$time") {
						arg.type = VerilogFmtArg::TIME;
					} else if (AstIdentifier::matches(node.get()) && node->str == "$realtime") {
						arg.type = VerilogFmtArg::TIME;
						arg.realtime = true;
					} else {
						arg.type = VerilogFmtArg::INTEGER;
						arg.sig = node->genWidthRTLIL(-1, false, &subst_rvalue_map.stdmap());
						arg.signed_ = is_signed;
					}
					args.push_back(arg);
				}

				Fmt fmt;
				fmt.parse_verilog(args, /*sformat_like=*/false, default_base, /*task_name=*/tc->str(), current_module->name);
				if (tc->str().substr(0, 8) == "$display")
					fmt.append_literal("\n");
				fmt.emit_rtlil(cell);
			} else if (!tc->str().empty()) {
				log_file_error(*ast->location.begin.filename, ast->location.begin.line, "Found unsupported invocation of system task `%s'!\n", tc->str());
			}
			return;
		}

		if (FormalAssertions::accepts(ast)) {
			std::string flavor, desc;
			if (AstAssert::matches(ast)) { flavor = "assert"; desc = "assert ()"; }
			if (AstAssume::matches(ast)) { flavor = "assume"; desc = "assume ()"; }
			if (AstLive::matches(ast)) { flavor = "live"; desc = "assert (eventually)"; }
			if (AstFair::matches(ast)) { flavor = "fair"; desc = "assume (eventually)"; }
			if (AstCover::matches(ast)) { flavor = "cover"; desc = "cover ()"; }

			IdString cellname;
			if (ast->str.empty())
				cellname = stringf("$%s$%s:%d$%d", flavor, RTLIL::encode_filename(*ast->location.begin.filename), ast->location.begin.line, autoidx++);
			else
				cellname = ast->str;
			check_unique_id(current_module, cellname, ast, "procedural assertion");

			RTLIL::SigSpec check = ast->children[0]->genWidthRTLIL(-1, false, &subst_rvalue_map.stdmap());
			if (GetSize(check) != 1)
				check = current_module->ReduceBool(NEW_ID, check);

			Wire *en = current_module->addWire(cellname.str() + "_EN", 1);
			set_src_attr(en, ast);
			proc->root_case.actions.push_back(SigSig(en, false));
			current_case->actions.push_back(SigSig(en, true));

			RTLIL::SigSpec triggers;
			RTLIL::Const::Builder polarity_builder;
			for (auto sync : proc->syncs) {
				if (sync->type == RTLIL::STp) {
					triggers.append(sync->signal);
					polarity_builder.push_back(RTLIL::S1);
				} else if (sync->type == RTLIL::STn) {
					triggers.append(sync->signal);
					polarity_builder.push_back(RTLIL::S0);
				}
			}
			RTLIL::Const polarity = polarity_builder.build();

			RTLIL::Cell *cell = current_module->addCell(cellname, ID($check));
			set_src_attr(cell, ast);
			cell->set_bool_attribute(ID(keep));
			copy_const_attributes(cell, ast);
			cell->setParam(ID::FLAVOR, flavor);
			cell->setParam(ID::TRG_WIDTH, triggers.size());
			cell->setParam(ID::TRG_ENABLE, (AstInitial::matches(always.get())) || !triggers.empty());
			cell->setParam(ID::TRG_POLARITY, polarity);
			cell->setParam(ID::PRIORITY, --last_effect_priority);
			cell->setPort(ID::TRG, triggers);
			cell->setPort(ID::EN, en);
			cell->setPort(ID::A, check);

			// No message is emitted to ensure Verilog code roundtrips correctly.
			Fmt fmt;
			fmt.emit_rtlil(cell);
			return;
		}

		if (AstNone::matches(ast) || ast->type == AST_FOR)
			return;

		// ast->dumpAst(nullptr, "ast> ");
		// current_ast_mod->dumpAst(nullptr, "mod> ");
		log_abort();
	}

	void processMemWrites(RTLIL::SyncRule *sync)
	{
		// Maps per-memid AST_MEMWR IDs to indices in the mem_write_actions array.
		dict<std::pair<std::string, int>, int> port_map;
		for (auto& child : always->children)
			if (auto wr = AstMemWr::cast(child.get()))
			{
				std::string memid = child->str;
				int portid = wr->portid()->asInt(false);
				int cur_idx = GetSize(sync->mem_write_actions);
				RTLIL::MemWriteAction action;
				set_src_attr(&action, child.get());
				action.memid = memid;
				action.address = wr->addr()->genWidthRTLIL(-1, true, &subst_rvalue_map.stdmap());
				action.data = wr->data()->genWidthRTLIL(current_module->memories[memid]->width, true, &subst_rvalue_map.stdmap());
				action.enable = wr->en()->genWidthRTLIL(-1, true, &subst_rvalue_map.stdmap());
				RTLIL::Const orig_priority_mask = wr->prio_mask()->bitsAsConst();
				RTLIL::Const priority_mask = RTLIL::Const(0, cur_idx);
				for (int i = 0; i < portid; i++) {
					int new_bit = port_map[std::make_pair(memid, i)];
					priority_mask.set(new_bit, orig_priority_mask[i]);
				}
				action.priority_mask = priority_mask;
				sync->mem_write_actions.push_back(action);
				port_map[std::make_pair(memid, portid)] = cur_idx;
			}
	}
};

RTLIL::SigSpec AST_INTERNAL::generate_process(std::unique_ptr<AstNode> always,
		RTLIL::SigSpec initSyncSignals)
{
	ProcessGenerator generator(std::move(always), initSyncSignals);
	return generator.outputSignals;
}

YOSYS_NAMESPACE_END
