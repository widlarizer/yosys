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
 *  This is the AST frontend library.
 *
 *  The AST frontend library is not a frontend on it's own but provides a
 *  generic abstract syntax tree (AST) abstraction for HDL code and can be
 *  used by HDL frontends. See "ast.h" for an overview of the API and the
 *  Verilog frontend for an usage example.
 *
 */

#include "kernel/log.h"
#include "libs/sha1/sha1.h"
#include "frontends/verilog/verilog_frontend.h"
#include "ast.h"
#include "ast_typed.h"
#include "kernel/io.h"

#include <sstream>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
#include <optional>
#include <numeric>

YOSYS_NAMESPACE_BEGIN

using namespace AST;
using namespace AST_INTERNAL;


// helper function for mem2reg_as_needed_pass1
static void mark_memories_assign_lhs_complex(dict<AstNode*, pool<std::string>> &mem2reg_places,
		dict<AstNode*, uint32_t> &mem2reg_candidates, AstNode *that)
{
	for (auto &child : that->children)
		mark_memories_assign_lhs_complex(mem2reg_places, mem2reg_candidates, child.get());

	auto id = AstIdentifier::cast(that);
	if (id && AstMemory::matches(that->id2ast)) {
		AstNode *mem = that->id2ast;
		if (!(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_CMPLX_LHS))
			mem2reg_places[mem].insert(stringf("%s:%d", RTLIL::encode_filename(*that->location.begin.filename), that->location.begin.line));
		mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_CMPLX_LHS;
	}
}

