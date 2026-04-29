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
#include "kernel/utils.h"
#include "kernel/binding.h"
#include "libs/sha1/sha1.h"
#include "ast.h"
#include "ast_binding.h"
#include "ast_typed.h"
#include "genrtlil_internal.h"

#include <sstream>
#include <stdarg.h>
#include <algorithm>

YOSYS_NAMESPACE_BEGIN

using namespace AST;
using namespace AST_INTERNAL;

// Copy a node's attribute table onto an RTLIL AttrObject, asserting each attribute
// value is a constant. Used wherever AST -> RTLIL conversion forwards attributes.
void AST_INTERNAL::copy_const_attributes(RTLIL::AttrObject *target, AstNode *that)
{
	for (auto &attr : that->attributes) {
		if (!AstConstant::matches(attr.second.get()))
			that->input_error("Attribute `%s' with non-constant value!\n", attr.first);
		target->attributes[attr.first] = attr.second->asAttrConst();
	}
}

// helper function for creating RTLIL code for unary operations
static RTLIL::SigSpec uniop2rtlil(AstNode *that, IdString type, int result_width, const RTLIL::SigSpec &arg, bool gen_attributes = true)
{
	IdString name = stringf("%s$%s:%d$%d", type, RTLIL::encode_filename(*that->location.begin.filename), that->location.begin.line, autoidx++);
	RTLIL::Cell *cell = current_module->addCell(name, type);
	set_src_attr(cell, that);

	RTLIL::Wire *wire = current_module->addWire(cell->name.str() + "_Y", result_width);
	set_src_attr(wire, that);
	wire->is_signed = that->is_signed;

	if (gen_attributes)
		copy_const_attributes(cell, that);

	cell->parameters[ID::A_SIGNED] = RTLIL::Const(that->children[0]->is_signed);
	cell->parameters[ID::A_WIDTH] = RTLIL::Const(arg.size());
	cell->setPort(ID::A, arg);

	cell->parameters[ID::Y_WIDTH] = result_width;
	cell->setPort(ID::Y, wire);
	return wire;
}

// helper function for extending bit width (preferred over SigSpec::extend() because of correct undef propagation in ConstEval)
static void widthExtend(AstNode *that, RTLIL::SigSpec &sig, int width, bool is_signed)
{
	if (width <= sig.size()) {
		sig.extend_u0(width, is_signed);
		return;
	}

	IdString name = stringf("$extend$%s:%d$%d", RTLIL::encode_filename(*that->location.begin.filename), that->location.begin.line, autoidx++);
	RTLIL::Cell *cell = current_module->addCell(name, ID($pos));
	set_src_attr(cell, that);

	RTLIL::Wire *wire = current_module->addWire(cell->name.str() + "_Y", width);
	set_src_attr(wire, that);
	wire->is_signed = that->is_signed;

	if (that != nullptr)
		copy_const_attributes(cell, that);

	cell->parameters[ID::A_SIGNED] = RTLIL::Const(is_signed);
	cell->parameters[ID::A_WIDTH] = RTLIL::Const(sig.size());
	cell->setPort(ID::A, sig);

	cell->parameters[ID::Y_WIDTH] = width;
	cell->setPort(ID::Y, wire);
	sig = wire;
}

// helper function for creating RTLIL code for binary operations
static RTLIL::SigSpec binop2rtlil(AstNode *that, IdString type, int result_width, const RTLIL::SigSpec &left, const RTLIL::SigSpec &right)
{
	IdString name = stringf("%s$%s:%d$%d", type, RTLIL::encode_filename(*that->location.begin.filename), that->location.begin.line, autoidx++);
	RTLIL::Cell *cell = current_module->addCell(name, type);
	set_src_attr(cell, that);

	RTLIL::Wire *wire = current_module->addWire(cell->name.str() + "_Y", result_width);
	set_src_attr(wire, that);
	wire->is_signed = that->is_signed;

	copy_const_attributes(cell, that);

	cell->parameters[ID::A_SIGNED] = RTLIL::Const(that->children[0]->is_signed);
	cell->parameters[ID::B_SIGNED] = RTLIL::Const(that->children[1]->is_signed);

	cell->parameters[ID::A_WIDTH] = RTLIL::Const(left.size());
	cell->parameters[ID::B_WIDTH] = RTLIL::Const(right.size());

	cell->setPort(ID::A, left);
	cell->setPort(ID::B, right);

	cell->parameters[ID::Y_WIDTH] = result_width;
	cell->setPort(ID::Y, wire);
	return wire;
}

// helper function for creating RTLIL code for multiplexers (lowers AST_TERNARY).
static RTLIL::SigSpec mux2rtlil(AstTernary that, const RTLIL::SigSpec &cond, const RTLIL::SigSpec &left, const RTLIL::SigSpec &right)
{
	log_assert(cond.size() == 1);

	std::stringstream sstr;
	sstr << "$ternary$" << RTLIL::encode_filename(*that.loc().begin.filename) << ":" << that.loc().begin.line << "$" << (autoidx++);

	RTLIL::Cell *cell = current_module->addCell(sstr.str(), ID($mux));
	set_src_attr(cell, that.raw());

	RTLIL::Wire *wire = current_module->addWire(cell->name.str() + "_Y", left.size());
	set_src_attr(wire, that.raw());
	wire->is_signed = that.raw()->is_signed;

	copy_const_attributes(cell, that.raw());

	cell->parameters[ID::WIDTH] = RTLIL::Const(left.size());

	cell->setPort(ID::A, right);
	cell->setPort(ID::B, left);
	cell->setPort(ID::S, cond);
	cell->setPort(ID::Y, wire);

	return wire;
}

void AST_INTERNAL::check_unique_id(RTLIL::Module *module, RTLIL::IdString id,
		const AstNode *node, const char *to_add_kind)
{
	auto already_exists = [&](const RTLIL::AttrObject *existing, const char *existing_kind) {
		std::string src = existing->get_string_attribute(ID::src);
		std::string location_str = "earlier";
		if (!src.empty())
			location_str = "at " + src;
		node->input_error("Cannot add %s `%s' because a %s with the same name was already created %s!\n",
						  to_add_kind, id.c_str(), existing_kind, location_str.c_str());
	};

	if (const RTLIL::Wire *wire = module->wire(id))
		already_exists(wire, "signal");
	if (const RTLIL::Cell *cell = module->cell(id))
		already_exists(cell, "cell");
	if (module->processes.count(id))
		already_exists(module->processes.at(id), "process");
	if (module->memories.count(id))
		already_exists(module->memories.at(id), "memory");
}

// Generate RTLIL for a bind construct
//
// The AST node will have one or more AST_IDENTIFIER children, which were added
// by bind_target_instance in the parser. After these, it will have one or more
// cells, as parsed by single_cell. These have type AST_CELL.
//
// If there is more than one AST_IDENTIFIER, the first one should be considered
// a module identifier. If there is only one AST_IDENTIFIER, we can't tell at
// this point whether it's a module/interface name or the name of an instance
// because the correct interpretation depends on what's visible at elaboration
// time. For now, we just treat it as a target instance with unknown type, and
// we'll deal with the corner case in the hierarchy pass.
//
// To simplify downstream code, RTLIL::Binding only has a single target and
// single bound instance. If we see the syntax that allows more than one of
// either, we split it into multiple Binding objects.
std::vector<RTLIL::Binding *> AstNode::genBindings() const
{
	// Partition children into identifiers and cells
	int num_ids = 0;
	for (int i = 0; i < GetSize(children); ++i) {
		if (!AstIdentifier::matches(children[i].get())) {
			log_assert(i > 0);
			num_ids = i;
			break;
		}
	}

	// We should have found at least one child that's not an identifier
	log_assert(num_ids > 0);

	// Make sense of the identifiers, extracting a possible type name and a
	// list of hierarchical IDs. We represent an unknown type with an empty
	// string.
	RTLIL::IdString tgt_type;
	int first_tgt_inst = 0;
	if (num_ids > 1) {
		tgt_type = children[0]->str;
		first_tgt_inst = 1;
	}

	std::vector<RTLIL::Binding *> ret;

	// At this point, we know that children with index >= first_tgt_inst and
	// index < num_ids are (hierarchical?) names of target instances. Make a
	// binding object for each of them, and fill in the generated instance
	// cells each time.
	for (int i = first_tgt_inst; i < num_ids; ++i) {
		const AstNode &tgt_child = *children[i];

		for (int j = num_ids; j < GetSize(children); ++j) {
			const AstNode &cell_child = *children[j];

			log_assert(AstCell::matches(&cell_child));

			ret.push_back(new AST::Binding(tgt_type, tgt_child.str,
			                               cell_child));
		}
	}

	return ret;
}

