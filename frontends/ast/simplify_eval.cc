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

YOSYS_NAMESPACE_BEGIN

using namespace AST;
using namespace AST_INTERNAL;


bool AstNode::has_const_only_constructs()
{
	if (type == AST_WHILE || type == AST_REPEAT)
		return true;
	for (auto& child : children)
		if (child->has_const_only_constructs())
			return true;
	return false;
}

bool AstNode::is_simple_const_expr()
{
	if (type == AST_IDENTIFIER)
		return false;
	for (auto& child : children)
		if (!child->is_simple_const_expr())
			return false;
	return true;
}

// attempt to statically evaluate a functions with all-const arguments
std::unique_ptr<AstNode> AstNode::eval_const_function(AstNode *fcall, bool must_succeed)
{
	std::map<std::string, AstNode*> backup_scope = current_scope;
	std::map<std::string, AstNode::varinfo_t> variables;
	auto block = std::make_unique<AstNode>(location, AST_BLOCK);
	std::unique_ptr<AstNode> result = nullptr;

	size_t argidx = 0;
	for (auto& child : children)
	{
		block->children.push_back(child->clone());
	}
	block->set_in_param_flag(true);
	std::vector<std::unique_ptr<AstNode>> temporary_nodes;

	while (!block->children.empty())
	{
		auto& stmt = block->children.front();

#if 0
		log("-----------------------------------\n");
		for (auto &it : variables)
			log("%20s %40s\n", it.first, log_signal(it.second.val));
		stmt->dumpAst(nullptr, "stmt> ");
#endif
		if (stmt->type == AST_WIRE)
		{
			while (stmt->simplify(true, 1, -1, false)) { }
			if (!stmt->range_valid) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Can't determine size of variable %s\n%s: ... called from here.\n",
						stmt->str.c_str(), fcall->loc_string().c_str());
			}
			AstNode::varinfo_t &variable = variables[stmt->str];
			int width = abs(stmt->range_left - stmt->range_right) + 1;
			// if this variable has already been declared as an input, check the
			// sizes match if it already had an explicit size
			if (variable.arg && variable.explicitly_sized && variable.val.size() != width) {
				input_error("Incompatible re-declaration of constant function wire %s.\n", stmt->str);
			}
			variable.val = RTLIL::Const(RTLIL::State::Sx, width);
			variable.offset = stmt->range_swapped ? stmt->range_left : stmt->range_right;
			variable.range_swapped = stmt->range_swapped;
			variable.is_signed = stmt->is_signed;
			variable.explicitly_sized = stmt->children.size() &&
				stmt->children.back()->type == AST_RANGE;
			// identify the argument corresponding to this wire, if applicable
			if (stmt->is_input && argidx < fcall->children.size()) {
				variable.arg = fcall->children.at(argidx++).get();
			}
			// load the constant arg's value into this variable
			if (variable.arg) {
				if (variable.arg->type == AST_CONSTANT) {
					variable.val = variable.arg->bitsAsConst(width);
				} else {
					log_assert(variable.arg->type == AST_REALVALUE);
					variable.val = variable.arg->realAsConst(width);
				}
			}
			current_scope[stmt->str] = stmt.get();
			temporary_nodes.push_back(std::move(stmt));

			block->children.erase(block->children.begin());
			continue;
		}

		log_assert(variables.count(str) != 0);

		if (stmt->type == AST_LOCALPARAM)
		{
			while (stmt->simplify(true, 1, -1, false)) { }

			current_scope[stmt->str] = stmt.get();
			temporary_nodes.push_back(std::move(stmt));

			block->children.erase(block->children.begin());
			continue;
		}

		if (auto asgn = AstAssignEq::cast(stmt.get()))
		{
			AstNode *asgn_lhs = asgn->lhs().get();
			AstNode *asgn_rhs = asgn->rhs().get();
			if (asgn_lhs->type == AST_IDENTIFIER && asgn_lhs->children.size() != 0 &&
					asgn_lhs->children.at(0)->type == AST_RANGE)
				if (!asgn_lhs->children.at(0)->replace_variables(variables, fcall, must_succeed))
					goto finished;
			if (!asgn_rhs->replace_variables(variables, fcall, must_succeed))
				goto finished;
			while (stmt->simplify(true, 1, -1, false)) { }

			if (stmt->type != AST_ASSIGN_EQ)
				continue;

			// Re-read pointers: simplify may have rewritten children.
			asgn_lhs = asgn->lhs().get();
			asgn_rhs = asgn->rhs().get();

			if (asgn_rhs->type != AST_CONSTANT) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Non-constant expression in constant function\n%s: ... called from here. X\n",
						fcall->loc_string().c_str());
			}

			if (asgn_lhs->type != AST_IDENTIFIER) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Unsupported composite left hand side in constant function\n%s: ... called from here.\n",
						fcall->loc_string().c_str());
			}

			if (!variables.count(asgn_lhs->str)) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Assignment to non-local variable in constant function\n%s: ... called from here.\n",
						fcall->loc_string().c_str());
			}

			if (asgn_lhs->children.empty()) {
				variables[asgn_lhs->str].val = asgn_rhs->bitsAsConst(variables[asgn_lhs->str].val.size());
			} else {
				AstNode *range = asgn_lhs->children.at(0).get();
				if (!range->range_valid) {
					if (!must_succeed)
						goto finished;
					range->input_error("Non-constant range\n%s: ... called from here.\n", fcall->loc_string());
				}
				int offset = min(range->range_left, range->range_right);
				int width = std::abs(range->range_left - range->range_right) + 1;
				varinfo_t &v = variables[asgn_lhs->str];
				RTLIL::Const r = asgn_rhs->bitsAsConst(v.val.size());
				for (int i = 0; i < width; i++) {
					int index = i + offset - v.offset;
					if (v.range_swapped)
						index = -index;
					v.val.set(index, r.at(i));
				}
			}

			block->children.erase(block->children.begin());
			continue;
		}

		if (auto for_ = AstFor::cast(stmt.get()))
		{
			// AST_FOR: [init, cond, step, body] -> AST_WHILE: [cond, body (+step appended)]
			auto init = for_->init().take();
			auto cond = for_->cond().take();
			auto step = for_->step().take();
			auto body = for_->body().take();
			body->children.push_back(std::move(step));
			reshape_as<AST_WHILE>(stmt.get(), std::move(cond), std::move(body));
			block->children.insert(block->children.begin(), std::move(init));
			continue;
		}

		if (auto while_ = AstWhile::cast(stmt.get()))
		{
			auto cond = while_->cond().get()->clone();
			if (!cond->replace_variables(variables, fcall, must_succeed))
				goto finished;
			cond->set_in_param_flag(true);
			while (cond->simplify(true, 1, -1, false)) { }

			if (cond->type != AST_CONSTANT) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Non-constant expression in constant function\n%s: ... called from here.\n",
						fcall->loc_string().c_str());
			}

			if (cond->asBool()) {
				block->children.insert(block->children.begin(), while_->body().get()->clone());
			} else {
				block->children.erase(block->children.begin());
			}
			continue;
		}

		if (auto rep = AstRepeat::cast(stmt.get()))
		{
			auto num = rep->count().get()->clone();
			if (!num->replace_variables(variables, fcall, must_succeed))
				goto finished;
			num->set_in_param_flag(true);
			while (num->simplify(true, 1, -1, false)) { }

			if (num->type != AST_CONSTANT) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Non-constant expression in constant function\n%s: ... called from here.\n",
						fcall->loc_string().c_str());
			}

			temporary_nodes.push_back(std::move(stmt));
			block->children.erase(block->children.begin());
			AstRepeat rep_back(temporary_nodes.back().get());
			for (int i = 0; i < num->bitsAsConst().as_int(); i++)
				block->children.insert(block->children.begin(), rep_back.body().get()->clone());

			continue;
		}

		if (auto case_ = AstCase::cast(stmt.get()))
		{
			auto expr = case_->selector()->clone();
			if (!expr->replace_variables(variables, fcall, must_succeed))
				goto finished;
			expr->set_in_param_flag(true);
			while (expr->simplify(true, 1, -1, false)) { }

			AstNode *sel_case = nullptr;
			std::unique_ptr<AstNode> sel_case_copy = nullptr;
			for (auto it = case_->conditions_begin(); it != case_->conditions_end(); ++it)
			{
				auto &child = *it;
				bool found_match = false;
				log_assert(child->type == AST_COND || child->type == AST_CONDX || child->type == AST_CONDZ);

				if (child->children.front()->type == AST_DEFAULT) {
					sel_case = child->children.back().get();
					continue;
				}

				for (size_t j = 0; j+1 < child->children.size() && !found_match; j++)
				{
					auto cond = child->children.at(j)->clone();
					if (!cond->replace_variables(variables, fcall, must_succeed))
						goto finished;

					cond = std::make_unique<AstNode>(location, AST_EQ, expr->clone(), std::move(cond));
					cond->set_in_param_flag(true);
					while (cond->simplify(true, 1, -1, false)) { }

					if (cond->type != AST_CONSTANT) {
						if (!must_succeed)
							goto finished;
						stmt->input_error("Non-constant expression in constant function\n%s: ... called from here.\n",
								fcall->loc_string().c_str());
					}

					found_match = cond->asBool();
				}

				if (found_match) {
					sel_case = child->children.back().get();
					break;
				}
			}
			if (sel_case)
				sel_case_copy = sel_case->clone();

			block->children.erase(block->children.begin());
			if (sel_case_copy)
				block->children.insert(block->children.begin(), std::move(sel_case_copy));
			continue;
		}

		if (stmt->type == AST_BLOCK)
		{
			if (!stmt->str.empty())
				stmt->expand_genblock(stmt->str + ".");
			auto* stmt_leaky = stmt.get();
			temporary_nodes.push_back(std::move(stmt));
			block->children.erase(block->children.begin());
			block->children.reserve(block->children.size() + stmt_leaky->children.size());
			block->children.insert(block->children.begin(),
				std::make_move_iterator(stmt_leaky->children.begin()),
				std::make_move_iterator(stmt_leaky->children.end()));
			stmt_leaky->children.clear();
			block->fixup_hierarchy_flags();
			continue;
		}

		// log("C\n");
		if (!must_succeed)
			goto finished;
		stmt->input_error("Unsupported language construct in constant function\n%s: ... called from here.\n",
				fcall->loc_string().c_str());
		log_abort();
	}

	result = AstNode::mkconst_bits(location, variables.at(str).val.to_bits(), variables.at(str).is_signed);

finished:
	current_scope = backup_scope;
	return result;
}

YOSYS_NAMESPACE_END