// find memories that should be replaced by registers
void AstNode::mem2reg_as_needed_pass1(dict<AstNode*, pool<std::string>> &mem2reg_places,
		dict<AstNode*, uint32_t> &mem2reg_candidates, dict<AstNode*, uint32_t> &proc_flags, uint32_t &flags)
{
	uint32_t children_flags = 0;
	int lhs_children_counter = 0;

	if (AstTypedef::matches(this))
		return; // don't touch content of typedefs

	if (auto asgn = AstAnyAssign::cast(this))
	{
		// mark all memories that are used in a complex expression on the left side of an assignment
		for (auto &lhs_child : asgn->lhs()->children)
			mark_memories_assign_lhs_complex(mem2reg_places, mem2reg_candidates, lhs_child.get());

		auto lhs_id = AstIdentifier::cast(asgn->lhs());
		if (lhs_id && AstMemory::matches(asgn->lhs()->id2ast))
		{
			AstNode *mem = asgn->lhs()->id2ast;

			// activate mem2reg if this is assigned in an async proc
			if (flags & AstNode::MEM2REG_FL_ASYNC) {
				if (!(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_SET_ASYNC))
					mem2reg_places[mem].insert(stringf("%s:%d", RTLIL::encode_filename(*location.begin.filename), location.begin.line));
				mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_SET_ASYNC;
			}

			// remember if this is assigned blocking (=)
			if (AstAssignEq::matches(this)) {
				if (!(proc_flags[mem] & AstNode::MEM2REG_FL_EQ1))
					mem2reg_places[mem].insert(stringf("%s:%d", RTLIL::encode_filename(*location.begin.filename), location.begin.line));
				proc_flags[mem] |= AstNode::MEM2REG_FL_EQ1;
			}

			// for proper (non-init) writes: remember if this is a constant index or not
			if ((flags & MEM2REG_FL_INIT) == 0) {
				AstNode *lhs = asgn->lhs();
				if (lhs->children.size() && AstRange::matches(lhs->children[0].get()) && lhs->children[0]->children.size()) {
					if (AstConstant::matches(lhs->children[0]->children[0].get()))
						mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_CONST_LHS;
					else
						mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_VAR_LHS;
				}
			}

			// remember where this is
			if (flags & MEM2REG_FL_INIT) {
				if (!(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_SET_INIT))
					mem2reg_places[mem].insert(stringf("%s:%d", RTLIL::encode_filename(*location.begin.filename), location.begin.line));
				mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_SET_INIT;
			} else {
				if (!(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_SET_ELSE))
					mem2reg_places[mem].insert(stringf("%s:%d", RTLIL::encode_filename(*location.begin.filename), location.begin.line));
				mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_SET_ELSE;
			}
		}

		lhs_children_counter = 1;
	}

	if (AstIdentifier::matches(this) && AstMemory::matches(id2ast))
	{
		AstNode *mem = id2ast;

		if (integer < (unsigned)mem->unpacked_dimensions)
			input_error("Insufficient number of array indices for %s.\n", log_id(str));

		// flag if used after blocking assignment (in same proc)
		if ((proc_flags[mem] & AstNode::MEM2REG_FL_EQ1) && !(mem2reg_candidates[mem] & AstNode::MEM2REG_FL_EQ2)) {
			mem2reg_places[mem].insert(stringf("%s:%d", RTLIL::encode_filename(*location.begin.filename), location.begin.line));
			mem2reg_candidates[mem] |= AstNode::MEM2REG_FL_EQ2;
		}
	}

	// also activate if requested, either by using mem2reg attribute or by declaring array as 'wire' instead of 'reg' or 'logic'
	if (AstMemory::matches(this) && (get_bool_attribute(ID::mem2reg) || (flags & AstNode::MEM2REG_FL_ALL) || !(is_reg || is_logic)))
		mem2reg_candidates[this] |= AstNode::MEM2REG_FL_FORCED;

	if (ModuleLike::accepts(this) && get_bool_attribute(ID::mem2reg))
		children_flags |= AstNode::MEM2REG_FL_ALL;

	dict<AstNode*, uint32_t> *proc_flags_p = nullptr;

	if (auto proc = AstProcBase::cast(this)) {
		children_flags |= proc->mem2reg_root_flags();
		proc_flags_p = new dict<AstNode*, uint32_t>;
	}

	uint32_t backup_flags = flags;
	flags |= children_flags;
	log_assert((flags & ~0x000000ff) == 0);

	for (auto& child : children)
	{
		if (lhs_children_counter > 0) {
			lhs_children_counter--;
			if (child->children.size() && AstRange::matches(child->children[0].get()) && child->children[0]->children.size()) {
				for (auto& c : child->children[0]->children) {
					if (proc_flags_p)
						c->mem2reg_as_needed_pass1(mem2reg_places, mem2reg_candidates, *proc_flags_p, flags);
					else
						c->mem2reg_as_needed_pass1(mem2reg_places, mem2reg_candidates, proc_flags, flags);
				}
			}
		} else
		if (proc_flags_p)
			child->mem2reg_as_needed_pass1(mem2reg_places, mem2reg_candidates, *proc_flags_p, flags);
		else
			child->mem2reg_as_needed_pass1(mem2reg_places, mem2reg_candidates, proc_flags, flags);
	}

	flags &= ~children_flags | backup_flags;

	if (proc_flags_p) {
#ifndef NDEBUG
		for (auto it : *proc_flags_p)
			log_assert((it.second & ~0xff000000) == 0);
#endif
		delete proc_flags_p;
	}
}

bool AstNode::mem2reg_check(pool<AstNode*> &mem2reg_set)
{
	if (!AstIdentifier::matches(this) || !id2ast || !mem2reg_set.count(id2ast))
		return false;

	if (children.empty() || !AstRange::matches(children[0].get()) || GetSize(children[0]->children) != 1)
		input_error("Invalid array access.\n");

	return true;
}

void AstNode::mem2reg_remove(pool<AstNode*> &mem2reg_set)
{
	log_assert(mem2reg_set.count(this) == 0);

	if (mem2reg_set.count(id2ast))
		id2ast = nullptr;

	for (size_t i = 0; i < children.size(); i++) {
		if (mem2reg_set.count(children[i].get()) > 0) {
			children.erase(children.begin() + (i--));
		} else {
			children[i]->mem2reg_remove(mem2reg_set);
		}
	}
}