// detect sign and width of an expression
void AstNode::detectSignWidthWorker(int &width_hint, bool &sign_hint, bool *found_real)
{
	std::string type_name;
	bool sub_sign_hint = true;
	int sub_width_hint = -1;
	int this_width = 0;
	AstNode *range = nullptr;
	AstNode *id_ast = nullptr;

	bool local_found_real = false;
	if (found_real == nullptr)
		found_real = &local_found_real;

	switch (type)
	{
	case AST_NONE:
		// unallocated enum, ignore
		break;
	case AST_CONSTANT:
		width_hint = max(width_hint, GetSize(bits));
		if (!is_signed)
			sign_hint = false;
		break;

	case AST_REALVALUE:
		*found_real = true;
		width_hint = max(width_hint, 32);
		break;

	case AST_IDENTIFIER:
		id_ast = id2ast;
		if (!id_ast) {
			if (current_scope.count(str))
				id_ast = current_scope[str];
			else {
				std::string alt = try_pop_module_prefix();
				if (current_scope.count(alt))
					id_ast = current_scope[alt];
			}
		}
		if (!id_ast)
			input_error("Failed to resolve identifier %s for width detection!\n", str);
		if (AstAnyParamLike::matches(id_ast)) {
			if (id_ast->children.size() > 1 && id_ast->children[1]->range_valid) {
				this_width = id_ast->children[1]->range_left - id_ast->children[1]->range_right + 1;
			} else {
				if (!AstConstant::matches(id_ast->children[0].get()))
					while (id_ast->simplify()) { }
				if (AstConstant::matches(id_ast->children[0].get()))
					this_width = id_ast->children[0]->bits.size();
				else
					input_error("Failed to detect width for parameter %s!\n", str);
			}
			if (children.size() != 0)
				range = children[0].get();
		} else if (ChildConstraint<AST_WIRE, AST_AUTOWIRE>::accepts(id_ast)) {
			if (!id_ast->range_valid) {
				if (AstAutowire::matches(id_ast))
					this_width = 1;
				else {
					// current_ast_mod->dumpAst(nullptr, "mod> ");
					// log("---\n");
					// id_ast->dumpAst(nullptr, "decl> ");
					// dumpAst(nullptr, "ref> ");
					input_error("Failed to detect width of signal access `%s'!\n", str);
				}
			} else {
				this_width = id_ast->range_left - id_ast->range_right + 1;
				if (children.size() != 0)
					range = children[0].get();
			}
		} else if (AstGenvar::matches(id_ast)) {
			this_width = 32;
		} else if (AstMemory::matches(id_ast)) {
			if (!id_ast->children[0]->range_valid)
				input_error("Failed to detect width of memory access `%s'!\n", str);
			this_width = id_ast->children[0]->range_left - id_ast->children[0]->range_right + 1;
			if (children.size() > 1)
				range = children[1].get();
		} else if (StructMemberOrAggregate::accepts(id_ast)) {
			auto tmp_range = make_index_range(id_ast);
			this_width = tmp_range->range_left - tmp_range->range_right + 1;
		} else
			input_error("Failed to detect width for identifier %s!\n", str);
		if (range) {
			if (range->children.size() == 1)
				this_width = 1;
			else if (!range->range_valid) {
				auto left_at_zero_ast = children[0]->children[0]->clone_at_zero();
				auto right_at_zero_ast = children[0]->children.size() >= 2 ? children[0]->children[1]->clone_at_zero() : left_at_zero_ast->clone();
				while (left_at_zero_ast->simplify()) { }
				while (right_at_zero_ast->simplify()) { }
				if (!AstConstant::matches(left_at_zero_ast.get()) || !AstConstant::matches(right_at_zero_ast.get()))
					input_error("Unsupported expression on dynamic range select on signal `%s'!\n", str);
				this_width = abs(int(left_at_zero_ast->integer - right_at_zero_ast->integer)) + 1;
			} else
				this_width = range->range_left - range->range_right + 1;
			sign_hint = false;
		}
		width_hint = max(width_hint, this_width);
		if (!id_ast->is_signed)
			sign_hint = false;
		break;

	case AST_TO_BITS:
		while (children[0]->simplify() == true) { }
		if (!AstConstant::matches(children[0].get()))
			input_error("Left operand of tobits expression is not constant!\n");
		children[1]->detectSignWidthWorker(sub_width_hint, sign_hint);
		width_hint = max(width_hint, children[0]->bitsAsConst().as_int());
		break;

	case AST_TO_SIGNED:
		children.at(0)->detectSignWidthWorker(width_hint, sub_sign_hint);
		break;

	case AST_TO_UNSIGNED:
		children.at(0)->detectSignWidthWorker(width_hint, sub_sign_hint);
		sign_hint = false;
		break;

	case AST_SELFSZ:
		sub_width_hint = 0;
		children.at(0)->detectSignWidthWorker(sub_width_hint, sign_hint);
		break;

	case AST_CAST_SIZE:
		while (children.at(0)->simplify()) { }
		if (!AstConstant::matches(children.at(0).get()))
			input_error("Static cast with non constant expression!\n");
		children.at(1)->detectSignWidthWorker(width_hint, sign_hint);
		this_width = children.at(0)->bitsAsConst().as_int();
		width_hint = max(width_hint, this_width);
		if (width_hint <= 0)
			input_error("Static cast with zero or negative size!\n");
		break;

	case AST_CONCAT:
		for (auto& child : children) {
			sub_width_hint = 0;
			sub_sign_hint = true;
			child->detectSignWidthWorker(sub_width_hint, sub_sign_hint);
			this_width += sub_width_hint;
		}
		width_hint = max(width_hint, this_width);
		sign_hint = false;
		break;

	case AST_REPLICATE:
		while (children[0]->simplify() == true) { }
		if (!AstConstant::matches(children[0].get()))
			input_error("Left operand of replicate expression is not constant!\n");
		children[1]->detectSignWidthWorker(sub_width_hint, sub_sign_hint);
		width_hint = max(width_hint, children[0]->bitsAsConst().as_int() * sub_width_hint);
		sign_hint = false;
		break;

	case AST_NEG:
	case AST_BIT_NOT:
	case AST_POS:
		children[0]->detectSignWidthWorker(width_hint, sign_hint, found_real);
		break;

	case AST_BIT_AND:
	case AST_BIT_OR:
	case AST_BIT_XOR:
	case AST_BIT_XNOR:
		for (auto& child : children)
			child->detectSignWidthWorker(width_hint, sign_hint, found_real);
		break;

	case AST_REDUCE_AND:
	case AST_REDUCE_OR:
	case AST_REDUCE_XOR:
	case AST_REDUCE_XNOR:
	case AST_REDUCE_BOOL:
		width_hint = max(width_hint, 1);
		sign_hint = false;
		break;

	case AST_SHIFT_LEFT:
	case AST_SHIFT_RIGHT:
	case AST_SHIFT_SLEFT:
	case AST_SHIFT_SRIGHT:
	case AST_SHIFTX:
	case AST_SHIFT:
	case AST_POW:
		children[0]->detectSignWidthWorker(width_hint, sign_hint, found_real);
		break;

	case AST_LT:
	case AST_LE:
	case AST_EQ:
	case AST_NE:
	case AST_EQX:
	case AST_NEX:
	case AST_GE:
	case AST_GT:
		width_hint = max(width_hint, 1);
		sign_hint = false;
		break;

	case AST_ADD:
	case AST_SUB:
	case AST_MUL:
	case AST_DIV:
	case AST_MOD:
		for (auto& child : children)
			child->detectSignWidthWorker(width_hint, sign_hint, found_real);
		break;

	case AST_LOGIC_AND:
	case AST_LOGIC_OR:
	case AST_LOGIC_NOT:
		width_hint = max(width_hint, 1);
		sign_hint = false;
		break;

	case AST_TERNARY:
		children.at(1)->detectSignWidthWorker(width_hint, sign_hint, found_real);
		children.at(2)->detectSignWidthWorker(width_hint, sign_hint, found_real);
		break;

	case AST_MEMRD:
		if (!id2ast->is_signed)
			sign_hint = false;
		if (!id2ast->children[0]->range_valid)
			input_error("Failed to detect width of memory access `%s'!\n", str);
		this_width = id2ast->children[0]->range_left - id2ast->children[0]->range_right + 1;
		width_hint = max(width_hint, this_width);
		break;

	case AST_CASE:
	{
		// This detects the _overall_ sign and width to be used for comparing
		// the case expression with the case item expressions. The case
		// expression and case item expressions are extended to the maximum
		// width among them, and are only interpreted as signed if all of them
		// are signed.
		width_hint = -1;
		sign_hint = true;
		auto visit_case_expr = [&width_hint, &sign_hint] (AstNode *node) {
			int sub_width_hint = -1;
			bool sub_sign_hint = true;
			node->detectSignWidth(sub_width_hint, sub_sign_hint);
			width_hint = max(width_hint, sub_width_hint);
			sign_hint &= sub_sign_hint;
		};
		visit_case_expr(children[0].get());
		for (size_t i = 1; i < children.size(); i++) {
			AstNode *child = children[i].get();
			for (auto& v : child->children) {
				if (!ChildConstraint<AST_DEFAULT, AST_BLOCK>::accepts(v.get()))
					visit_case_expr(v.get());
			}
		}
		break;
	}

	case AST_PREFIX:
		// Prefix nodes always resolve to identifiers in generate loops, so we
		// can simply perform the resolution to determine the sign and width.
		simplify(true, 1, -1, false);
		log_assert(AstIdentifier::matches(this));
		detectSignWidthWorker(width_hint, sign_hint, found_real);
		break;

	case AST_FCALL:
		if (str == "\\$anyconst" || str == "\\$anyseq" || str == "\\$allconst" || str == "\\$allseq") {
			if (GetSize(children) == 1) {
				while (children[0]->simplify() == true) { }
				if (!AstConstant::matches(children[0].get()))
					input_error("System function %s called with non-const argument!\n",
							RTLIL::unescape_id(str));
				width_hint = max(width_hint, int(children[0]->asInt(true)));
			}
			break;
		}
		if (str == "\\$past") {
			if (GetSize(children) > 0) {
				sub_width_hint = 0;
				sub_sign_hint = true;
				children.at(0)->detectSignWidthWorker(sub_width_hint, sub_sign_hint);
				width_hint = max(width_hint, sub_width_hint);
				sign_hint &= sub_sign_hint;
			}
			break;
		}
		if (str == "\\$size" || str == "\\$bits" || str == "\\$high" || str == "\\$low" || str == "\\$left" || str == "\\$right") {
			width_hint = max(width_hint, 32);
			break;
		}
		if (current_scope.count(str))
		{
			// This width detection is needed for function calls which are
			// unelaborated, which currently applies to calls to functions
			// reached via unevaluated ternary branches or used in case or case
			// item expressions.
			const AstNode *func = current_scope.at(str);
			if (!AstFunction::matches(func))
				input_error("Function call to %s resolved to something that isn't a function!\n", RTLIL::unescape_id(str));
			const AstNode *wire = nullptr;
			for (const auto& child : func->children)
				if (child->str == func->str) {
					wire = child.get();
					break;
				}
			log_assert(wire && AstWire::matches(wire));
			sign_hint &= wire->is_signed;
			int result_width = 1;
			if (!wire->children.empty())
			{
				log_assert(wire->children.size() == 1);
				const AstNode *range = wire->children.at(0).get();
				log_assert(AstRange::matches(range) && range->children.size() == 2);
				auto left = range->children.at(0)->clone();
				auto right = range->children.at(1)->clone();
				left->set_in_param_flag(true);
				right->set_in_param_flag(true);
				while (left->simplify()) { }
				while (right->simplify()) { }
				if (!AstConstant::matches(left.get()) || !AstConstant::matches(right.get()))
					input_error("Function %s has non-constant width!",
							RTLIL::unescape_id(str));
				result_width = abs(int(left->asInt(true) - right->asInt(true)));
			}
			width_hint = max(width_hint, result_width);
			break;
		}
		YS_FALLTHROUGH

	// everything should have been handled above -> print error if not.
	default:
		AstNode *current_scope_ast = current_ast_mod == nullptr ? current_ast : current_ast_mod;
		for (auto f : log_files)
			current_scope_ast->dumpAst(f, "verilog-ast> ");
		input_error("Don't know how to detect sign and width for %s node!\n", type2str(type));

	}

	if (*found_real)
		sign_hint = true;
}

