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
#include "kernel/yosys_common.h"
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
	if (AstWhile::matches(this) || AstRepeat::matches(this))
		return true;
	for (auto& child : children)
		if (child->has_const_only_constructs())
			return true;
	return false;
}

bool AstNode::is_simple_const_expr()
{
	if (AstIdentifier::matches(this))
		return false;
	for (auto& child : children)
		if (!child->is_simple_const_expr())
			return false;
	return true;
}

// attempt to statically evaluate a functions with all-const arguments
std::unique_ptr<AstNode> AstNode::eval_const_function(AstNode *fcall_node, bool must_succeed)
{
	AstFcall fcall(fcall_node);
	std::map<std::string, AstNode*> backup_scope = current_scope;
	std::map<std::string, AstNode::varinfo_t> variables;
	auto block_owned = std::make_unique<AstNode>(location, AST_BLOCK);
	AstBlock block(block_owned.get());
	std::unique_ptr<AstNode> result = nullptr;

	size_t argidx = 0;
	for (auto& child : children)
		block_owned->children.push_back(child->clone());
	block_owned->set_in_param_flag(true);
	std::vector<std::unique_ptr<AstNode>> temporary_nodes;

	auto pop_front = [&]() {
		block_owned->children.erase(block_owned->children.begin());
	};

	while (!block_owned->children.empty())
	{
		auto& stmt = block_owned->children.front();

		if (AstWire::matches(stmt.get()))
		{
			while (stmt->simplify()) { }
			if (!stmt->range_valid) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Can't determine size of variable %s\n%s: ... called from here.\n",
						stmt->str.c_str(), fcall.loc().to_string());
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
				AstRange::matches(stmt->children.back().get());
			if (stmt->is_input && argidx < fcall.num_args()) {
				variable.arg = fcall.arg(argidx++);
			}
			// load the constant arg's value into this variable
			if (variable.arg) {
				if (auto k = AstConstant::cast(variable.arg)) {
					variable.val = k->raw()->bitsAsConst(width);
				} else {
					log_assert(AstRealvalue::matches(variable.arg));
					variable.val = variable.arg->realAsConst(width);
				}
			}
			current_scope[stmt->str] = stmt.get();
			temporary_nodes.push_back(std::move(stmt));
			pop_front();
			continue;
		}

		log_assert(variables.count(str) != 0);

		if (AstLocalparam::cast(stmt.get()))
		{
			while (stmt->simplify()) { }
			current_scope[stmt->str] = stmt.get();
			temporary_nodes.push_back(std::move(stmt));
			pop_front();
			continue;
		}

		if (auto asgn = AstAssignEq::cast(stmt.get()))
		{
			// Pre-simplify: if LHS is an indexed identifier, replace vars in the
			// bit-select range; always replace vars on the RHS expression.
			if (auto lhs_id = AstIdentifier::cast(asgn->lhs().get()))
				if (auto sel = lhs_id->bit_select_raw())
					if (AstRange::matches(sel))
						if (!sel->replace_variables(variables, fcall.raw(), must_succeed))
							goto finished;
			if (!asgn->rhs().get()->replace_variables(variables, fcall.raw(), must_succeed))
				goto finished;
			while (stmt->simplify()) { }

			// simplify may have entirely replaced the assignment
			auto asgn_after = AstAssignEq::cast(stmt.get());
			if (!asgn_after)
				continue;

			auto rhs_const = AstConstant::cast(asgn_after->rhs().get());
			if (!rhs_const) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Non-constant expression in constant function\n%s: ... called from here. X\n",
						fcall.loc().to_string());
			}

			auto lhs_id = AstIdentifier::cast(asgn_after->lhs().get());
			if (!lhs_id) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Unsupported composite left hand side in constant function\n%s: ... called from here.\n",
						fcall.loc().to_string());
			}

			if (!variables.count(lhs_id->str())) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Assignment to non-local variable in constant function\n%s: ... called from here.\n",
						fcall.loc().to_string());
			}

			varinfo_t &v = variables[lhs_id->str()];
			if (!lhs_id->has_bit_select()) {
				v.val = rhs_const->raw()->bitsAsConst(v.val.size());
			} else {
				AstNode *range = lhs_id->bit_select_raw();
				if (!range->range_valid) {
					if (!must_succeed)
						goto finished;
					range->input_error("Non-constant range\n%s: ... called from here.\n", fcall.raw()->loc_string());
				}
				int offset = min(range->range_left, range->range_right);
				int width = std::abs(range->range_left - range->range_right) + 1;
				RTLIL::Const r = rhs_const->raw()->bitsAsConst(v.val.size());
				for (int i = 0; i < width; i++) {
					int index = i + offset - v.offset;
					if (v.range_swapped)
						index = -index;
					v.val.set(index, r.at(i));
				}
			}
			pop_front();
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
			block_owned->children.insert(block_owned->children.begin(), std::move(init));
			continue;
		}

		if (auto while_ = AstWhile::cast(stmt.get()))
		{
			auto cond = while_->cond().get()->clone();
			if (!cond->replace_variables(variables, fcall.raw(), must_succeed))
				goto finished;
			cond->set_in_param_flag(true);
			while (cond->simplify()) { }

			auto cond_const = AstConstant::cast(cond.get());
			if (!cond_const) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Non-constant expression in constant function\n%s: ... called from here.\n",
						fcall.loc().to_string());
			}

			if (cond_const->asBool()) {
				block_owned->children.insert(block_owned->children.begin(), while_->body().get()->clone());
			} else {
				pop_front();
			}
			continue;
		}

		if (auto rep = AstRepeat::cast(stmt.get()))
		{
			auto num = rep->count().get()->clone();
			if (!num->replace_variables(variables, fcall.raw(), must_succeed))
				goto finished;
			num->set_in_param_flag(true);
			while (num->simplify()) { }

			auto num_const = AstConstant::cast(num.get());
			if (!num_const) {
				if (!must_succeed)
					goto finished;
				stmt->input_error("Non-constant expression in constant function\n%s: ... called from here.\n",
						fcall.loc().to_string());
			}

			temporary_nodes.push_back(std::move(stmt));
			pop_front();
			AstRepeat rep_back(temporary_nodes.back().get());
			int n = num_const->raw()->bitsAsConst().as_int();
			for (int i = 0; i < n; i++)
				block_owned->children.insert(block_owned->children.begin(), rep_back.body().get()->clone());
			continue;
		}

		if (auto case_ = AstCase::cast(stmt.get()))
		{
			auto expr = case_->selector()->clone();
			if (!expr->replace_variables(variables, fcall.raw(), must_succeed))
				goto finished;
			expr->set_in_param_flag(true);
			while (expr->simplify()) { }

			AstNode *sel_case = nullptr;
			std::unique_ptr<AstNode> sel_case_copy = nullptr;
			for (auto it = case_->conditions_begin(); it != case_->conditions_end(); ++it)
			{
				AstAnyCond cond_view(it->get());
				bool found_match = false;

				if (cond_view.is_default()) {
					sel_case = cond_view.body();
					continue;
				}

				for (size_t j = 0; j < cond_view.num_labels() && !found_match; j++)
				{
					auto cond = cond_view.label(j)->clone();
					if (!cond->replace_variables(variables, fcall.raw(), must_succeed))
						goto finished;

					cond = std::make_unique<AstNode>(location, AST_EQ, expr->clone(), std::move(cond));
					cond->set_in_param_flag(true);
					while (cond->simplify()) { }

					auto cond_const = AstConstant::cast(cond.get());
					if (!cond_const) {
						if (!must_succeed)
							goto finished;
						stmt->input_error("Non-constant expression in constant function\n%s: ... called from here.\n",
								fcall.loc().to_string());
					}

					found_match = cond_const->asBool();
				}

				if (found_match) {
					sel_case = cond_view.body();
					break;
				}
			}
			if (sel_case)
				sel_case_copy = sel_case->clone();

			pop_front();
			if (sel_case_copy)
				block_owned->children.insert(block_owned->children.begin(), std::move(sel_case_copy));
			continue;
		}

		if (auto inner = AstBlock::cast(stmt.get()))
		{
			if (!inner->str().empty())
				inner->raw()->expand_genblock(inner->str() + ".");
			AstNode *stmt_leaky = inner->raw();
			temporary_nodes.push_back(std::move(stmt));
			pop_front();
			block_owned->children.reserve(block_owned->children.size() + stmt_leaky->children.size());
			block_owned->children.insert(block_owned->children.begin(),
				std::make_move_iterator(stmt_leaky->children.begin()),
				std::make_move_iterator(stmt_leaky->children.end()));
			stmt_leaky->children.clear();
			block_owned->fixup_hierarchy_flags();
			continue;
		}

		if (!must_succeed)
			goto finished;
		stmt->input_error("Unsupported language construct in constant function\n%s: ... called from here.\n",
				fcall.loc().to_string());
		log_abort();
	}

	result = AstNode::mkconst_bits(location, variables.at(str).val.to_bits(), variables.at(str).is_signed);

	finished:
	current_scope = backup_scope;
	return result;
}

YOSYS_NAMESPACE_END