// actually replace memories with registers
bool AstNode::mem2reg_as_needed_pass2(pool<AstNode*> &mem2reg_set, AstNode *mod, AstNode *block, AstNode* async_block)
{
	bool did_something = false;

	if (AstBlock::matches(this))
		block = this;

	if (FunctionTaskLike::accepts(this))
		return false;

	if (AstTypedef::matches(this))
		return false;

	if (AstMemInit::matches(this) && id2ast && mem2reg_set.count(id2ast))
	{
		log_assert(AstConstant::matches(children[0].get()));
		log_assert(AstConstant::matches(children[1].get()));
		log_assert(AstConstant::matches(children[2].get()));
		log_assert(AstConstant::matches(children[3].get()));

		int cursor = children[0]->asInt(false);
		Const data = children[1]->bitsAsConst();
		Const en = children[2]->bitsAsConst();
		int length = children[3]->asInt(false);

		if (length != 0)
		{
			auto block_owned = std::make_unique<AstNode>(location,
				AST_INITIAL, std::make_unique<AstNode>(location,
					AST_BLOCK));
			auto block = block_owned.get();
			mod->children.push_back(std::move(block_owned));
			block = block->children[0].get();

			int wordsz = GetSize(data) / length;

			for (int i = 0; i < length; i++) {
				int pos = 0;
				while (pos < wordsz) {
					if (en[pos] != State::S1) {
						pos++;
					} else {
						int epos = pos + 1;
						while (epos < wordsz && en[epos] == State::S1)
							epos++;
						int clen = epos - pos;
						auto range = std::make_unique<AstNode>(location,
							AST_RANGE, AstNode::mkconst_int(location,
								cursor+i, false));
						if (pos != 0 || epos != wordsz) {
							int left;
							int right;
							auto& mrange = id2ast->children[0];
							if (mrange->range_left < mrange->range_right) {
								right = mrange->range_right - pos;
								left = mrange->range_right - epos + 1;
							} else {
								right = mrange->range_right + pos;
								left = mrange->range_right + epos - 1;
							}
							range = std::make_unique<AstNode>(location,
								AST_MULTIRANGE, std::move(range), std::make_unique<AstNode>(location,
									AST_RANGE,
									AstNode::mkconst_int(location, left, true),
									AstNode::mkconst_int(location, right, true)));
						}
						auto target = std::make_unique<AstNode>(location, AST_IDENTIFIER, std::move(range));
						target->str = str;
						target->id2ast = id2ast;
						target->was_checked = true;
						block->children.push_back(std::make_unique<AstNode>(location,
							AST_ASSIGN_EQ,
							std::move(target),
							mkconst_bits(location,
								data.extract(i*wordsz + pos, clen).to_bits(),
								false)));
						pos = epos;
					}
				}
			}
		}

		auto newNode = std::make_unique<AstNode>(location, AST_NONE);
		newNode->cloneInto(*this);
		did_something = true;
	}

	if (AstAssign::matches(this) && block == nullptr && children[0]->mem2reg_check(mem2reg_set))
	{
		if (async_block == nullptr) {
			auto async_block_owned = std::make_unique<AstNode>(location, AST_ALWAYS, std::make_unique<AstNode>(location, AST_BLOCK));
			async_block = async_block_owned.get();
			mod->children.push_back(std::move(async_block_owned));
		}

		auto newNode = clone();
		// AST_ASSIGN → AST_ASSIGN_EQ: same 2-child [lhs, rhs] shape (grammar §15).
		AstAssign old_view(newNode.get());
		auto lhs_c = old_view.lhs().take();
		auto rhs_c = old_view.rhs().take();
		lhs_c->was_checked = true;
		reshape_as<AST_ASSIGN_EQ>(newNode.get(), std::move(lhs_c), std::move(rhs_c));
		async_block->children[0]->children.push_back(std::move(newNode));

		newNode = std::make_unique<AstNode>(location, AST_NONE);
		newNode->cloneInto(*this);
		did_something = true;
	}

	if (auto indexed = BlockingAssignLike::accepts(this) && children[0]->mem2reg_check(mem2reg_set)
			? AstAnyAssign(this).lhs_as_indexed_identifier() : std::nullopt;
			indexed && !AstConstant::matches(indexed->index_expr))
	{
		AstNode *index_expr = indexed->index_expr;

		std::stringstream sstr;
		sstr << "$mem2reg_wr$" << children[0]->str << "$" << RTLIL::encode_filename(*location.begin.filename) << ":" << location.begin.line << "$" << (autoidx++);
		std::string id_addr = sstr.str() + "_ADDR", id_data = sstr.str() + "_DATA";

		AstMemory mem(children[0]->id2ast);
		int mem_width, mem_size, addr_bits;
		bool mem_signed = mem.is_signed();
		mem.meminfo(mem_width, mem_size, addr_bits);

		auto wire_addr = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, addr_bits-1, true), mkconst_int(location, 0, true)));
		wire_addr->str = id_addr;
		wire_addr->is_reg = true;
		wire_addr->was_checked = true;
		wire_addr->set_attribute(ID::nosync, AstNode::mkconst_int(location, 1, false));
		while (wire_addr->simplify()) { }
		mod->children.push_back(std::move(wire_addr));

		auto wire_data = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, mem_width-1, true), mkconst_int(location, 0, true)));
		wire_data->str = id_data;
		wire_data->is_reg = true;
		wire_data->was_checked = true;
		wire_data->is_signed = mem_signed;
		wire_data->set_attribute(ID::nosync, AstNode::mkconst_int(location, 1, false));
		while (wire_data->simplify()) { }
		mod->children.push_back(std::move(wire_data));

		log_assert(block != nullptr);
		size_t assign_idx = 0;
		while (assign_idx < block->children.size() && block->children[assign_idx].get() != this)
			assign_idx++;
		log_assert(assign_idx < block->children.size());

		auto assign_addr = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), index_expr->clone());
		assign_addr->children[0]->str = id_addr;
		assign_addr->children[0]->was_checked = true;
		block->children.insert(block->children.begin()+assign_idx+1, std::move(assign_addr));

		auto case_node = std::make_unique<AstNode>(location, AST_CASE, std::make_unique<AstNode>(location, AST_IDENTIFIER));
		case_node->children[0]->str = id_addr;
		for (int i = 0; i < mem_size; i++) {
			if (AstConstant::matches(index_expr) && int(index_expr->integer) != i)
				continue;
			auto cond_node = std::make_unique<AstNode>(location, AST_COND, AstNode::mkconst_int(location, i, false, addr_bits), std::make_unique<AstNode>(location, AST_BLOCK));
			auto assign_reg = std::make_unique<AstNode>(location, type, std::make_unique<AstNode>(location, AST_IDENTIFIER), std::make_unique<AstNode>(location, AST_IDENTIFIER));
			if (children[0]->children.size() == 2)
				assign_reg->children[0]->children.push_back(children[0]->children[1]->clone());
			assign_reg->children[0]->str = stringf("%s[%d]", children[0]->str, i);
			assign_reg->children[1]->str = id_data;
			cond_node->children[1]->children.push_back(std::move(assign_reg));
			case_node->children.push_back(std::move(cond_node));
		}

		// fixup on the full hierarchy below case_node
		case_node->fixup_hierarchy_flags(true);

		block->children.insert(block->children.begin()+assign_idx+2, std::move(case_node));

		// Morph AST_ASSIGN_LE/AST_ASSIGN_EQ → AST_ASSIGN_EQ (same [lhs, rhs] shape §15).
		AstNode *lhs_ptr = children[0].get();
		lhs_ptr->delete_children();
		lhs_ptr->range_valid = false;
		lhs_ptr->id2ast = nullptr;
		lhs_ptr->str = id_data;
		lhs_ptr->was_checked = true;
		type = AST_ASSIGN_EQ;

		fixup_hierarchy_flags();
		did_something = true;
	}

	if (mem2reg_check(mem2reg_set))
	{
		std::unique_ptr<AstNode> bit_part_sel = nullptr;
		if (children.size() == 2)
			bit_part_sel = children[1]->clone();

		if (AstConstant::matches(children[0]->children[0].get()))
		{
			int id = children[0]->children[0]->integer;
			int left = id2ast->children[1]->children[0]->integer;
			int right = id2ast->children[1]->children[1]->integer;
			bool valid_const_access =
				(left <= id && id <= right) ||
				(right <= id && id <= left);
			if (valid_const_access)
			{
				str = stringf("%s[%d]", str, id);
				delete_children();
				range_valid = false;
				id2ast = nullptr;
			}
			else
			{
				int width;
				if (bit_part_sel)
				{
					// bit_part_sel->dumpAst(nullptr, "? ");
					if (bit_part_sel->children.size() == 1)
						width = 0;
					else
						width = bit_part_sel->children[0]->integer -
							bit_part_sel->children[1]->integer;
					bit_part_sel = nullptr;
				}
				else
				{
					width = id2ast->children[0]->children[0]->integer -
						id2ast->children[0]->children[1]->integer;
				}
				width = abs(width) + 1;

				delete_children();

				std::vector<RTLIL::State> x_bits;
				for (int i = 0; i < width; i++)
					x_bits.push_back(RTLIL::State::Sx);
				auto constant = AstNode::mkconst_bits(location, x_bits, false);
				constant->cloneInto(*this);
			}
		}
		else
		{
			std::stringstream sstr;
			sstr << "$mem2reg_rd$" << str << "$" << RTLIL::encode_filename(*location.begin.filename) << ":" << location.begin.line << "$" << (autoidx++);
			std::string id_addr = sstr.str() + "_ADDR", id_data = sstr.str() + "_DATA";

			AstMemory mem(id2ast);
			int mem_width, mem_size, addr_bits;
			bool mem_signed = mem.is_signed();
			mem.meminfo(mem_width, mem_size, addr_bits);

			auto wire_addr = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, addr_bits-1, true), mkconst_int(location, 0, true)));
			wire_addr->str = id_addr;
			wire_addr->is_reg = true;
			wire_addr->was_checked = true;
			if (block)
				wire_addr->set_attribute(ID::nosync, AstNode::mkconst_int(location, 1, false));
			while (wire_addr->simplify()) { }
			mod->children.push_back(std::move(wire_addr));

			auto wire_data = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, mem_width-1, true), mkconst_int(location, 0, true)));
			wire_data->str = id_data;
			wire_data->is_reg = true;
			wire_data->was_checked = true;
			wire_data->is_signed = mem_signed;
			if (block)
				wire_data->set_attribute(ID::nosync, AstNode::mkconst_int(location, 1, false));
			while (wire_data->simplify()) { }
			mod->children.push_back(std::move(wire_data));

			auto assign_addr = std::make_unique<AstNode>(location, block ? AST_ASSIGN_EQ : AST_ASSIGN, std::make_unique<AstNode>(location, AST_IDENTIFIER), children[0]->children[0]->clone());
			assign_addr->children[0]->str = id_addr;
			assign_addr->children[0]->was_checked = true;

			auto case_node = std::make_unique<AstNode>(location, AST_CASE, std::make_unique<AstNode>(location, AST_IDENTIFIER));
			case_node->children[0]->str = id_addr;

			for (int i = 0; i < mem_size; i++) {
				if (AstConstant::matches(children[0]->children[0].get()) && int(children[0]->children[0]->integer) != i)
					continue;
				auto cond_node = std::make_unique<AstNode>(location, AST_COND, AstNode::mkconst_int(location, i, false, addr_bits), std::make_unique<AstNode>(location, AST_BLOCK));
				auto assign_reg = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), std::make_unique<AstNode>(location, AST_IDENTIFIER));
				assign_reg->children[0]->str = id_data;
				assign_reg->children[0]->was_checked = true;
				assign_reg->children[1]->str = stringf("%s[%d]", str, i);
				cond_node->children[1]->children.push_back(std::move(assign_reg));
				case_node->children.push_back(std::move(cond_node));
			}

			std::vector<RTLIL::State> x_bits;
			for (int i = 0; i < mem_width; i++)
				x_bits.push_back(RTLIL::State::Sx);

			auto cond_node = std::make_unique<AstNode>(location, AST_COND, std::make_unique<AstNode>(location, AST_DEFAULT), std::make_unique<AstNode>(location, AST_BLOCK));
			auto assign_reg = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), AstNode::mkconst_bits(location, x_bits, false));
			assign_reg->children[0]->str = id_data;
			assign_reg->children[0]->was_checked = true;
			cond_node->children[1]->children.push_back(std::move(assign_reg));
			case_node->children.push_back(std::move(cond_node));

			// fixup on the full hierarchy below case_node
			case_node->fixup_hierarchy_flags(true);

			if (block)
			{
				size_t assign_idx = 0;
				while (assign_idx < block->children.size() && !block->children[assign_idx]->contains(this))
					assign_idx++;
				log_assert(assign_idx < block->children.size());
				block->children.insert(block->children.begin()+assign_idx, std::move(case_node));
				block->children.insert(block->children.begin()+assign_idx, std::move(assign_addr));
			}
			else
			{
				auto proc = std::make_unique<AstNode>(location, AST_ALWAYS, std::make_unique<AstNode>(location, AST_BLOCK, std::move(case_node)));
				mod->children.push_back(std::move(proc));
				mod->children.push_back(std::move(assign_addr));
				mod->fixup_hierarchy_flags();
			}

			delete_children();
			range_valid = false;
			id2ast = nullptr;
			str = id_data;
		}

		if (bit_part_sel) {
			children.push_back(std::move(bit_part_sel));
			fixup_hierarchy_flags();
		}

		did_something = true;
	}

	log_assert(id2ast == nullptr || mem2reg_set.count(id2ast) == 0);

	std::vector<AstNode*> children_list;
	for (auto& child : children)
		children_list.push_back(child.get());

	for (size_t i = 0; i < children_list.size(); i++)
		if (children_list[i]->mem2reg_as_needed_pass2(mem2reg_set, mod, block, async_block))
			did_something = true;

	return did_something;
}