// detect sign and width of an expression
void AstNode::detectSignWidth(int &width_hint, bool &sign_hint, bool *found_real)
{
	width_hint = -1;
	sign_hint = true;
	if (found_real)
		*found_real = false;
	detectSignWidthWorker(width_hint, sign_hint, found_real);

	constexpr int kWidthLimit = 1 << 24;
	if (width_hint >= kWidthLimit)
		input_error("Expression width %d exceeds implementation limit of %d!\n",
					width_hint, kWidthLimit);
}

// =====================================================================
// Per-view genRTLIL implementations. Each of these handles one case (or
// a small group of related cases) from the AstNode::genRTLIL dispatcher.
// =====================================================================

namespace AST {

RTLIL::SigSpec AstInterfacePort::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	RTLIL::IdString id = that->str;
	AST_INTERNAL::check_unique_id(current_module, id, that, "interface port");
	RTLIL::Wire *wire = current_module->addWire(id, 1);
	set_src_attr(wire, that);
	wire->start_offset = 0;
	wire->port_id = that->port_id;
	wire->port_input = true;
	wire->port_output = true;
	wire->set_bool_attribute(ID::is_interface);
	if (that->children.size() > 0) {
		for(size_t i = 0; i < that->children.size(); i++) {
			if (AstInterfacePortType::matches(that->children[i].get())) {
				std::pair<std::string,std::string> res = AST::split_modport_from_type(that->children[i]->str);
				wire->attributes[ID::interface_type] = res.first;
				if (res.second != "")
					wire->attributes[ID::interface_modport] = res.second;
				break;
			}
		}
	}
	wire->upto = 0;
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstAnyParamLike::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	if (that->type == AST_PARAMETER) {
		current_module->avail_parameters(that->str);
		if (GetSize(that->children) >= 1 && AstConstant::matches(that->children[0].get())) {
			current_module->parameter_default_values[that->str] = that->children[0]->asParaConst();
		}
	}
	if (flag_pwires) {
		if (GetSize(that->children) < 1 || !AstConstant::matches(that->children[0].get()))
			that->input_error("Parameter `%s' with non-constant value!\n", that->str.c_str());

		RTLIL::Const val = that->children[0]->bitsAsConst();
		RTLIL::IdString id = that->str;
		AST_INTERNAL::check_unique_id(current_module, id, that, "pwire");
		RTLIL::Wire *wire = current_module->addWire(id, GetSize(val));
		current_module->connect(wire, val);
		wire->is_signed = that->children[0]->is_signed;

		set_src_attr(wire, that);
		wire->attributes[that->type == AST_PARAMETER ? ID::parameter : ID::localparam] = 1;

		AST_INTERNAL::copy_const_attributes(wire, that);
	}
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstWire::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	if (!that->range_valid)
		that->input_error("Signal `%s' with non-constant width!\n", that->str.c_str());

	if (!(that->range_left + 1 >= that->range_right))
		that->input_error("Signal `%s' with invalid width range %d!\n", that->str.c_str(), that->range_left - that->range_right + 1);

	RTLIL::IdString id = that->str;
	AST_INTERNAL::check_unique_id(current_module, id, that, "signal");
	RTLIL::Wire *wire = current_module->addWire(id, that->range_left - that->range_right + 1);
	set_src_attr(wire, that);
	wire->start_offset = that->range_right;
	wire->port_id = that->port_id;
	wire->port_input = that->is_input;
	wire->port_output = that->is_output;
	wire->upto = that->range_swapped;

	wire->is_signed = that->is_signed;

	AST_INTERNAL::copy_const_attributes(wire, that);

	if (that->is_wand) wire->set_bool_attribute(ID::wand);
	if (that->is_wor)  wire->set_bool_attribute(ID::wor);
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstMemory::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	log_assert(that->children.size() >= 2);
	log_assert(AstRange::matches(that->children[0].get()));
	log_assert(AstRange::matches(that->children[1].get()));

	if (!that->children[0]->range_valid || !that->children[1]->range_valid)
		that->input_error("Memory `%s' with non-constant width or size!\n", that->str.c_str());

	RTLIL::Memory *memory = new RTLIL::Memory;
	set_src_attr(memory, that);
	memory->name = that->str;
	memory->width = that->children[0]->range_left - that->children[0]->range_right + 1;
	if (that->children[1]->range_right < that->children[1]->range_left) {
		memory->start_offset = that->children[1]->range_right;
		memory->size = that->children[1]->range_left - that->children[1]->range_right + 1;
	} else {
		memory->start_offset = that->children[1]->range_left;
		memory->size = that->children[1]->range_right - that->children[1]->range_left + 1;
	}
	AST_INTERNAL::check_unique_id(current_module, memory->name, that, "memory");
	current_module->memories[memory->name] = memory;

	AST_INTERNAL::copy_const_attributes(memory, that);
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstConstant::genRTLIL(int width_hint, bool sign_hint) {
	AstNode *that = node;
	if (width_hint < 0)
		that->detectSignWidth(width_hint, sign_hint);
	that->is_signed = sign_hint;

	if (that->is_unsized) {
		return RTLIL::SigSpec(that->bitsAsUnsizedConst(width_hint));
	} else {
		return RTLIL::SigSpec(that->bitsAsConst());
	}
}

RTLIL::SigSpec AstRealvalue::genRTLIL(int width_hint, bool sign_hint) {
	AstNode *that = node;
	if (width_hint < 0)
		that->detectSignWidth(width_hint, sign_hint);
	that->is_signed = sign_hint;

	RTLIL::SigSpec sig = that->realAsConst(width_hint);
	log_file_warning(*that->location.begin.filename, that->location.begin.line,
		"converting real value %e to binary %s.\n", that->realvalue, log_signal(sig));
	return sig;
}

RTLIL::SigSpec AstSelfSz::genRTLIL(int width_hint, bool sign_hint) {
	AstNode *that = node;
	RTLIL::SigSpec sig = that->children[0]->genRTLIL();
	if (sig.size() < width_hint)
		sig.extend_u0(width_hint, sign_hint);
	that->is_signed = sign_hint;
	return sig;
}

RTLIL::SigSpec AstCastSize::genRTLIL(int /*width_hint*/, bool sign_hint) {
	AstNode *that = node;
	RTLIL::SigSpec size = that->children[0]->genRTLIL();
	if (!size.is_fully_const())
		that->input_error("Static cast with non constant expression!\n");
	int width = size.as_int();
	if (width <= 0)
		that->input_error("Static cast with zero or negative size!\n");
	int sub_width_hint = -1;
	bool sub_sign_hint = true;
	that->children[1]->detectSignWidth(sub_width_hint, sub_sign_hint);
	RTLIL::SigSpec sig = that->children[1]->genWidthRTLIL(width, sub_sign_hint);
	that->is_signed = sign_hint;
	return sig;
}

RTLIL::SigSpec AstConcat::genRTLIL(int width_hint, bool /*sign_hint*/) {
	AstNode *that = node;
	RTLIL::SigSpec sig;
	for (auto it = that->children.begin(); it != that->children.end(); it++)
		sig.append((*it)->genRTLIL());
	if (sig.size() < width_hint)
		sig.extend_u0(width_hint, false);
	return sig;
}

RTLIL::SigSpec AstReplicate::genRTLIL(int width_hint, bool /*sign_hint*/) {
	AstNode *that = node;
	RTLIL::SigSpec left = that->children[0]->genRTLIL();
	RTLIL::SigSpec right = that->children[1]->genRTLIL();
	if (!left.is_fully_const())
		that->input_error("Left operand of replicate expression is not constant!\n");
	int count = left.as_int();
	RTLIL::SigSpec sig;
	for (int i = 0; i < count; i++)
		sig.append(right);
	if (sig.size() < width_hint)
		sig.extend_u0(width_hint, false);
	that->is_signed = false;
	return sig;
}

RTLIL::SigSpec AstTernary::genRTLIL(int width_hint, bool sign_hint) {
	AstNode *that = node;
	if (width_hint < 0)
		that->detectSignWidth(width_hint, sign_hint);
	that->is_signed = sign_hint;

	RTLIL::SigSpec cond = that->children[0]->genRTLIL();
	RTLIL::SigSpec sig;

	if (cond.is_fully_def())
	{
		if (cond.as_bool()) {
			sig = that->children[1]->genRTLIL(width_hint, sign_hint);
			log_assert(that->is_signed == that->children[1]->is_signed);
		} else {
			sig = that->children[2]->genRTLIL(width_hint, sign_hint);
			log_assert(that->is_signed == that->children[2]->is_signed);
		}

		widthExtend(that, sig, sig.size(), that->is_signed);
	}
	else
	{
		RTLIL::SigSpec val1 = that->children[1]->genRTLIL(width_hint, sign_hint);
		RTLIL::SigSpec val2 = that->children[2]->genRTLIL(width_hint, sign_hint);

		if (cond.size() > 1)
			cond = uniop2rtlil(that, ID($reduce_bool), 1, cond, false);

		int width = max(val1.size(), val2.size());
		log_assert(that->is_signed == that->children[1]->is_signed);
		log_assert(that->is_signed == that->children[2]->is_signed);
		widthExtend(that, val1, width, that->is_signed);
		widthExtend(that, val2, width, that->is_signed);

		sig = mux2rtlil(AstTernary{that}, cond, val1, val2);
	}

	if (sig.size() < width_hint)
		sig.extend_u0(width_hint, sign_hint);
	return sig;
}

RTLIL::SigSpec AstMemRd::genRTLIL(int /*width_hint*/, bool sign_hint) {
	AstNode *that = node;
	std::stringstream sstr;
	sstr << "$memrd$" << that->str << "$" << RTLIL::encode_filename(*that->location.begin.filename)
	     << ":" << that->location.begin.line << "$" << (autoidx++);

	RTLIL::Cell *cell = current_module->addCell(sstr.str(), ID($memrd));
	set_src_attr(cell, that);

	RTLIL::Wire *wire = current_module->addWire(cell->name.str() + "_DATA", current_module->memories[that->str]->width);
	set_src_attr(wire, that);

	AstMemory mem(that->id2ast);
	int mem_width, mem_size, addr_bits;
	that->is_signed = mem.is_signed();
	wire->is_signed = that->is_signed;
	mem.meminfo(mem_width, mem_size, addr_bits);

	RTLIL::SigSpec addr_sig = that->children[0]->genRTLIL();

	cell->setPort(ID::CLK, RTLIL::SigSpec(RTLIL::State::Sx, 1));
	cell->setPort(ID::EN, RTLIL::SigSpec(RTLIL::State::Sx, 1));
	cell->setPort(ID::ADDR, addr_sig);
	cell->setPort(ID::DATA, RTLIL::SigSpec(wire));

	cell->parameters[ID::MEMID] = RTLIL::Const(that->str);
	cell->parameters[ID::ABITS] = RTLIL::Const(GetSize(addr_sig));
	cell->parameters[ID::WIDTH] = RTLIL::Const(wire->width);

	cell->parameters[ID::CLK_ENABLE] = RTLIL::Const(0);
	cell->parameters[ID::CLK_POLARITY] = RTLIL::Const(0);
	cell->parameters[ID::TRANSPARENT] = RTLIL::Const(0);

	if (!sign_hint)
		that->is_signed = false;

	return RTLIL::SigSpec(wire);
}

RTLIL::SigSpec AstMemInit::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	AstMemInit init(that);
	std::stringstream sstr;
	sstr << "$meminit$" << that->str << "$" << RTLIL::encode_filename(*that->location.begin.filename)
	     << ":" << that->location.begin.line << "$" << (autoidx++);

	SigSpec en_sig = init.en()->genRTLIL();

	RTLIL::Cell *cell = current_module->addCell(sstr.str(), ID($meminit_v2));
	set_src_attr(cell, that);

	int mem_width, mem_size, addr_bits;
	AstMemory(that->id2ast).meminfo(mem_width, mem_size, addr_bits);

	if (!AstConstant::matches(init.count()))
		that->input_error("Memory init with non-constant word count!\n");
	int num_words = int(init.count()->asInt(false));
	cell->parameters[ID::WORDS] = RTLIL::Const(num_words);

	SigSpec addr_sig = init.addr()->genRTLIL();

	cell->setPort(ID::ADDR, addr_sig);
	cell->setPort(ID::DATA, init.data()->genWidthRTLIL(current_module->memories[that->str]->width * num_words, true));
	cell->setPort(ID::EN, en_sig);