// replace a readmem[bh] TCALL ast node with a block of memory assignments
std::unique_ptr<AstNode> AstNode::readmem(bool is_readmemh, std::string mem_filename, AstNode *memory, int start_addr, int finish_addr, bool unconditional_init)
{
	AstMemory mem(memory);
	int mem_width, mem_size, addr_bits;
	mem.meminfo(mem_width, mem_size, addr_bits);

	auto block = std::make_unique<AstNode>(location, AST_BLOCK);

	AstNode* meminit = nullptr;
	int next_meminit_cursor=0;
	vector<State> meminit_bits;
	vector<State> en_bits;
	int meminit_size=0;

	for (int i = 0; i < mem_width; i++)
		en_bits.push_back(State::S1);

	std::ifstream f;
	f.open(mem_filename.c_str());
	if (f.fail()) {
		std::string path = parent_from_file_path(*location.begin.filename);
		f.open(path + mem_filename.c_str());
		yosys_input_files.insert(path + mem_filename);
	} else {
		yosys_input_files.insert(mem_filename);
	}
	if (f.fail() || GetSize(mem_filename) == 0)
		input_error("Can not open file `%s` for %s.\n", mem_filename, str);

	log_assert(GetSize(memory->children) == 2 && AstRange::matches(memory->children[1].get()) && memory->children[1]->range_valid);
	int range_left =  memory->children[1]->range_left, range_right =  memory->children[1]->range_right;
	int range_min = min(range_left, range_right), range_max = max(range_left, range_right);

	if (start_addr < 0)
		start_addr = range_min;

	if (finish_addr < 0)
		finish_addr = range_max + 1;

	bool in_comment = false;
	int increment = start_addr <= finish_addr ? +1 : -1;
	int cursor = start_addr;

	while (!f.eof())
	{
		std::string line, token;
		std::getline(f, line);

		for (int i = 0; i < GetSize(line); i++) {
			if (in_comment && line.compare(i, 2, "*/") == 0) {
				line[i] = ' ';
				line[i+1] = ' ';
				in_comment = false;
				continue;
			}
			if (!in_comment && line.compare(i, 2, "/*") == 0)
				in_comment = true;
			if (in_comment)
				line[i] = ' ';
		}

		while (1)
		{
			token = next_token(line, " \t\r\n");
			if (token.empty() || token.compare(0, 2, "//") == 0)
				break;

			if (token[0] == '@') {
				token = token.substr(1);
				const char *nptr = token.c_str();
				char *endptr;
				cursor = strtol(nptr, &endptr, 16);
				if (!*nptr || *endptr)
					input_error("Can not parse address `%s` for %s.\n", nptr, str);
				continue;
			}

			VERILOG_FRONTEND::ConstParser p{memory->location};
			auto value = p.const2ast(stringf("%d'%c", mem_width, is_readmemh ? 'h' : 'b') + token);

			if (unconditional_init)
			{
				if (meminit == nullptr || cursor != next_meminit_cursor)
				{
					if (meminit != nullptr) {
						AstMemInit prev(meminit);
						prev.set_data(AstNode::mkconst_bits(location, meminit_bits, false));
						prev.set_count(AstNode::mkconst_int(location, meminit_size, false));
					}

					auto meminit_owned = std::make_unique<AstNode>(location, AST_MEMINIT);
					meminit = meminit_owned.get();
					meminit->children.push_back(AstNode::mkconst_int(location, cursor, false));
					meminit->children.push_back(nullptr);
					meminit->children.push_back(AstNode::mkconst_bits(location, en_bits, false));
					meminit->children.push_back(nullptr);
					meminit->str = memory->str;
					meminit->id2ast = memory;
					meminit_bits.clear();
					meminit_size = 0;

					current_ast_mod->children.push_back(std::move(meminit_owned));
					next_meminit_cursor = cursor;
				}

				meminit_size++;
				next_meminit_cursor++;
				meminit_bits.insert(meminit_bits.end(), value->bits.begin(), value->bits.end());
			}
			else
			{
				block->children.push_back(
					std::make_unique<AstNode>(location,
						AST_ASSIGN_EQ, std::make_unique<AstNode>(location,
							AST_IDENTIFIER, std::make_unique<AstNode>(location,
								AST_RANGE, AstNode::mkconst_int(location,
									cursor, false))),
							std::move(value)));
				block->children.back()->children[0]->str = memory->str;
				block->children.back()->children[0]->id2ast = memory;
				block->children.back()->children[0]->was_checked = true;
			}

			cursor += increment;
			if ((cursor == finish_addr+increment) || (increment > 0 && cursor > range_max) || (increment < 0 && cursor < range_min))
				break;
		}

		if ((cursor == finish_addr+increment) || (increment > 0 && cursor > range_max) || (increment < 0 && cursor < range_min))
			break;
	}

	if (meminit != nullptr) {
		AstMemInit final_init(meminit);
		final_init.set_data(AstNode::mkconst_bits(location, meminit_bits, false));
		final_init.set_count(AstNode::mkconst_int(location, meminit_size, false));
	}

	return block;
}


YOSYS_NAMESPACE_END