	cell->parameters[ID::MEMID] = RTLIL::Const(that->str);
	cell->parameters[ID::ABITS] = RTLIL::Const(GetSize(addr_sig));
	cell->parameters[ID::WIDTH] = RTLIL::Const(current_module->memories[that->str]->width);

	cell->parameters[ID::PRIORITY] = RTLIL::Const(autoidx-1);
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstAnyFormalAssertion::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	std::string flavor, desc;
	if (that->type == AST_ASSERT) { flavor = "assert"; desc = "assert property ()"; }
	if (that->type == AST_ASSUME) { flavor = "assume"; desc = "assume property ()"; }
	if (that->type == AST_LIVE)   { flavor = "live";   desc = "assert property (eventually)"; }
	if (that->type == AST_FAIR)   { flavor = "fair";   desc = "assume property (eventually)"; }
	if (that->type == AST_COVER)  { flavor = "cover";  desc = "cover property ()"; }

	IdString cellname;
	if (that->str.empty())
		cellname = stringf("$%s$%s:%d$%d", flavor.c_str(), RTLIL::encode_filename(*that->location.begin.filename).c_str(), that->location.begin.line, autoidx++);
	else
		cellname = that->str;
	AST_INTERNAL::check_unique_id(current_module, cellname, that, "procedural assertion");

	RTLIL::SigSpec check = that->children[0]->genRTLIL();
	if (GetSize(check) != 1)
		check = current_module->ReduceBool(NEW_ID, check);

	RTLIL::Cell *cell = current_module->addCell(cellname, ID($check));
	set_src_attr(cell, that);
	AST_INTERNAL::copy_const_attributes(cell, that);
	cell->setParam(ID(FLAVOR), flavor);
	cell->parameters[ID::TRG_WIDTH] = 0;
	cell->parameters[ID::TRG_ENABLE] = 0;
	cell->parameters[ID::TRG_POLARITY] = 0;
	cell->parameters[ID::PRIORITY] = 0;
	cell->setPort(ID::TRG, RTLIL::SigSpec());
	cell->setPort(ID::EN, RTLIL::S1);
	cell->setPort(ID::A, check);

	Fmt fmt;
	fmt.emit_rtlil(cell);
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstAssign::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	RTLIL::SigSpec left = that->children[0]->genRTLIL();
	RTLIL::SigSpec right = that->children[1]->genWidthRTLIL(left.size(), true);
	if (left.has_const()) {
		RTLIL::SigSpec new_left, new_right;
		for (int i = 0; i < GetSize(left); i++)
			if (left[i].wire) {
				new_left.append(left[i]);
				new_right.append(right[i]);
			}
		log_file_warning(*that->location.begin.filename, that->location.begin.line, "Ignoring assignment to constant bits:\n"
				"    old assignment: %s = %s\n    new assignment: %s = %s.\n",
				log_signal(left), log_signal(right),
				log_signal(new_left), log_signal(new_right));
		left = new_left;
		right = new_right;
	}
	current_module->connect(RTLIL::SigSig(left, right));
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstCell::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	int port_counter = 0, para_counter = 0;

	RTLIL::IdString id = that->str;
	AST_INTERNAL::check_unique_id(current_module, id, that, "cell");
	RTLIL::Cell *cell = current_module->addCell(id, "");
	set_src_attr(cell, that);

	for (auto it = that->children.begin(); it != that->children.end(); it++) {
		auto* child = it->get();
		if (AstCelltype::matches(child)) {
			cell->type = child->str;
			if (flag_icells && cell->type.begins_with("\\$"))
				cell->type = cell->type.substr(1);
			continue;
		}
		if (AstParaset::matches(child)) {
			IdString paraname = child->str.empty() ? stringf("$%d", ++para_counter) : child->str;
			const auto* value = child->children[0].get();
			if (AstRealvalue::matches(value))
				log_file_warning(*that->location.begin.filename, that->location.begin.line, "Replacing floating point parameter %s.%s = %f with string.\n",
						log_id(cell), log_id(paraname), value->realvalue);
			else if (!AstConstant::matches(value))
				that->input_error("Parameter %s.%s with non-constant value!\n",
						log_id(cell), log_id(paraname));
			cell->parameters[paraname] = value->asParaConst();
			continue;
		}
		if (AstArgument::matches(child)) {
			RTLIL::SigSpec sig;
			if (child->children.size() > 0) {
				auto* arg = child->children[0].get();
				int local_width_hint = -1;
				bool local_sign_hint = false;
				if (!AstIdentifier::matches(arg) || !arg->id2ast || !AstCell::matches(arg->id2ast))
					arg->detectSignWidth(local_width_hint, local_sign_hint);
				sig = arg->genRTLIL(local_width_hint, local_sign_hint);
				log_assert(local_sign_hint == arg->is_signed);
				if (sig.is_wire()) {
					if (AstIdentifier::matches(arg) && arg->id2ast && arg->id2ast->is_signed && !arg->is_signed)
						log_assert(that->attributes.count(ID::reprocess_after));
					else
						log_assert(arg->is_signed == sig.as_wire()->is_signed);
				} else if (arg->is_signed) {
					RTLIL::IdString wire_name = NEW_ID;
					RTLIL::Wire *wire = current_module->addWire(wire_name, GetSize(sig));
					wire->is_signed = true;
					current_module->connect(wire, sig);
					sig = wire;
				}
			}
			if (child->str.size() == 0) {
				char buf[100];
				snprintf(buf, 100, "$%d", ++port_counter);
				cell->setPort(buf, sig);
			} else {
				cell->setPort(child->str, sig);
			}
			continue;
		}
		log_abort();
	}

	if (cell->type.isPublic())
		cell->set_bool_attribute(ID::module_not_derived);

	AST_INTERNAL::copy_const_attributes(cell, that);
	if (cell->type == ID($specify2)) {
		int src_width = GetSize(cell->getPort(ID::SRC));
		int dst_width = GetSize(cell->getPort(ID::DST));
		bool full = cell->getParam(ID::FULL).as_bool();
		if (!full && src_width != dst_width)
			that->input_error("Parallel specify SRC width does not match DST width.\n");
		cell->setParam(ID::SRC_WIDTH, Const(src_width));
		cell->setParam(ID::DST_WIDTH, Const(dst_width));
	}
	else if (cell->type ==  ID($specify3)) {
		int dat_width = GetSize(cell->getPort(ID::DAT));
		int dst_width = GetSize(cell->getPort(ID::DST));
		if (dat_width != dst_width)
			that->input_error("Specify DAT width does not match DST width.\n");
		int src_width = GetSize(cell->getPort(ID::SRC));
		cell->setParam(ID::SRC_WIDTH, Const(src_width));
		cell->setParam(ID::DST_WIDTH, Const(dst_width));
	}
	else if (cell->type == ID($specrule)) {
		int src_width = GetSize(cell->getPort(ID::SRC));
		int dst_width = GetSize(cell->getPort(ID::DST));
		cell->setParam(ID::SRC_WIDTH, Const(src_width));
		cell->setParam(ID::DST_WIDTH, Const(dst_width));
	}
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstAlways::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	ignoreThisSignalsInInitial.append(AST_INTERNAL::generate_process(that->clone()));
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstInitial::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	AST_INTERNAL::generate_process(that->clone(), ignoreThisSignalsInInitial);
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstTecall::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	int sz = that->children.size();
	if (that->str == "$info") {
		if (sz > 0)
			log_file_info(*that->location.begin.filename, that->location.begin.line, "%s.\n", that->children[0]->str.c_str());
		else
			log_file_info(*that->location.begin.filename, that->location.begin.line, "\n");
	} else if (that->str == "$warning") {
		if (sz > 0)
			log_file_warning(*that->location.begin.filename, that->location.begin.line, "%s.\n", that->children[0]->str.c_str());
		else
			log_file_warning(*that->location.begin.filename, that->location.begin.line, "\n");
	} else if (that->str == "$error") {
		if (sz > 0)
			that->input_error("%s.\n", that->children[0]->str.c_str());
		else
			that->input_error("\n");
	} else if (that->str == "$fatal") {
		if (sz > 0)
			that->input_error("FATAL: %s.\n", that->children[0]->str.c_str());
		else
			that->input_error("FATAL.\n");
	} else {
		that->input_error("Unknown elaboration system task '%s'.\n", that->str.c_str());
	}
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstBind::genRTLIL(int /*width_hint*/, bool /*sign_hint*/) {
	AstNode *that = node;
	for (RTLIL::Binding *binding : that->genBindings())
		current_module->add(binding);
	return RTLIL::SigSpec();
}

RTLIL::SigSpec AstIdentifier::genRTLIL(int /*width_hint*/, bool sign_hint) {
	AstNode *that = node;
	RTLIL::Wire *wire = nullptr;
	RTLIL::SigChunk chunk;
	bool is_interface = false;

	AST::AstNode *member_node = nullptr;
	int add_undef_bits_msb = 0;
	int add_undef_bits_lsb = 0;

	log_assert(that->id2ast != nullptr);

	if (AstAutowire::matches(that->id2ast) && current_module->wires_.count(that->str) == 0) {
		RTLIL::Wire *w = current_module->addWire(that->str);
		set_src_attr(w, that);
		w->name = that->str;

		if (dynamic_cast<RTLIL::Binding*>(current_module)) {
			/* nothing to do here */
		} else if (flag_autowire)
			log_file_warning(*that->location.begin.filename, that->location.begin.line, "Identifier `%s' is implicitly declared.\n", that->str.c_str());
		else
			that->input_error("Identifier `%s' is implicitly declared and `default_nettype is set to none.\n", that->str.c_str());
	}
	else if (AstAnyParamLike::matches(that->id2ast)) {
		if (!AstConstant::matches(that->id2ast->children[0].get()))
			that->input_error("Parameter %s does not evaluate to constant value!\n", that->str.c_str());
		chunk = RTLIL::Const(that->id2ast->children[0]->bits);
		goto use_const_chunk;
	}
	else if ((ChildConstraint<AST_WIRE, AST_AUTOWIRE, AST_MEMORY>::accepts(that->id2ast)) && current_module->wires_.count(that->str) != 0) {
		RTLIL::Wire *current_wire = current_module->wire(that->str);
		if (current_wire->get_bool_attribute(ID::is_interface))
			is_interface = true;
		// Ignore
	}
	else if (1) { // FIXME: Check if sv_mode first?
		is_interface = true;
	}
	else {
		that->input_error("Identifier `%s' doesn't map to any signal!\n", that->str.c_str());
	}

	if (AstMemory::matches(that->id2ast))
		that->input_error("Identifier `%s' does map to an unexpanded memory!\n", that->str.c_str());

	if (is_interface) {
		IdString dummy_wire_name = stringf("$dummywireforinterface%s", that->str.c_str());
		RTLIL::Wire *dummy_wire = current_module->wire(dummy_wire_name);
		if (!dummy_wire) {
			dummy_wire = current_module->addWire(dummy_wire_name);
			dummy_wire->set_bool_attribute(ID::is_interface);
		}
		return dummy_wire;
	}

	wire = current_module->wires_[that->str];
	chunk.wire = wire;
	chunk.width = wire->width;
	chunk.offset = 0;

	if ((member_node = that->get_struct_member())) {
		chunk.width = member_node->range_left - member_node->range_right + 1;
		chunk.offset = member_node->range_right;
	}

use_const_chunk:
	if (that->children.size() != 0) {
		if (!AstRange::matches(that->children[0].get()))
			that->input_error("Single range expected.\n");
		int source_width = that->id2ast->range_left - that->id2ast->range_right + 1;
		int source_offset = that->id2ast->range_right;
		int chunk_left = source_width - 1;
		int chunk_right = 0;

		if (member_node) {
			log_assert(!source_offset && !that->id2ast->range_swapped);
			chunk_left = chunk.offset + chunk.width - 1;
			chunk_right = chunk.offset;
		}

		if (!that->children[0]->range_valid) {
			auto left_at_zero_ast = that->children[0]->children[0]->clone_at_zero();
			auto right_at_zero_ast = that->children[0]->children.size() >= 2 ? that->children[0]->children[1]->clone_at_zero() : left_at_zero_ast->clone();
			while (left_at_zero_ast->simplify()) { }
			while (right_at_zero_ast->simplify()) { }
			if (!AstConstant::matches(left_at_zero_ast.get()) || !AstConstant::matches(right_at_zero_ast.get()))
				that->input_error("Unsupported expression on dynamic range select on signal `%s'!\n", that->str.c_str());
			int width = abs(int(left_at_zero_ast->integer - right_at_zero_ast->integer)) + 1;
			auto fake_ast = std::make_unique<AstNode>(that->children[0]->location, AST_NONE, that->clone(), that->children[0]->children.size() >= 2 ?
					that->children[0]->children[1]->clone() : that->children[0]->children[0]->clone());
			fake_ast->children[0]->delete_children();
			if (member_node)
				fake_ast->children[0]->set_attribute(ID::wiretype, member_node->clone());

			int fake_ast_width = 0;
			bool fake_ast_sign = true;
			fake_ast->children[1]->detectSignWidth(fake_ast_width, fake_ast_sign);
			RTLIL::SigSpec shift_val = fake_ast->children[1]->genRTLIL(fake_ast_width, fake_ast_sign);

			if (source_offset != 0) {
				shift_val = current_module->Sub(NEW_ID, shift_val, source_offset, fake_ast_sign);
				fake_ast->children[1]->is_signed = true;
			}
			if (that->id2ast->range_swapped) {
				shift_val = current_module->Sub(NEW_ID, RTLIL::SigSpec(source_width - width), shift_val, fake_ast_sign);
				fake_ast->children[1]->is_signed = true;
			}
			if (GetSize(shift_val) >= 32)
				fake_ast->children[1]->is_signed = true;
			RTLIL::SigSpec sig = binop2rtlil(fake_ast.get(), ID($shiftx), width, fake_ast->children[0]->genRTLIL(), shift_val);
			return sig;
		} else {
			chunk.width = that->children[0]->range_left - that->children[0]->range_right + 1;
			chunk.offset += that->children[0]->range_right - source_offset;
			if (that->id2ast->range_swapped)
				chunk.offset = source_width - (chunk.offset + chunk.width);
			if (chunk.offset > chunk_left || chunk.offset + chunk.width < chunk_right) {
				if (chunk.width == 1)
					log_file_warning(*that->location.begin.filename, that->location.begin.line, "Range select out of bounds on signal `%s': Setting result bit to undef.\n",
							that->str.c_str());
				else
					log_file_warning(*that->location.begin.filename, that->location.begin.line, "Range select [%d:%d] out of bounds on signal `%s': Setting all %d result bits to undef.\n",
							that->children[0]->range_left, that->children[0]->range_right, that->str.c_str(), chunk.width);
				chunk = RTLIL::SigChunk(RTLIL::State::Sx, chunk.width);
			} else {
				if (chunk.offset + chunk.width - 1 > chunk_left) {
					add_undef_bits_msb = (chunk.offset + chunk.width - 1) - chunk_left;
					chunk.width -= add_undef_bits_msb;
				}
				if (chunk.offset < chunk_right) {
					add_undef_bits_lsb = chunk_right - chunk.offset;
					chunk.width -= add_undef_bits_lsb;
					chunk.offset += add_undef_bits_lsb;
				}
				if (add_undef_bits_lsb)
					log_file_warning(*that->location.begin.filename, that->location.begin.line, "Range [%d:%d] select out of bounds on signal `%s': Setting %d LSB bits to undef.\n",
							that->children[0]->range_left, that->children[0]->range_right, that->str.c_str(), add_undef_bits_lsb);
				if (add_undef_bits_msb)
					log_file_warning(*that->location.begin.filename, that->location.begin.line, "Range [%d:%d] select out of bounds on signal `%s': Setting %d MSB bits to undef.\n",
							that->children[0]->range_left, that->children[0]->range_right, that->str.c_str(), add_undef_bits_msb);
			}
		}
	}

	{
		RTLIL::SigSpec sig = { RTLIL::SigSpec(RTLIL::State::Sx, add_undef_bits_msb), chunk, RTLIL::SigSpec(RTLIL::State::Sx, add_undef_bits_lsb) };

		if (genRTLIL_subst_ptr)
			sig.replace(*genRTLIL_subst_ptr);

		that->is_signed = that->children.size() > 0 ? false : that->id2ast->is_signed && sign_hint;
		return sig;
	}
}

template<AstNodeType Tag>
RTLIL::SigSpec AstUnaryOp<Tag>::genRTLIL(int width_hint, bool sign_hint) {
	AstNode *that = this->node;
	if constexpr (Tag == AST_BIT_NOT || Tag == AST_POS || Tag == AST_NEG) {
		IdString type_name = (Tag == AST_BIT_NOT) ? ID($not) : (Tag == AST_POS) ? ID($pos) : ID($neg);
		RTLIL::SigSpec arg = that->children[0]->genRTLIL(width_hint, sign_hint);
		that->is_signed = that->children[0]->is_signed;
		int width = arg.size();
		if (width_hint > 0) {
			width = width_hint;
			widthExtend(that, arg, width, that->is_signed);
		}
		return uniop2rtlil(that, type_name, width, arg);
	} else if constexpr (Tag == AST_REDUCE_AND || Tag == AST_REDUCE_OR || Tag == AST_REDUCE_XOR || Tag == AST_REDUCE_XNOR) {
		IdString type_name = (Tag == AST_REDUCE_AND)  ? ID($reduce_and)
		                   : (Tag == AST_REDUCE_OR)   ? ID($reduce_or)
		                   : (Tag == AST_REDUCE_XOR)  ? ID($reduce_xor)
		                                              : ID($reduce_xnor);
		RTLIL::SigSpec arg = that->children[0]->genRTLIL();
		return uniop2rtlil(that, type_name, max(width_hint, 1), arg);
	} else if constexpr (Tag == AST_REDUCE_BOOL) {
		RTLIL::SigSpec arg = that->children[0]->genRTLIL();
		return arg.size() > 1 ? uniop2rtlil(that, ID($reduce_bool), max(width_hint, 1), arg) : arg;
	} else if constexpr (Tag == AST_LOGIC_NOT) {
		RTLIL::SigSpec arg = that->children[0]->genRTLIL();
		return uniop2rtlil(that, ID($logic_not), max(width_hint, 1), arg);
	} else if constexpr (Tag == AST_TO_SIGNED || Tag == AST_TO_UNSIGNED) {
		RTLIL::SigSpec sig = that->children[0]->genRTLIL();
		if (sig.size() < width_hint)
			sig.extend_u0(width_hint, sign_hint);
		that->is_signed = sign_hint;
		return sig;
	} else {
		log_abort();
		return RTLIL::SigSpec();
	}
}

template RTLIL::SigSpec AstUnaryOp<AST_BIT_NOT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_POS>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_NEG>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_REDUCE_AND>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_REDUCE_OR>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_REDUCE_XOR>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_REDUCE_XNOR>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_REDUCE_BOOL>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_LOGIC_NOT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_TO_SIGNED>::genRTLIL(int, bool);
template RTLIL::SigSpec AstUnaryOp<AST_TO_UNSIGNED>::genRTLIL(int, bool);

template<AstNodeType Tag>
RTLIL::SigSpec AstBinaryOp<Tag>::genRTLIL(int width_hint, bool sign_hint) {
	AstNode *that = this->node;
	if constexpr (Tag == AST_BIT_AND || Tag == AST_BIT_OR || Tag == AST_BIT_XOR || Tag == AST_BIT_XNOR) {
		IdString type_name = (Tag == AST_BIT_AND) ? ID($and)
		                   : (Tag == AST_BIT_OR)  ? ID($or)
		                   : (Tag == AST_BIT_XOR) ? ID($xor)
		                                          : ID($xnor);
		if (width_hint < 0)
			that->detectSignWidth(width_hint, sign_hint);
		RTLIL::SigSpec left = that->children[0]->genRTLIL(width_hint, sign_hint);
		RTLIL::SigSpec right = that->children[1]->genRTLIL(width_hint, sign_hint);
		int width = max(left.size(), right.size());
		if (width_hint > 0)
			width = width_hint;
		that->is_signed = that->children[0]->is_signed && that->children[1]->is_signed;
		return binop2rtlil(that, type_name, width, left, right);
	} else if constexpr (Tag == AST_SHIFT_LEFT || Tag == AST_SHIFT_RIGHT ||
	                     Tag == AST_SHIFT_SLEFT || Tag == AST_SHIFT_SRIGHT ||
	                     Tag == AST_SHIFTX || Tag == AST_SHIFT) {
		IdString type_name = (Tag == AST_SHIFT_LEFT)   ? ID($shl)
		                   : (Tag == AST_SHIFT_RIGHT)  ? ID($shr)
		                   : (Tag == AST_SHIFT_SLEFT)  ? ID($sshl)
		                   : (Tag == AST_SHIFT_SRIGHT) ? ID($sshr)
		                   : (Tag == AST_SHIFTX)       ? ID($shiftx)
		                                               : ID($shift);
		if (width_hint < 0)
			that->detectSignWidth(width_hint, sign_hint);
		RTLIL::SigSpec left = that->children[0]->genRTLIL(width_hint, sign_hint);
		RTLIL::SigSpec right = that->children[1]->genRTLIL(-1, Tag == AST_SHIFT || Tag == AST_SHIFTX);
		int width = width_hint > 0 ? width_hint : left.size();
		that->is_signed = that->children[0]->is_signed;
		return binop2rtlil(that, type_name, width, left, right);
	} else if constexpr (Tag == AST_POW) {
		int right_width;
		bool right_signed;
		that->children[1]->detectSignWidth(right_width, right_signed);
		if (width_hint < 0)
			that->detectSignWidth(width_hint, sign_hint);
		RTLIL::SigSpec left = that->children[0]->genRTLIL(width_hint, sign_hint);
		RTLIL::SigSpec right = that->children[1]->genRTLIL(right_width, right_signed);
		int width = width_hint > 0 ? width_hint : left.size();
		that->is_signed = that->children[0]->is_signed;
		if (!flag_noopt && left.is_fully_const() && left.as_int() == 2 && !right_signed)
			return binop2rtlil(that, ID($shl), width, RTLIL::SigSpec(1, left.size()), right);
		return binop2rtlil(that, ID($pow), width, left, right);
	} else if constexpr (Tag == AST_LT || Tag == AST_LE || Tag == AST_EQ ||
	                     Tag == AST_NE || Tag == AST_EQX || Tag == AST_NEX ||
	                     Tag == AST_GE || Tag == AST_GT) {
		IdString type_name = (Tag == AST_LT)  ? ID($lt)
		                   : (Tag == AST_LE)  ? ID($le)
		                   : (Tag == AST_EQ)  ? ID($eq)
		                   : (Tag == AST_NE)  ? ID($ne)
		                   : (Tag == AST_EQX) ? ID($eqx)
		                   : (Tag == AST_NEX) ? ID($nex)
		                   : (Tag == AST_GE)  ? ID($ge)
		                                      : ID($gt);
		int width = max(width_hint, 1);
		width_hint = -1, sign_hint = true;
		that->children[0]->detectSignWidthWorker(width_hint, sign_hint);
		that->children[1]->detectSignWidthWorker(width_hint, sign_hint);
		RTLIL::SigSpec left = that->children[0]->genRTLIL(width_hint, sign_hint);
		RTLIL::SigSpec right = that->children[1]->genRTLIL(width_hint, sign_hint);
		return binop2rtlil(that, type_name, width, left, right);
	} else if constexpr (Tag == AST_ADD || Tag == AST_SUB || Tag == AST_MUL ||
	                     Tag == AST_DIV || Tag == AST_MOD) {
		IdString type_name = (Tag == AST_ADD) ? ID($add)
		                   : (Tag == AST_SUB) ? ID($sub)
		                   : (Tag == AST_MUL) ? ID($mul)
		                   : (Tag == AST_DIV) ? ID($div)
		                                      : ID($mod);
		if (width_hint < 0)
			that->detectSignWidth(width_hint, sign_hint);
		RTLIL::SigSpec left = that->children[0]->genRTLIL(width_hint, sign_hint);
		RTLIL::SigSpec right = that->children[1]->genRTLIL(width_hint, sign_hint);
		int width = max(max(left.size(), right.size()), width_hint);
		that->is_signed = that->children[0]->is_signed && that->children[1]->is_signed;
		return binop2rtlil(that, type_name, width, left, right);
	} else if constexpr (Tag == AST_LOGIC_AND || Tag == AST_LOGIC_OR) {
		IdString type_name = (Tag == AST_LOGIC_AND) ? ID($logic_and) : ID($logic_or);
		RTLIL::SigSpec left = that->children[0]->genRTLIL();
		RTLIL::SigSpec right = that->children[1]->genRTLIL();
		return binop2rtlil(that, type_name, max(width_hint, 1), left, right);
	} else {
		log_abort();
		return RTLIL::SigSpec();
	}
}

template RTLIL::SigSpec AstBinaryOp<AST_BIT_AND>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_BIT_OR>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_BIT_XOR>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_BIT_XNOR>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_SHIFT_LEFT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_SHIFT_RIGHT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_SHIFT_SLEFT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_SHIFT_SRIGHT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_SHIFTX>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_SHIFT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_POW>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_LT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_LE>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_EQ>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_NE>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_EQX>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_NEX>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_GE>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_GT>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_ADD>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_SUB>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_MUL>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_DIV>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_MOD>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_LOGIC_AND>::genRTLIL(int, bool);
template RTLIL::SigSpec AstBinaryOp<AST_LOGIC_OR>::genRTLIL(int, bool);

RTLIL::SigSpec AstFcall::genRTLIL(int width_hint, bool sign_hint) {
	AstNode *that = node;
	if (that->str == "\\$anyconst" || that->str == "\\$anyseq" || that->str == "\\$allconst" || that->str == "\\$allseq")
	{
		string myid = stringf("%s$%d", that->str.c_str() + 1, autoidx++);
		int width = width_hint;

		if (GetSize(that->children) > 1)
			that->input_error("System function %s got %d arguments, expected 1 or 0.\n",
					RTLIL::unescape_id(that->str).c_str(), GetSize(that->children));

		if (GetSize(that->children) == 1) {
			if (!AstConstant::matches(that->children[0].get()))
				that->input_error("System function %s called with non-const argument!\n",
						RTLIL::unescape_id(that->str).c_str());
			width = that->children[0]->asInt(true);
		}

		if (width <= 0)
			that->input_error("Failed to detect width of %s!\n", RTLIL::unescape_id(that->str).c_str());

		Cell *cell = current_module->addCell(myid, that->str.substr(1));
		set_src_attr(cell, that);
		cell->parameters[ID::WIDTH] = width;

		if (that->attributes.count(ID::reg)) {
			auto &attr = that->attributes.at(ID::reg);
			if (!AstConstant::matches(attr.get()))
				that->input_error("Attribute `reg' with non-constant value!\n");
			cell->attributes[ID::reg] = attr->asAttrConst();
		}

		Wire *wire = current_module->addWire(myid + "_wire", width);
		set_src_attr(wire, that);
		cell->setPort(ID::Y, wire);

		that->is_signed = sign_hint;
		return SigSpec(wire);
	}
	for (auto f : log_files)
		AST_INTERNAL::current_ast_mod->dumpAst(f, "verilog-ast> ");
	that->input_error("Don't know how to generate RTLIL code for %s node!\n", type2str(that->type).c_str());
	return RTLIL::SigSpec();
}

} // namespace AST

// create RTLIL from an AST node
// all generated cells, wires and processes are added to the module pointed to by 'current_module'
// when the AST node is an expression (AST_ADD, AST_BIT_XOR, etc.), the result signal is returned.
//
// note that this function is influenced by a number of global variables that might be set when
// called from genWidthRTLIL(). also note that this function recursively calls itself to transform
// larger expressions into a netlist of cells.
RTLIL::SigSpec AstNode::genRTLIL(int width_hint, bool sign_hint)
{
	using namespace AST;

	switch (type) {
	case AST_NONE: case AST_TASK: case AST_FUNCTION: case AST_DPI_FUNCTION:
	case AST_AUTOWIRE: case AST_DEFPARAM: case AST_GENVAR: case AST_GENFOR:
	case AST_GENBLOCK: case AST_GENIF: case AST_GENCASE: case AST_PACKAGE:
	case AST_IMPORT: case AST_ENUM: case AST_MODPORT: case AST_MODPORTMEMBER:
	case AST_TYPEDEF: case AST_STRUCT: case AST_UNION: case AST_INTERFACEPORTTYPE:
		return RTLIL::SigSpec();
	default: break;
	}

	if (auto v = AstInterfacePort::cast(this)) return v->genRTLIL(width_hint, sign_hint);
	if (ParameterLike::accepts(this))          return AstAnyParamLike(this).genRTLIL(width_hint, sign_hint);
	if (auto v = AstWire::cast(this))          return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstMemory::cast(this))        return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstConstant::cast(this))      return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstRealvalue::cast(this))     return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstIdentifier::cast(this))    return v->genRTLIL(width_hint, sign_hint);

#define UNOP_DISPATCH(T)  if (auto v = AstUnaryOp<T>::cast(this))  return v->genRTLIL(width_hint, sign_hint)
#define BINOP_DISPATCH(T) if (auto v = AstBinaryOp<T>::cast(this)) return v->genRTLIL(width_hint, sign_hint)

	UNOP_DISPATCH(AST_TO_SIGNED);
	UNOP_DISPATCH(AST_TO_UNSIGNED);
	if (auto v = AstSelfSz::cast(this))    return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstCastSize::cast(this))  return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstConcat::cast(this))    return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstReplicate::cast(this)) return v->genRTLIL(width_hint, sign_hint);

	UNOP_DISPATCH(AST_BIT_NOT); UNOP_DISPATCH(AST_POS); UNOP_DISPATCH(AST_NEG);
	BINOP_DISPATCH(AST_BIT_AND); BINOP_DISPATCH(AST_BIT_OR); BINOP_DISPATCH(AST_BIT_XOR); BINOP_DISPATCH(AST_BIT_XNOR);
	UNOP_DISPATCH(AST_REDUCE_AND); UNOP_DISPATCH(AST_REDUCE_OR); UNOP_DISPATCH(AST_REDUCE_XOR); UNOP_DISPATCH(AST_REDUCE_XNOR);
	UNOP_DISPATCH(AST_REDUCE_BOOL);
	BINOP_DISPATCH(AST_SHIFT_LEFT); BINOP_DISPATCH(AST_SHIFT_RIGHT); BINOP_DISPATCH(AST_SHIFT_SLEFT); BINOP_DISPATCH(AST_SHIFT_SRIGHT);
	BINOP_DISPATCH(AST_SHIFTX); BINOP_DISPATCH(AST_SHIFT); BINOP_DISPATCH(AST_POW);
	BINOP_DISPATCH(AST_LT); BINOP_DISPATCH(AST_LE); BINOP_DISPATCH(AST_EQ); BINOP_DISPATCH(AST_NE);
	BINOP_DISPATCH(AST_EQX); BINOP_DISPATCH(AST_NEX); BINOP_DISPATCH(AST_GE); BINOP_DISPATCH(AST_GT);
	BINOP_DISPATCH(AST_ADD); BINOP_DISPATCH(AST_SUB); BINOP_DISPATCH(AST_MUL); BINOP_DISPATCH(AST_DIV); BINOP_DISPATCH(AST_MOD);
	BINOP_DISPATCH(AST_LOGIC_AND); BINOP_DISPATCH(AST_LOGIC_OR);
	UNOP_DISPATCH(AST_LOGIC_NOT);

#undef UNOP_DISPATCH
#undef BINOP_DISPATCH

	if (auto v = AstTernary::cast(this))             return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstMemRd::cast(this))               return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstMemInit::cast(this))             return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstAnyFormalAssertion::cast(this))  return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstAssign::cast(this))              return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstCell::cast(this))                return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstAlways::cast(this))              return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstInitial::cast(this))             return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstTecall::cast(this))              return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstBind::cast(this))                return v->genRTLIL(width_hint, sign_hint);
	if (auto v = AstFcall::cast(this))               return v->genRTLIL(width_hint, sign_hint);

	for (auto f : log_files)
		AST_INTERNAL::current_ast_mod->dumpAst(f, "verilog-ast> ");
	input_error("Don't know how to generate RTLIL code for %s node!\n", type2str(type).c_str());
	return RTLIL::SigSpec();
}

// this is a wrapper for AstNode::genRTLIL() when a specific signal width is requested and/or
// signals must be substituted before being used as input values (used by ProcessGenerator)
// note that this is using some global variables to communicate this special settings to AstNode::genRTLIL().
RTLIL::SigSpec AstNode::genWidthRTLIL(int width, bool sgn, const dict<RTLIL::SigBit, RTLIL::SigBit> *new_subst_ptr)
{
	const dict<RTLIL::SigBit, RTLIL::SigBit> *backup_subst_ptr = genRTLIL_subst_ptr;

	if (new_subst_ptr)
		genRTLIL_subst_ptr = new_subst_ptr;

	bool sign_hint = sgn;
	int width_hint = width;
	detectSignWidthWorker(width_hint, sign_hint);
	RTLIL::SigSpec sig = genRTLIL(width_hint, sign_hint);

	genRTLIL_subst_ptr = backup_subst_ptr;

	if (width >= 0)
		sig.extend_u0(width, is_signed);

	return sig;
}

YOSYS_NAMESPACE_END
