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

#include "ast.h"
#include "simplify_package.h"
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

// ---------------------------------------------------------------------------
// View methods for the per-type branches of AstNode::simplify(). These live
// here (not in ast_typed.h) because they touch simplifier statics like
// `current_scope` and `current_ast_mod`. They're listed in roughly the same
// order as their original branches in simplify() below for ease of cross-ref.
// ---------------------------------------------------------------------------

void AstRepeat::unroll(int stage) const
{
	auto cnt = count().take();
	auto body_node = body().take();

	// eval count expression
	while (cnt->simplify(true, stage, 32, true)) { }

	if (cnt->type != AST_CONSTANT)
		node->input_error("Repeat loops outside must have constant repeat counts!\n");

	// convert to a block with the body repeated n times
	std::vector<std::unique_ptr<AstNode>> new_body;
	for (int i = 0; i < cnt->bitsAsConst().as_int(); i++)
		new_body.push_back(body_node->clone());
	reshape_as_vec<AST_BLOCK>(node, std::move(new_body));
}

// Splice an already-simplified GENBLOCK's contents into current_ast_mod, with
// optional name-prefix expansion. Used by AstGenBlock/AstGenIf/AstGenCase.
static void splice_genblock_into_module(AstNode *blk, bool const_fold, int stage)
{
	if (!blk->str.empty())
		blk->expand_genblock(blk->str + ".");
	for (size_t i = 0; i < blk->children.size(); i++) {
		blk->children[i]->simplify(const_fold, stage, -1, false);
		current_ast_mod->children.push_back(std::move(blk->children[i]));
	}
	blk->children.clear();
}

void AstGenBlock::elaborate(bool const_fold, int stage) const
{
	splice_genblock_into_module(node, const_fold, stage);
}

void AstGenIf::elaborate(int stage, int width_hint, bool sign_hint, bool const_fold) const
{
	auto buf = cond()->clone();
	while (buf->simplify(true, stage, width_hint, sign_hint)) { }
	auto buf_const = AstConstant::cast(buf.get());
	if (!buf_const)
		node->input_error("Condition for generate if is not constant!\n");

	if (buf_const->asBool() != 0) {
		buf = then_body()->clone();
	} else {
		buf = has_else_body() ? else_body()->clone() : nullptr;
	}

	if (buf) {
		if (buf->type != AST_GENBLOCK)
			buf = std::make_unique<AstNode>(node->location, AST_GENBLOCK, std::move(buf));
		splice_genblock_into_module(buf.get(), const_fold, stage);
	}

	node->delete_children();
}

void AstGenCase::elaborate(int stage, int width_hint, bool sign_hint, bool const_fold) const
{
	auto buf = selector()->clone();
	while (buf->simplify(true, stage, width_hint, sign_hint)) { }
	if (buf->type != AST_CONSTANT)
		node->input_error("Condition for generate case is not constant!\n");

	bool ref_signed = buf->is_signed;
	RTLIL::Const ref_value = buf->bitsAsConst();

	AstNode *selected_case = nullptr;
	for (auto it = conditions_begin(); it != conditions_end(); ++it) {
		AstAnyCond cond_view((*it).get());
		AstNode *this_genblock = nullptr;
		for (auto& child : (*it)->children) {
			log_assert(this_genblock == nullptr);
			if (child->type == AST_GENBLOCK)
				this_genblock = child.get();
		}

		bool matched_here = false;
		for (auto& child : (*it)->children) {
			if (child->type == AST_DEFAULT) {
				if (selected_case == nullptr)
					selected_case = this_genblock;
				continue;
			}
			if (child->type == AST_GENBLOCK)
				continue;

			buf = child->clone();
			buf->set_in_param_flag(true);
			while (buf->simplify(true, stage, width_hint, sign_hint)) { }
			if (buf->type != AST_CONSTANT)
				node->input_error("Expression in generate case is not constant!\n");

			bool is_selected = RTLIL::const_eq(ref_value, buf->bitsAsConst(),
					ref_signed && buf->is_signed,
					ref_signed && buf->is_signed, 1).as_bool();

			if (is_selected) {
				selected_case = this_genblock;
				matched_here = true;
				break;
			}
		}
		if (matched_here)
			break;
	}

	if (selected_case != nullptr) {
		log_assert(selected_case->type == AST_GENBLOCK);
		auto blk = selected_case->clone();
		splice_genblock_into_module(blk.get(), const_fold, stage);
	}

	node->delete_children();
}

std::unique_ptr<AstNode> AstCellArray::unroll()
{
	AstNode *range_c = range().get();
	AstNode *cell_c = cell().get();
	if (!range_c->range_valid)
		node->input_error("Non-constant array range on cell array.\n");

	int num = max(range_c->range_left, range_c->range_right)
			- min(range_c->range_left, range_c->range_right) + 1;

	if (cell_c->type == AST_PRIMITIVE) {
		// Move the range to the AST_PRIMITIVE node so it's handled later as a
		// single primitive with array semantics.
		auto result = cell().take();
		result->range_left = range_c->range_left;
		result->range_right = range_c->range_right;
		result->range_valid = true;
		return result;
	}

	auto result = std::make_unique<AstNode>(node->location, AST_GENBLOCK);
	for (int i = 0; i < num; i++) {
		int idx = range_c->range_left > range_c->range_right
				? range_c->range_right + i : range_c->range_right - i;
		auto new_cell_owned = cell_c->clone();
		auto *new_cell = new_cell_owned.get();
		result->children.push_back(std::move(new_cell_owned));
		new_cell->str += stringf("[%d]", idx);

		AstNode *ct = AstCell(new_cell).celltype();
		log_assert(ct->type == AST_CELLTYPE);
		ct->str = stringf("$array:%d:%d:%s", i, num, ct->str);
	}
	return result;
}

std::unique_ptr<AstNode> AstPrimitive::lower()
{
	if (num_args() < 2)
		node->input_error("Insufficient number of arguments for primitive `%s'!\n", node->str);

	std::vector<std::unique_ptr<AstNode>> children_list;
	for (auto& child : *this) {
		AstArgument arg(child.get());
		log_assert(arg.has_expr());
		children_list.push_back(arg.expr_slot()->take());
	}
	node->children.clear();

	// TODO handle bit-widths of primitives and support cell arrays for more primitives

	if (node->range_valid && node->str != "tran")
		node->input_error("Cell arrays of primitives are currently not supported.\n");

	const auto &name = node->str;
	const auto &loc = node->location;

	if (name == "bufif0" || name == "bufif1" || name == "notif0" || name == "notif1") {
		if (children_list.size() != 3)
			node->input_error("Invalid number of arguments for primitive `%s'!\n", node->str);

		std::vector<RTLIL::State> z_const(1, RTLIL::State::Sz);

		auto& mux_input = children_list.at(1);
		if (name == "notif0" || name == "notif1")
			mux_input = std::make_unique<AstNode>(loc, AST_BIT_NOT, std::move(mux_input));

		auto tern = std::make_unique<AstNode>(loc, AST_TERNARY, std::move(children_list.at(2)));
		if (name == "bufif0") {
			tern->children.push_back(AstNode::mkconst_bits(loc, z_const, false));
			tern->children.push_back(std::move(mux_input));
		} else {
			tern->children.push_back(std::move(mux_input));
			tern->children.push_back(AstNode::mkconst_bits(loc, z_const, false));
		}

		node->str.clear();
		auto lhs_c = std::move(children_list.at(0));
		lhs_c->was_checked = true;
		reshape_as<AST_ASSIGN>(node, std::move(lhs_c), std::move(tern));
		node->fixup_hierarchy_flags();
		return nullptr;
	}

	if (name == "buf" || name == "not" || name == "tran") {
		auto& input = children_list.back();
		if (name == "not")
			input = std::make_unique<AstNode>(loc, AST_BIT_NOT, std::move(input));

		auto block = std::make_unique<AstNode>(loc, AST_GENBLOCK);
		for (auto it = children_list.begin(); it != std::prev(children_list.end()); ++it) {
			block->children.push_back(std::make_unique<AstNode>(loc, AST_ASSIGN, std::move(*it), input->clone()));
			block->children.back()->was_checked = true;
		}
		return block;
	}

	AstNodeType op_type = AST_NONE;
	bool invert_results = false;

	if (name == "and") op_type = AST_BIT_AND;
	if (name == "nand") op_type = AST_BIT_AND, invert_results = true;
	if (name == "or") op_type = AST_BIT_OR;
	if (name == "nor") op_type = AST_BIT_OR, invert_results = true;
	if (name == "xor") op_type = AST_BIT_XOR;
	if (name == "xnor") op_type = AST_BIT_XOR, invert_results = true;
	log_assert(op_type != AST_NONE);

	auto& acc = children_list[1];
	if (op_type != AST_POS)
		for (size_t i = 2; i < children_list.size(); i++) {
			acc = std::make_unique<AstNode>(loc, op_type, std::move(acc), std::move(children_list[i]));
			acc->location = loc;
		}
	if (invert_results)
		acc = std::make_unique<AstNode>(loc, AST_BIT_NOT, std::move(acc));

	node->str.clear();
	auto lhs_c = std::move(children_list[0]);
	lhs_c->was_checked = true;
	reshape_as<AST_ASSIGN>(node, std::move(lhs_c), std::move(acc));
	node->fixup_hierarchy_flags();
	return nullptr;
}

void AstPackage::register_scope() const
{
	// Parameters, typedefs, and subroutines defined at package scope are
	// reachable as `pkg::name`. The simplifier flattens that here by entering
	// each declaration's name into `current_scope` so AST_IDENTIFIER lookups
	// during package-body simplification (and later imports) resolve.
	for (auto& child : node->children) {
		if (ChildConstraint<AST_PARAMETER, AST_LOCALPARAM, AST_TYPEDEF,
				AST_FUNCTION, AST_TASK>::accepts(child.get())) {
			current_scope[child->str] = child.get();
		}
		if (auto en = AstEnum::cast(child.get())) {
			current_scope[en->str()] = en->raw();
			for (auto& enode : en->raw()->children) {
				log_assert(AstEnumItem::matches(enode.get()));
				if (current_scope.count(enode->str) == 0)
					current_scope[enode->str] = enode.get();
				else
					child->input_error("enum item %s already exists in package\n", enode->str);
			}
		}
	}
}


// convert the AST into a simpler AST that has all parameters substituted by their
// values, unrolled for-loops, expanded generate blocks, etc. when this function
// is done with an AST it can be converted into RTLIL using genRTLIL().
//
// this function also does all name resolving and sets the id2ast member of all
// nodes that link to a different node using names and lexical scoping.
bool AstNode::simplify(bool const_fold, int stage, int width_hint, bool sign_hint)
{
	static int recursion_counter = 0;
	static bool deep_recursion_warning = false;

	if (recursion_counter++ == 1000 && deep_recursion_warning) {
		log_warning("Deep recursion in AST simplifier.\nDoes this design contain overly long or deeply nested expressions, or excessive recursion?\n");
		deep_recursion_warning = false;
	}

	static bool unevaluated_tern_branch = false;

	std::unique_ptr<AstNode> newNode = nullptr;
	bool did_something = false;

#if 0
	log("-------------\n");
	log("AST simplify[%d] depth %d at %s:%d on %s %p:\n", stage, recursion_counter, location.begin.filename, location.begin.line, type2str(type), this);
	log("const_fold=%d, stage=%d, width_hint=%d, sign_hint=%d\n",
			int(const_fold), int(stage), int(width_hint), int(sign_hint));
	// dumpAst(nullptr, "> ");
#endif

	if (stage == 0)
	{
		log_assert(ModuleLike::accepts(this));

		deep_recursion_warning = true;
		while (simplify(const_fold, 1, width_hint, sign_hint)) { }

		if (!flag_nomem2reg && !get_bool_attribute(ID::nomem2reg))
		{
			dict<AstNode*, pool<std::string>> mem2reg_places;
			dict<AstNode*, uint32_t> mem2reg_candidates, dummy_proc_flags;
			uint32_t flags = flag_mem2reg ? AstNode::MEM2REG_FL_ALL : 0;
			mem2reg_as_needed_pass1(mem2reg_places, mem2reg_candidates, dummy_proc_flags, flags);

			pool<AstNode*> mem2reg_set;
			for (auto &it : mem2reg_candidates)
			{
				AstNode *mem = it.first;
				uint32_t memflags = it.second;
				bool this_nomeminit = flag_nomeminit;
				log_assert((memflags & ~0x00ffff00) == 0);

				if (mem->get_bool_attribute(ID::nomem2reg))
					continue;

				if (mem->get_bool_attribute(ID::nomeminit) || get_bool_attribute(ID::nomeminit))
					this_nomeminit = true;

				if (memflags & AstNode::MEM2REG_FL_FORCED)
					goto silent_activate;

				if (memflags & AstNode::MEM2REG_FL_EQ2)
					goto verbose_activate;

				if (memflags & AstNode::MEM2REG_FL_SET_ASYNC)
					goto verbose_activate;

				if ((memflags & AstNode::MEM2REG_FL_SET_INIT) && (memflags & AstNode::MEM2REG_FL_SET_ELSE) && this_nomeminit)
					goto verbose_activate;

				if (memflags & AstNode::MEM2REG_FL_CMPLX_LHS)
					goto verbose_activate;

				if ((memflags & AstNode::MEM2REG_FL_CONST_LHS) && !(memflags & AstNode::MEM2REG_FL_VAR_LHS))
					goto verbose_activate;

				// log("Note: Not replacing memory %s with list of registers (flags=0x%08lx).\n", mem->str, long(memflags));
				continue;

			verbose_activate:
				if (mem2reg_set.count(mem) == 0) {
					std::string message = stringf("Replacing memory %s with list of registers.", mem->str);
					bool first_element = true;
					for (auto &place : mem2reg_places[it.first]) {
						message += stringf("%s%s", first_element ? " See " : ", ", place);
						first_element = false;
					}
					log_warning("%s\n", message);
				}

			silent_activate:
				// log("Note: Replacing memory %s with list of registers (flags=0x%08lx).\n", mem->str, long(memflags));
				mem2reg_set.insert(mem);
			}

			for (auto node : mem2reg_set)
			{
				int mem_width, mem_size, addr_bits;
				node->meminfo(mem_width, mem_size, addr_bits);

				AstNode *data_range = AstMemory(node).data_range();
				int data_range_left = data_range->range_left;
				int data_range_right = data_range->range_right;

				if (data_range->range_swapped)
					std::swap(data_range_left, data_range_right);

				auto loc = node->location;
				for (int i = 0; i < mem_size; i++) {
					auto reg = std::make_unique<AstNode>(loc, AST_WIRE, std::make_unique<AstNode>(loc, AST_RANGE,
							mkconst_int(loc, data_range_left, true), mkconst_int(loc, data_range_right, true)));
					reg->str = stringf("%s[%d]", node->str, i);
					reg->is_reg = true;
					reg->is_signed = node->is_signed;
					for (auto &it : node->attributes)
						if (it.first != ID::mem2reg)
							reg->set_attribute(it.first, it.second->clone());
					reg->location.begin.filename = node->location.begin.filename;
					reg->location = node->location;
					while (reg->simplify()) { }
					children.push_back(std::move(reg));
				}
			}

			AstNode* async_block = nullptr;
			while (mem2reg_as_needed_pass2(mem2reg_set, this, nullptr, async_block)) { }

			mem2reg_remove(mem2reg_set);
		}

		while (simplify(const_fold, 2, width_hint, sign_hint)) { }
		recursion_counter--;
		return false;
	}

	// we do not look inside a task or function
	// (but as soon as a task or function is instantiated we process the generated AST as usual)
	if (FunctionTaskLike::accepts(this)) {
		recursion_counter--;
		return false;
	}

	// deactivate all calls to non-synthesis system tasks
	// note that $display, $finish, and $stop are used for synthesis-time DRC so they're not in this list
	if (CallLike::accepts(this) && (str == "$strobe" || str == "$monitor" || str == "$time" ||
			str == "$dumpfile" || str == "$dumpvars" || str == "$dumpon" || str == "$dumpoff" || str == "$dumpall")) {
		log_file_warning(*location.begin.filename, location.begin.line, "Ignoring call to system %s %s.\n", AstFcall::matches(this) ? "function" : "task", str);
		delete_children();
		str = std::string();
	}

	if (AstTcall::matches(this) &&
		(str == "$display" || str == "$displayb" || str == "$displayh" || str == "$displayo" ||
		 str == "$write"   || str == "$writeb"   || str == "$writeh"   || str == "$writeo"))
	{
		if (!current_always) {
			log_file_warning(*location.begin.filename, location.begin.line, "System task `%s' outside initial or always block is unsupported.\n", str);
			delete_children();
			str = std::string();
		} else {
			// simplify the expressions and convert them to a special cell later in genrtlil
			for (auto& node : children)
				while (node->simplify(true, stage, -1, false)) {}

			if (AstInitial::matches(current_always) && !flag_nodisplay && stage == 2) {
				int default_base = 10;
				if (str.back() == 'b')
					default_base = 2;
				else if (str.back() == 'o')
					default_base = 8;
				else if (str.back() == 'h')
					default_base = 16;

				// when $display()/$write() functions are used in an initial block, print them during synthesis
				Fmt fmt = processFormat(stage, /*sformat_like=*/false, default_base, /*first_arg_at=*/0, /*may_fail=*/true);
				if (str.substr(0, 8) == "$display")
					fmt.append_literal("\n");
				log("%s", fmt.render());
			}

			return false;
		}
	}

	// activate const folding if this is anything that must be evaluated statically (ranges, parameters, attributes, etc.)
	if (ConstFoldActivator::accepts(this))
		const_fold = true;
	if (AstIdentifier::matches(this) && current_scope.count(str) > 0 && AstAnyParamLike::matches(current_scope[str]))
		const_fold = true;

	std::map<std::string, AstNode*> backup_scope;

	// create name resolution entries for all objects with names
	// also merge multiple declarations for the same wire (e.g. "output foobar; reg foobar;")
	if (ModuleLike::accepts(this)) {
		current_scope.clear();
		std::set<std::string> existing;
		int counter = 0;
		label_genblks(existing, counter);
		std::map<std::string, AstNode*> this_wire_scope;

		// Process package imports after clearing the scope but before processing module declarations
		for (size_t i = 0; i < children.size(); i++) {
			AstNode *child = children[i].get();
			if (child->type == AST_IMPORT) {
				// Find the package in the design
				AstNode *package_node = nullptr;

				// First look in current_ast->children (for packages in same file)
				if (current_ast != nullptr) {
					for (auto &design_child : current_ast->children) {
						if (design_child->type == AST_PACKAGE) {
							if (design_child->str == child->str) {
								package_node = design_child.get();
								break;
							}
						}
					}
				}

				// If not found, look in design->verilog_packages (for packages from other files)
				if (!package_node && simplify_design_context != nullptr) {
					for (auto &design_package : simplify_design_context->verilog_packages) {
						// Handle both with and without leading backslash
						std::string package_name = design_package->str;
						if (package_name[0] == '\\') {
							package_name = package_name.substr(1);
						}
						if (package_name == child->str || design_package->str == child->str) {
							package_node = design_package.get();
							break;
						}
					}
				}

				if (package_node) {
					PackageImporter importer(this, child);
					// Import names from the package into current scope
					for (auto& pkg_child : package_node->children) {
						importer.import(current_scope, pkg_child.get());
					}
					// Remove the import node since it's been processed
					children.erase(children.begin() + i);
					i--; // Adjust index since we removed an element
				} else {
					// If we can't find the package, just remove the import node to avoid errors later
					log_warning("Package `%s' not found for import, removing import statement\n", child->str);
					children.erase(children.begin() + i);
					i--; // Adjust index since we removed an element
				}
			}
		}
		for (size_t i = 0; i < children.size(); i++) {
			AstNode* node = children[i].get();

			if (node->type == AST_WIRE) {
				if (node->children.size() == 1 && node->children[0]->type == AST_RANGE) {
					for (auto& c : node->children[0]->children) {
						if (!c->is_simple_const_expr())
							set_attribute(ID::dynports, AstNode::mkconst_int(c->location, 1, true));
					}
				}
				if (this_wire_scope.count(node->str) > 0) {
					AstNode *first_node = this_wire_scope[node->str];
					if (first_node->is_input && node->is_reg)
						goto wires_are_incompatible;
					if (!node->is_input && !node->is_output && node->is_reg && node->children.size() == 0)
						goto wires_are_compatible;
					if (first_node->children.size() == 0 && node->children.size() == 1 && node->children[0]->type == AST_RANGE) {
						AstNode* r = node->children[0].get();
						if (r->range_valid && r->range_left == 0 && r->range_right == 0) {
							node->children.pop_back();
						}
					}
					if (first_node->children.size() != node->children.size())
						goto wires_are_incompatible;
					for (size_t j = 0; j < node->children.size(); j++) {
						auto &n1 = first_node->children[j], &n2 = node->children[j];
						if (n1->type == AST_RANGE && n2->type == AST_RANGE && n1->range_valid && n2->range_valid) {
							if (n1->range_left != n2->range_left)
								goto wires_are_incompatible;
							if (n1->range_right != n2->range_right)
								goto wires_are_incompatible;
						} else if (*n1 != *n2)
							goto wires_are_incompatible;
					}
					if (first_node->range_left != node->range_left)
						goto wires_are_incompatible;
					if (first_node->range_right != node->range_right)
						goto wires_are_incompatible;
					if (first_node->port_id == 0 && (node->is_input || node->is_output))
						goto wires_are_incompatible;
				wires_are_compatible:
					if (node->is_input)
						first_node->is_input = true;
					if (node->is_output)
						first_node->is_output = true;
					if (node->is_reg)
						first_node->is_reg = true;
					if (node->is_logic)
						first_node->is_logic = true;
					if (node->is_signed)
						first_node->is_signed = true;
					for (auto &it : node->attributes) {
						first_node->set_attribute(it.first, it.second->clone());
					}
					children.erase(children.begin()+(i--));
					did_something = true;
					continue;
				wires_are_incompatible:
					if (stage > 1)
						input_error("Incompatible re-declaration of wire %s.\n", node->str);
					continue;
				}
				this_wire_scope[node->str] = node;
			}
			// these nodes appear at the top level in a module and can define names
			if (ChildConstraint<AST_PARAMETER, AST_LOCALPARAM, AST_WIRE, AST_AUTOWIRE, AST_GENVAR,
					AST_MEMORY, AST_FUNCTION, AST_TASK, AST_DPI_FUNCTION, AST_CELL,
					AST_TYPEDEF>::accepts(node)) {
				backup_scope[node->str] = current_scope[node->str];
				current_scope[node->str] = node;
			}
			if (node->type == AST_ENUM) {
				current_scope[node->str] = node;
				for (auto& enode : node->children) {
					log_assert(enode->type==AST_ENUM_ITEM);
					if (current_scope.count(enode->str) == 0)
						current_scope[enode->str] = enode.get();
					else
						input_error("enum item %s already exists\n", enode->str);
				}
			}
		}
		for (size_t i = 0; i < children.size(); i++) {
			auto& node = children[i];
			if (ChildConstraint<AST_PARAMETER, AST_LOCALPARAM, AST_WIRE, AST_AUTOWIRE, AST_MEMORY, AST_TYPEDEF>::accepts(node.get()))
				while (node->simplify())
					did_something = true;
			if (node->type == AST_ENUM) {
				for (auto& enode : node->children){
					log_assert(enode->type==AST_ENUM_ITEM);
					while (node->simplify())
						did_something = true;
				}
			}
		}

		for (auto& child : children)
			if (child->type == AST_ALWAYS &&
					child->attributes.count(ID::always_comb))
				check_auto_nosync(child.get());
	}

	// create name resolution entries for all objects with names
	if (auto pkg = AstPackage::cast(this))
		pkg->register_scope();


	auto backup_current_block = current_block;
	auto backup_current_block_child = current_block_child;
	auto backup_current_top_block = current_top_block;
	auto backup_current_always = current_always;
	auto backup_current_always_clocked = current_always_clocked;

	if (ProceduralBlockLike::accepts(this))
	{
		if (current_always != nullptr)
			input_error("Invalid nesting of always blocks and/or initializations.\n");

		current_always = this;
		current_always_clocked = false;

		if (AstAlways::matches(this))
			for (auto& child : children) {
				if (ClockedEdgeLike::accepts(child.get()))
					current_always_clocked = true;
				if (child->type == AST_EDGE && GetSize(child->children) == 1 &&
						child->children[0]->type == AST_IDENTIFIER && child->children[0]->str == "\\$global_clock")
					current_always_clocked = true;
			}
	}

	if (AstCell::matches(this)) {
		bool lookup_suggested = false;

		for (auto& child : children) {
			// simplify any parameters to constants
			if (child->type == AST_PARASET)
				while (child->simplify()) { }

			// look for patterns which _may_ indicate ambiguity requiring
			// resolution of the underlying module
			if (child->type == AST_ARGUMENT) {
				if (child->children.size() != 1)
					continue;
				const auto& value = child->children[0];
				if (value->type == AST_IDENTIFIER) {
					const AstNode *elem = value->id2ast;
					if (elem == nullptr) {
						if (current_scope.count(value->str))
							elem = current_scope.at(value->str);
						else
							continue;
					}
					if (elem->type == AST_MEMORY)
						// need to determine is the is a read or wire
						lookup_suggested = true;
					else if (elem->type == AST_WIRE && elem->is_signed && !value->children.empty())
						// this may be a fully sliced signed wire which needs
						// to be indirected to produce an unsigned connection
						lookup_suggested = true;
				}
				else if (contains_unbased_unsized(value.get()))
					// unbased unsized literals extend to width of the context
					lookup_suggested = true;
			}
		}

		const RTLIL::Module *module = nullptr;
		if (lookup_suggested)
			module = lookup_cell_module();
		if (module) {
			size_t port_counter = 0;
			for (auto& child : children) {
				if (child->type != AST_ARGUMENT)
					continue;

				// determine the full name of port this argument is connected to
				RTLIL::IdString port_name;
				if (child->str.size())
					port_name = child->str;
				else {
					if (port_counter >= module->ports.size())
						input_error("Cell instance has more ports than the module!\n");
					port_name = module->ports[port_counter++];
				}

				// find the port's wire in the underlying module
				const RTLIL::Wire *ref = module->wire(port_name);
				if (ref == nullptr)
					input_error("Cell instance refers to port %s which does not exist in module %s!.\n",
							log_id(port_name), log_id(module->name));

				// select the argument, if present
				log_assert(child->children.size() <= 1);
				if (child->children.empty())
					continue;

				{
					auto arg_check = child->children[0].get();

					// plain identifiers never need indirection; this also prevents
					// adding infinite levels of indirection
					if (arg_check->type == AST_IDENTIFIER && arg_check->children.empty())
						continue;

					// only add indirection for standard inputs or outputs
					if (ref->port_input == ref->port_output)
						continue;
				}

				auto arg = std::move(child->children[0]);
				did_something = true;

				// create the indirection wire
				std::stringstream sstr;
				sstr << "$indirect$" << ref->name.c_str() << "$" << RTLIL::encode_filename(*location.begin.filename) << ":" << location.begin.line << "$" << (autoidx++);
				std::string tmp_str = sstr.str();
				add_wire_for_ref(location, ref, tmp_str);

				auto asgn_owned = std::make_unique<AstNode>(child->location, AST_ASSIGN);
				auto* asgn = asgn_owned.get();
				current_ast_mod->children.push_back(std::move(asgn_owned));

				auto ident = std::make_unique<AstNode>(child->location, AST_IDENTIFIER);
				ident->str = tmp_str;
				child->children[0] = ident->clone();

				if (ref->port_input && !ref->port_output) {
					asgn->children.push_back(std::move(ident));
					asgn->children.push_back(std::move(arg));
				} else {
					log_assert(!ref->port_input && ref->port_output);
					asgn->children.push_back(std::move(arg));
					asgn->children.push_back(std::move(ident));
				}
				asgn->fixup_hierarchy_flags();
			}


		}
	}

	int backup_width_hint = width_hint;
	bool backup_sign_hint = sign_hint;

	bool detect_width_simple = false;
	bool child_0_is_self_determined = false;
	bool child_1_is_self_determined = false;
	bool child_2_is_self_determined = false;
	bool children_are_self_determined = false;
	bool reset_width_after_children = false;

	switch (type)
	{
	case AST_ASSIGN_EQ:
	case AST_ASSIGN_LE:
	case AST_ASSIGN: {
		// All three share the 2-child [lhs, rhs] shape (grammar invariant §15).
		// Slot accessors here would force a single concrete view tag; we pick lhs/rhs
		// by position because the same code path serves all three assignment kinds.
		AstNode *lhs = children[0].get();
		AstNode *rhs = children[1].get();
		while (!lhs->basic_prep && lhs->simplify(false, stage, -1, false) == true)
			did_something = true;
		while (!rhs->basic_prep && rhs->simplify(false, stage, -1, false) == true)
			did_something = true;
		lhs->detectSignWidth(backup_width_hint, backup_sign_hint);
		rhs->detectSignWidth(width_hint, sign_hint);
		width_hint = max(width_hint, backup_width_hint);
		child_0_is_self_determined = true;
		// test only once, before optimizations and memory mappings but after assignment LHS was mapped to an identifier
		if (lhs->id2ast && !lhs->was_checked) {
			bool is_blocking = BlockingAssignLike::accepts(this);
			if (is_blocking && lhs->id2ast->is_logic)
				lhs->id2ast->is_reg = true; // if logic type is used in a block asignment
			if (is_blocking && !lhs->id2ast->is_reg)
				log_warning("wire '%s' is assigned in a block at %s.\n", lhs->str, loc_string());
			if (AstAssign::matches(this) && lhs->id2ast->is_reg) {
				bool is_rand_reg = false;
				if (auto rhs_call = AstFcall::cast(rhs)) {
					const auto &name = rhs_call->str();
					is_rand_reg = name == "\\$anyconst" || name == "\\$anyseq" ||
							name == "\\$allconst" || name == "\\$allseq";
				}
				if (!is_rand_reg)
					log_warning("reg '%s' is assigned in a continuous assignment at %s.\n", lhs->str, loc_string());
			}
			lhs->was_checked = true;
		}
		break;
	}

	case AST_STRUCT:
	case AST_UNION:
		if (!basic_prep) {
			for (auto& node : children) {
				// resolve any ranges
				while (!node->basic_prep && node->simplify(true, stage, -1, false)) {
					did_something = true;
				}
			}
			// determine member offsets and widths
			size_packed_struct(this, 0);

			// instance rather than just a type in a typedef or outer struct?
			if (!str.empty() && str[0] == '\\') {
				// instance so add a wire for the packed structure
				auto wnode = make_packed_struct(this, str, attributes);
				log_assert(current_ast_mod);
				current_ast_mod->children.push_back(std::move(wnode));
			}
			basic_prep = true;
			is_custom_type = false;
		}
		break;

	case AST_STRUCT_ITEM:
		if (is_custom_type) {
			log_assert(children.size() >= 1);
			log_assert(AstWiretype::matches(children[0].get()));

			// Pretend it's just a wire in order to resolve the type.
			type = AST_WIRE;
			while (is_custom_type && simplify(const_fold, stage, width_hint, sign_hint)) {};
			if (AstWire::matches(this))
				type = AST_STRUCT_ITEM;

			did_something = true;
		}
		log_assert(!is_custom_type);
		break;

	case AST_ENUM:
		//log("\nENUM %s: %d child %d\n", str, basic_prep, children[0]->basic_prep);
		if (!basic_prep) {
			for (auto& item_node : children) {
				while (!item_node->basic_prep && item_node->simplify(false, stage, -1, false))
					did_something = true;
			}
			// allocate values (called more than once)
			allocateDefaultEnumValues();
		}
		break;

	case AST_PARAMETER:
	case AST_LOCALPARAM:
	case AST_ENUM_ITEM: {
		// All three share the [value, range_or_wiretype_or_realvalue?] shape.
		AstAnyParamLike param(this);
		AstNode *value_n = param.value();
		// if parameter is implicit type which is the typename of a struct or union,
		// save information about struct in wiretype attribute
		if (!AstEnumItem::matches(this) && AstIdentifier::matches(value_n) &&
				current_scope.count(value_n->str) > 0) {
			auto item_node = current_scope[value_n->str];
			if (AstStructLike::matches(item_node)) {
				set_attribute(ID::wiretype, item_node->clone());
				size_packed_struct(attributes[ID::wiretype].get(), 0);
				add_members_to_scope(attributes[ID::wiretype].get(), str);
			}
		}
		while (!value_n->basic_prep && value_n->simplify(false, stage, -1, false))
			did_something = true;
		value_n->detectSignWidth(width_hint, sign_hint);
		// Only the AST_RANGE shape contributes to the parameter width.
		// AST_WIRETYPE and AST_REALVALUE are handled later.
		if (AstNode *range_n = param.range_or_null()) {
			while (!range_n->basic_prep && range_n->simplify(false, stage, -1, false))
				did_something = true;
			if (!range_n->range_valid)
				input_error("Non-constant width range on %s decl.\n",
						AstEnumItem::matches(this) ? "enum item" : "parameter");
			width_hint = max(width_hint, range_n->range_left - range_n->range_right + 1);
		}
		break;
	}

	case AST_CAST_SIZE: {
		AstCastSize cast_view(this);
		AstNode *target = cast_view.target().get();
		if (AstWire::matches(target)) {
			int width = 1;
			std::unique_ptr<AstNode> resolved_size;
			if (target->children.empty()) {
				// Base type (e.g., int)
				width = target->range_left - target->range_right + 1;
				resolved_size = mkconst_int(target->location, width, target->is_signed);
			} else {
				// User defined type
				log_assert(AstWiretype::matches(target->children[0].get()));
				const std::string &type_name = target->children[0]->str;
				if (!current_scope.count(type_name))
					input_error("Unknown identifier `%s' used as type name\n", type_name);
				AstNode *resolved_type_node = current_scope.at(type_name);
				auto td_view = AstTypedef::cast(resolved_type_node);
				if (!td_view)
					input_error("`%s' does not name a type\n", type_name);
				AstNode *template_node = td_view->underlying().get();

				// Ensure typedef itself is fully simplified
				while (template_node->simplify(const_fold, stage, width_hint, sign_hint)) {};

				switch (template_node->type) {
				case AST_WIRE:
					if (!template_node->children.empty() && template_node->children[0]->type == AST_RANGE)
						width = range_width(this, template_node->children[0].get());
					target->delete_children();
					resolved_size = mkconst_int(target->location, width, true);
					break;
				case AST_STRUCT:
				case AST_UNION:
					target->delete_children();
					width = size_packed_struct(template_node, 0);
					resolved_size = mkconst_int(target->location, width, false);
					break;
				default:
					log_error("Don't know how to translate static cast of type %s\n", type2str(template_node->type));
				}
			}
			cast_view.target().set(std::move(resolved_size));
		}

		detect_width_simple = true;
		children_are_self_determined = true;
		break;
	}

	case AST_TO_BITS:
	case AST_TO_SIGNED:
	case AST_TO_UNSIGNED:
	case AST_SELFSZ:
	case AST_CONCAT:
	case AST_REPLICATE:
	case AST_REDUCE_AND:
	case AST_REDUCE_OR:
	case AST_REDUCE_XOR:
	case AST_REDUCE_XNOR:
	case AST_REDUCE_BOOL:
		detect_width_simple = true;
		children_are_self_determined = true;
		break;

	case AST_NEG:
	case AST_BIT_NOT:
	case AST_POS:
	case AST_BIT_AND:
	case AST_BIT_OR:
	case AST_BIT_XOR:
	case AST_BIT_XNOR:
	case AST_ADD:
	case AST_SUB:
	case AST_MUL:
	case AST_DIV:
	case AST_MOD:
		detect_width_simple = true;
		break;

	case AST_SHIFT_LEFT:
	case AST_SHIFT_RIGHT:
	case AST_SHIFT_SLEFT:
	case AST_SHIFT_SRIGHT:
	case AST_POW:
		detect_width_simple = true;
		child_1_is_self_determined = true;
		break;

	case AST_LT:
	case AST_LE:
	case AST_EQ:
	case AST_NE:
	case AST_EQX:
	case AST_NEX:
	case AST_GE:
	case AST_GT:
		width_hint = -1;
		sign_hint = true;
		for (auto& child : children) {
			while (!child->basic_prep && child->simplify(false, stage, -1, false) == true)
				did_something = true;
			child->detectSignWidthWorker(width_hint, sign_hint);
		}
		reset_width_after_children = true;
		break;

	case AST_LOGIC_AND:
	case AST_LOGIC_OR:
	case AST_LOGIC_NOT:
		detect_width_simple = true;
		children_are_self_determined = true;
		break;

	case AST_TERNARY:
		child_0_is_self_determined = true;
		break;

	case AST_MEMRD:
		detect_width_simple = true;
		children_are_self_determined = true;
		break;

	case AST_FCALL:
	case AST_TCALL:
		children_are_self_determined = true;
		break;

	default:
		width_hint = -1;
		sign_hint = false;
	}

	if (detect_width_simple && width_hint < 0) {
		if (AstReplicate::matches(this))
			while (children[0]->simplify(true, stage, -1, false) == true)
				did_something = true;
		for (auto& child : children)
			while (!child->basic_prep && child->simplify(false, stage, -1, false) == true)
				did_something = true;
		detectSignWidth(width_hint, sign_hint);
	}

	if (AstFcall::matches(this) && str == "\\$past")
		detectSignWidth(width_hint, sign_hint);

	if (auto tern = AstTernary::cast(this)) {
		AstNode *cond_c = tern->cond().get();
		AstNode *then_c = tern->then_().get();
		AstNode *else_c = tern->else_().get();
		if (width_hint < 0) {
			while (!cond_c->basic_prep && cond_c->simplify(true, stage, -1, false))
				did_something = true;

			bool backup_unevaluated_tern_branch = unevaluated_tern_branch;
			AstNode *chosen = get_tern_choice().first;

			unevaluated_tern_branch = backup_unevaluated_tern_branch || chosen == else_c;
			while (!then_c->basic_prep && then_c->simplify(false, stage, -1, false))
				did_something = true;

			unevaluated_tern_branch = backup_unevaluated_tern_branch || chosen == then_c;
			while (!else_c->basic_prep && else_c->simplify(false, stage, -1, false))
				did_something = true;

			unevaluated_tern_branch = backup_unevaluated_tern_branch;
			detectSignWidth(width_hint, sign_hint);
		}
		int width_hint_left, width_hint_right;
		bool sign_hint_left, sign_hint_right;
		bool found_real_left, found_real_right;
		then_c->detectSignWidth(width_hint_left, sign_hint_left, &found_real_left);
		else_c->detectSignWidth(width_hint_right, sign_hint_right, &found_real_right);
		if (found_real_left || found_real_right) {
			child_1_is_self_determined = true;
			child_2_is_self_determined = true;
		}
	}

	if (AstCondX::matches(this) && children.size() > 0 && children.at(0)->type == AST_CONSTANT) {
		for (auto &bit : children.at(0)->bits)
			if (bit == State::Sz || bit == State::Sx)
				bit = State::Sa;
	}

	if (AstCondZ::matches(this) && children.size() > 0 && children.at(0)->type == AST_CONSTANT) {
		for (auto &bit : children.at(0)->bits)
			if (bit == State::Sz)
				bit = State::Sa;
	}

	if (auto case_opt = AstCase::cast(this); const_fold && case_opt)
	{
		AstCase case_ = *case_opt;
		detectSignWidth(width_hint, sign_hint);
		AstNode *selector = case_.selector();
		while (selector->simplify(const_fold, stage, width_hint, sign_hint)) { }
		if (selector->type == AST_CONSTANT && selector->bits_only_01()) {
			selector->is_signed = sign_hint;
			RTLIL::Const case_expr = selector->bitsAsConst(width_hint, sign_hint);
			std::vector<std::unique_ptr<AstNode>> new_children;
			new_children.push_back(std::move(children[0]));
			for (int i = 1; i < GetSize(children); i++) {
				auto& child = children[i];
				log_assert(AstAnyCond::matches(child.get()));
				for (auto& v : child->children) {
					if (v->type == AST_DEFAULT)
						goto keep_const_cond;
					if (v->type == AST_BLOCK)
						continue;
					while (v->simplify(const_fold, stage, width_hint, sign_hint)) { }
					if (v->type == AST_CONSTANT && v->bits_only_01()) {
						RTLIL::Const case_item_expr = v->bitsAsConst(width_hint, sign_hint);
						RTLIL::Const match = const_eq(case_expr, case_item_expr, sign_hint, sign_hint, 1);
						log_assert(match.size() == 1);
						if (match.front() == RTLIL::State::S1) {
							// This is the only reachable case. Skip to the end
							i = GetSize(children);
							goto keep_const_cond;
						}
						continue;
					}
					goto keep_const_cond;
				}
				if (0)
			keep_const_cond:
					new_children.push_back(std::move(child));
			}
			new_children.swap(children);
		}
	}

	dict<std::string, pool<int>> backup_memwr_visible;
	dict<std::string, pool<int>> final_memwr_visible;

	if (AstCase::matches(this) && stage == 2) {
		backup_memwr_visible = current_memwr_visible;
		final_memwr_visible = current_memwr_visible;
	}

	// simplify all children first
	// (iterate by index as e.g. auto wires can add new children in the process)
	for (size_t i = 0; i < children.size(); i++) {
		bool did_something_here = true;
		bool backup_flag_autowire = flag_autowire;
		bool backup_unevaluated_tern_branch = unevaluated_tern_branch;
		if (ForLike::accepts(this) && i >= 3)
			break;
		if (GenCondLike::accepts(this) && i >= 1)
			break;
		if (AstGenBlock::matches(this))
			break;
		if (AstCellArray::matches(this) && ChildConstraint<AST_CELL, AST_PRIMITIVE>::accepts(children[i].get()))
			continue;
		if (AstBlock::matches(this) && !str.empty())
			break;
		if (AstPrefix::matches(this) && i >= 1)
			break;
		if (AstDefparam::matches(this) && i == 0)
			flag_autowire = true;
		if (AstTernary::matches(this) && i > 0 && !unevaluated_tern_branch) {
			AstNode *chosen = get_tern_choice().first;
			unevaluated_tern_branch = chosen && chosen != children[i].get();
		}
		while (did_something_here && i < children.size()) {
			bool const_fold_here = const_fold;
			int width_hint_here = width_hint;
			bool sign_hint_here = sign_hint;
			if (i == 0 && ChildConstraint<AST_REPLICATE, AST_WIRE>::accepts(this))
				const_fold_here = true;
			if (ParameterLike::accepts(this))
				const_fold_here = true;
			if (AstBlock::matches(this)) {
				current_block = this;
				current_block_child = children[i].get();
			}
			if (ProceduralBlockLike::accepts(this) && AstBlock::matches(children[i].get()))
				current_top_block = children[i].get();
			if (i == 0 && child_0_is_self_determined)
				width_hint_here = -1, sign_hint_here = false;
			if (i == 1 && child_1_is_self_determined)
				width_hint_here = -1, sign_hint_here = false;
			if (i == 2 && child_2_is_self_determined)
				width_hint_here = -1, sign_hint_here = false;
			if (children_are_self_determined)
				width_hint_here = -1, sign_hint_here = false;
			did_something_here = children[i]->simplify(const_fold_here, stage, width_hint_here, sign_hint_here);
			if (did_something_here)
				did_something = true;
		}
		if (stage == 2 && AstInitial::matches(children[i].get()) && current_ast_mod != this) {
			current_ast_mod->children.push_back(std::move(children[i]));
			children.erase(children.begin() + (i--));
			did_something = true;
		}
		flag_autowire = backup_flag_autowire;
		unevaluated_tern_branch = backup_unevaluated_tern_branch;
		if (stage == 2 && AstCase::matches(this)) {
			for (auto &x : current_memwr_visible) {
				for (int y : x.second)
					final_memwr_visible[x.first].insert(y);
			}
			current_memwr_visible = backup_memwr_visible;
		}
	}
	for (auto &attr : attributes) {
		while (attr.second->simplify(true, stage, -1, false))
			did_something = true;
	}
	if (AstCase::matches(this) && stage == 2) {
		current_memwr_visible = final_memwr_visible;
	}
	if (AstAlways::matches(this) && stage == 2) {
		current_memwr_visible.clear();
		current_memwr_count.clear();
	}

	if (reset_width_after_children) {
		width_hint = backup_width_hint;
		sign_hint = backup_sign_hint;
		if (width_hint < 0)
			detectSignWidth(width_hint, sign_hint);
	}

	current_block = backup_current_block;
	current_block_child = backup_current_block_child;
	current_top_block = backup_current_top_block;
	current_always = backup_current_always;
	current_always_clocked = backup_current_always_clocked;

	for (auto it = backup_scope.begin(); it != backup_scope.end(); it++) {
		if (it->second == nullptr)
			current_scope.erase(it->first);
		else
			current_scope[it->first] = it->second;
	}

	if (ModuleLike::accepts(this))
		current_scope.clear();

	// convert defparam nodes to cell parameters
	if (auto dp_opt = AstDefparam::cast(this); dp_opt && !children.empty())
	{
		AstDefparam dp = *dp_opt;
		if (!AstIdentifier::matches(dp.lvalue()))
			input_error("Module name in defparam contains non-constant expressions!\n");

		string modname, paramname = dp.lvalue()->str;

		size_t pos = paramname.rfind('.');

		while (pos != 0 && pos != std::string::npos)
		{
			modname = paramname.substr(0, pos);

			if (current_scope.count(modname))
				break;

			pos = paramname.rfind('.', pos - 1);
		}

		if (pos == std::string::npos)
			input_error("Can't find object for defparam `%s`!\n", RTLIL::unescape_id(paramname));

		paramname = "\\" + paramname.substr(pos+1);

		if (current_scope.at(modname)->type != AST_CELL)
			input_error("Defparam argument `%s . %s` does not match a cell!\n",
					RTLIL::unescape_id(modname), RTLIL::unescape_id(paramname));

		auto paraset = std::make_unique<AstNode>(location, AST_PARASET, dp.value()->clone(), GetSize(children) > 2 ? children[2]->clone() : nullptr);
		paraset->str = paramname;

		AstNode *cell = current_scope.at(modname);
		cell->children.insert(cell->children.begin() + 1, std::move(paraset));
		delete_children();
	}

	// resolve typedefs
	if (auto td = AstTypedef::cast(this)) {
		AstNode *type_node = td->underlying().get();
		log_assert(WireOrMemory::accepts(type_node) || AstStructLike::matches(type_node));
		while (type_node->simplify(const_fold, stage, width_hint, sign_hint)) {
			did_something = true;
		}
		log_assert(!type_node->is_custom_type);
	}

	// resolve types of wires
	if (WireOrMemory::accepts(this)) {
		if (is_custom_type) {
			log_assert(children.size() >= 1);
			log_assert(children[0]->type == AST_WIRETYPE);
			auto type_name = children[0]->str;
			if (!current_scope.count(type_name)) {
				input_error("Unknown identifier `%s' used as type name\n", type_name);
			}
			AstNode *resolved_type_node = current_scope.at(type_name);
			if (resolved_type_node->type != AST_TYPEDEF)
				input_error("`%s' does not name a type\n", type_name);
			log_assert(resolved_type_node->children.size() == 1);
			auto& template_node = resolved_type_node->children[0];

			// Resolve the typedef from the bottom up, recursing within the current
			// block of code. Defer further simplification until the complete type is
			// resolved.
			while (template_node->is_custom_type && template_node->simplify(const_fold, stage, width_hint, sign_hint)) {};

			if (!str.empty() && str[0] == '\\' && AstStructLike::matches(template_node.get())) {
				// replace instance with wire representing the packed structure
				newNode = make_packed_struct(template_node.get(), str, attributes);
				newNode->set_attribute(ID::wiretype, mkconst_str(newNode->location, resolved_type_node->str));
				// add original input/output attribute to resolved wire
				newNode->is_input = this->is_input;
				newNode->is_output = this->is_output;
				current_scope[str] = this;
				goto apply_newNode;
			}

			// Prepare replacement node.
			newNode = template_node->clone();
			newNode->str = str;
			newNode->set_attribute(ID::wiretype, mkconst_str(newNode->location, resolved_type_node->str));
			newNode->is_input = is_input;
			newNode->is_output = is_output;
			newNode->is_wand = is_wand;
			newNode->is_wor = is_wor;
			for (auto &pair : attributes)
				newNode->set_attribute(pair.first, pair.second->clone());

			// if an enum then add attributes to support simulator tracing
			newNode->annotateTypedEnums(template_node.get());

			bool add_packed_dimensions = (AstWire::matches(this) && GetSize(children) > 1) || (AstMemory::matches(this) && GetSize(children) > 2);

			// Cannot add packed dimensions if unpacked dimensions are already specified.
			if (add_packed_dimensions && newNode->type == AST_MEMORY)
				input_error("Cannot extend unpacked type `%s' with packed dimensions\n", type_name);

			// Add packed dimensions.
			if (add_packed_dimensions) {
				auto& packed = children[1];
				if (newNode->children.empty())
					newNode->children.insert(newNode->children.begin(), packed->clone());
				else
					prepend_ranges(newNode->children[0], packed.get());
			}

			// Add unpacked dimensions.
			if (type == AST_MEMORY) {
				auto& unpacked = children.back();
				if (GetSize(newNode->children) < 2)
					newNode->children.push_back(unpacked->clone());
				else
					prepend_ranges(newNode->children[1], unpacked.get());
				newNode->type = type;
			}

			// Prepare to generate dimensions metadata for the resolved type.
			newNode->dimensions.clear();
			newNode->unpacked_dimensions = 0;

			goto apply_newNode;
		}
	}

	// resolve types of parameters
	if (ParameterLike::accepts(this)) {
		if (is_custom_type) {
			AstAnyParamLike param(this);
			log_assert(param.has_wiretype());

			// Pretend it's just a wire in order to resolve the type in the code block above.
			AstNodeType param_type = type;
			type = AST_WIRE;
			auto expr = std::move(children.front());
			children.erase(children.begin());
			while (is_custom_type && simplify(const_fold, stage, width_hint, sign_hint)) {};
			type = param_type;
			children.insert(children.begin(), std::move(expr));

			if (children[1]->type == AST_MEMORY)
				input_error("unpacked array type `%s' cannot be used for a parameter\n", children[1]->str);
			fixup_hierarchy_flags();
			did_something = true;
		}
		log_assert(!is_custom_type);
	}

	// resolve constant prefixes
	if (auto prefix = AstPrefix::cast(this)) {
		AstNode *idx = prefix->index().get();
		AstNode *suffix = prefix->suffix().get();
		if (idx->type != AST_CONSTANT) {
			// dumpAst(nullptr, ">   ");
			input_error("Index in generate block prefix syntax is not constant!\n");
		}
		if (suffix->type == AST_PREFIX)
			suffix->simplify(const_fold, stage, width_hint, sign_hint);
		log_assert(suffix->type == AST_IDENTIFIER);
		newNode = suffix->clone();
		const char *second_part = suffix->str.c_str();
		if (second_part[0] == '\\')
			second_part++;
		newNode->str = stringf("%s[%d].%s", str, idx->integer, second_part);
		goto apply_newNode;
	}

	// evaluate TO_BITS nodes
	if (auto tb = AstToBits::cast(this)) {
		AstNode *size_n = tb->size().get();
		AstNode *expr_n = tb->expr().get();
		if (size_n->type != AST_CONSTANT)
			input_error("Left operand of to_bits expression is not constant!\n");
		if (expr_n->type != AST_CONSTANT)
			input_error("Right operand of to_bits expression is not constant!\n");
		RTLIL::Const new_value = expr_n->bitsAsConst(size_n->bitsAsConst().as_int(), expr_n->is_signed);
		newNode = mkconst_bits(location, new_value.to_bits(), expr_n->is_signed);
		goto apply_newNode;
	}

	// annotate constant ranges
	if (auto r = AstRange::cast(this)) {
		bool old_range_valid = range_valid;
		range_valid = false;
		range_swapped = false;
		range_left = -1;
		range_right = 0;
		AstNode *msb = r->msb().get();
		log_assert(msb);
		if (auto k = AstConstant::cast(msb)) {
			range_valid = true;
			range_left = k->raw()->integer;
			if (children.size() == 1)
				range_right = range_left;
		}
		if (children.size() >= 2) {
			AstNode *lsb = r->lsb().get();
			if (auto k = AstConstant::cast(lsb))
				range_right = k->raw()->integer;
			else
				range_valid = false;
		}
		if (old_range_valid != range_valid)
			did_something = true;
		if (range_valid && range_right > range_left) {
			std::swap(range_left, range_right);
			range_swapped = true;
		}
	}

	// annotate wires with their ranges
	if (AstWire::matches(this)) {
		if (children.size() > 0) {
			// children[0] is the packed range/multirange in AstWire's grammar.
			AstNode *packed = children[0].get();
			if (packed->range_valid) {
				if (!range_valid)
					did_something = true;
				range_valid = true;
				range_swapped = packed->range_swapped;
				range_left = packed->range_left;
				range_right = packed->range_right;
				bool force_upto = false, force_downto = false;
				if (attributes.count(ID::force_upto)) {
					auto* val = attributes[ID::force_upto].get();
					if (val->type != AST_CONSTANT)
						input_error("Attribute `force_upto' with non-constant value!\n");
					force_upto = val->asAttrConst().as_bool();
				}
				if (attributes.count(ID::force_downto)) {
					auto* val = attributes[ID::force_downto].get();
					if (val->type != AST_CONSTANT)
						input_error("Attribute `force_downto' with non-constant value!\n");
					force_downto = val->asAttrConst().as_bool();
				}
				if (force_upto && force_downto)
					input_error("Attributes `force_downto' and `force_upto' cannot be both set!\n");
				if ((force_upto && !range_swapped) || (force_downto && range_swapped)) {
					std::swap(range_left, range_right);
					range_swapped = force_upto;
				}
				if (range_left == range_right && !attributes.count(ID::single_bit_vector))
					set_attribute(ID::single_bit_vector, mkconst_int(location, 1, false));
			}
		} else {
			if (!range_valid)
				did_something = true;
			range_valid = true;
			range_swapped = false;
			range_left = 0;
			range_right = 0;
			attributes.erase(ID::single_bit_vector);
		}
	}

	// Resolve packed and unpacked ranges in declarations.
	if (WireOrMemory::accepts(this) && dimensions.empty()) {
		if (!children.empty()) {
			// Unpacked ranges first, then packed ranges.
			for (int i = std::min(GetSize(children), 2) - 1; i >= 0; i--) {
				if (children[i]->type == AST_MULTIRANGE) {
					int width = 1;
					for (auto& range : children[i]->children) {
						width *= add_dimension(this, range.get());
						if (i) unpacked_dimensions++;
					}
					int left = width - 1, right = 0;
					if (i)
						std::swap(left, right);
					auto loc = children[i]->location;
					children[i] = std::make_unique<AstNode>(loc, AST_RANGE, mkconst_int(loc, left, true), mkconst_int(loc, right, true));
					fixup_hierarchy_flags();
					did_something = true;
				} else if (children[i]->type == AST_RANGE) {
					add_dimension(this, children[i].get());
					if (i) unpacked_dimensions++;
				}
			}
		} else {
			// 1 bit signal: bit, logic or reg
			dimensions.push_back({ 0, 1, false });
		}
	}

	// Resolve multidimensional array access.
	if (AstIdentifier::matches(this) && !basic_prep && id2ast && WireOrMemory::accepts(id2ast) &&
		children.size() > 0 && (children[0]->type == AST_RANGE || children[0]->type == AST_MULTIRANGE))
	{
		int dims_sel = children[0]->type == AST_MULTIRANGE ? children[0]->children.size() : 1;
		// Save original number of dimensions for $size() etc.
		integer = dims_sel;

		// Split access into unpacked and packed parts.
		std::unique_ptr<AstNode> unpacked_range = nullptr;
		std::unique_ptr<AstNode> packed_range = nullptr;

		if (id2ast->unpacked_dimensions) {
			if (id2ast->unpacked_dimensions > 1) {
				// Flattened range for access to unpacked dimensions.
				unpacked_range = make_index_range(id2ast, true);
			} else {
				// Index into one-dimensional unpacked part; unlink simple range node.
				auto& range = children[0]->type == AST_MULTIRANGE ? children[0]->children[0] : children[0];
				unpacked_range = std::move(range);
				range = nullptr;
			}
		}

		if (dims_sel > id2ast->unpacked_dimensions) {
			if (GetSize(id2ast->dimensions) - id2ast->unpacked_dimensions > 1) {
				// Flattened range for access to packed dimensions.
				packed_range = make_index_range(id2ast, false);
			} else {
				// Index into one-dimensional packed part; unlink simple range node.
				auto& range = children[0]->type == AST_MULTIRANGE ? children[0]->children[dims_sel - 1] : children[0];
				packed_range = std::move(range);
				range = nullptr;
			}
		}

		children.clear();

		if (unpacked_range)
			children.push_back(std::move(unpacked_range));

		if (packed_range)
			children.push_back(std::move(packed_range));

		fixup_hierarchy_flags();
		basic_prep = true;
		did_something = true;
	}

	// trim/extend parameters
	if (auto param = AstAnyParamLike::cast(this)) {
		if (param->has_range()) {
			AstNode *range_node = param->range_or_null();
			if (!range_node->range_valid)
				input_error("Non-constant width range on parameter decl.\n");
			int width = std::abs(range_node->range_left - range_node->range_right) + 1;
			AstNode *value_node = param->value();
			if (value_node->type == AST_REALVALUE) {
				RTLIL::Const constvalue = value_node->realAsConst(width);
				log_file_warning(*location.begin.filename, location.begin.line, "converting real value %e to binary %s.\n",
						value_node->realvalue, log_signal(constvalue));
				children[0] = mkconst_bits(location, constvalue.to_bits(), sign_hint);
				value_node = children[0].get();
				fixup_hierarchy_flags();
				did_something = true;
			}
			if (value_node->type == AST_CONSTANT) {
				if (width != int(value_node->bits.size())) {
					RTLIL::Const val;
					if (value_node->is_unsized) {
						val = value_node->bitsAsUnsizedConst(width);
					} else {
						val = value_node->bitsAsConst(width);
					}
					children[0] = mkconst_bits(location, val.to_bits(), is_signed);
					value_node = children[0].get();
					fixup_hierarchy_flags();
				}
				value_node->is_signed = is_signed;
			}
			range_valid = true;
			range_swapped = range_node->range_swapped;
			range_left = range_node->range_left;
			range_right = range_node->range_right;
		} else
		if (param->has_realvalue() && param->value()->type == AST_CONSTANT) {
			double as_realvalue = param->value()->asReal(sign_hint);
			children[0] = std::make_unique<AstNode>(location, AST_REALVALUE);
			children[0]->realvalue = as_realvalue;
			fixup_hierarchy_flags();
			did_something = true;
		}
	}

	if (AstIdentifier::matches(this) && !basic_prep) {
		// check if a plausible struct member sss.mmmm
		if (!str.empty() && str[0] == '\\' && current_scope.count(str)) {
			auto item_node = current_scope[str];
			if (StructMemberOrAggregate::accepts(item_node)) {
				// Traverse any hierarchical path until the full name for the referenced struct/union is found.
				std::string sname;
				bool found_sname = false;
				for (std::string::size_type pos = 0; (pos = str.find('.', pos)) != std::string::npos; pos++) {
					sname = str.substr(0, pos);
					if (current_scope.count(sname)) {
						if (ScopedDeclTarget::accepts(current_scope[sname])) {
							found_sname = true;
							break;
						}
					}
				}

				if (found_sname) {
					// structure member, rewrite this node to reference the packed struct wire
					auto range = make_index_range(item_node);
					newNode = std::make_unique<AstNode>(location, AST_IDENTIFIER, std::move(range));
					newNode->str = sname;
					// save type and original number of dimensions for $size() etc.
					newNode->set_attribute(ID::wiretype, item_node->clone());
					if (!item_node->dimensions.empty() && children.size() > 0) {
						if (children[0]->type == AST_RANGE)
							newNode->integer = 1;
						else if (children[0]->type == AST_MULTIRANGE)
							newNode->integer = children[0]->children.size();
					}
					newNode->basic_prep = true;
					if (item_node->is_signed)
						newNode = std::make_unique<AstNode>(location, AST_TO_SIGNED, std::move(newNode));
					goto apply_newNode;
				}
			}
		}
	}
	// annotate identifiers using scope resolution and create auto-wires as needed
	if (AstIdentifier::matches(this)) {
		if (current_scope.count(str) == 0) {
			AstNode *current_scope_ast = (current_ast_mod == nullptr) ? current_ast : current_ast_mod;
			str = try_pop_module_prefix();
			for (auto& node : current_scope_ast->children) {
				//log("looking at mod scope child %s\n", type2str(node->type));
				switch (node->type) {
				case AST_PARAMETER:
				case AST_LOCALPARAM:
				case AST_WIRE:
				case AST_AUTOWIRE:
				case AST_GENVAR:
				case AST_MEMORY:
				case AST_FUNCTION:
				case AST_TASK:
				case AST_DPI_FUNCTION:
					//log("found child %s, %s\n", type2str(node->type), node->str);
					if (str == node->str) {
						//log("add %s, type %s to scope\n", str, type2str(node->type));
						current_scope[node->str] = node.get();
					}
					break;
				case AST_ENUM:
					current_scope[node->str] = node.get();
					for (auto& enum_node : node->children) {
						log_assert(enum_node->type==AST_ENUM_ITEM);
						if (str == enum_node->str) {
							//log("\nadding enum item %s to scope\n", str);
							current_scope[str] = enum_node.get();
						}
					}
					break;
				default:
					break;
				}
			}
		}
		if (current_scope.count(str) == 0) {
			if (current_ast_mod == nullptr) {
				input_error("Identifier `%s' is implicitly declared outside of a module.\n", str);
			} else if (flag_autowire || str == "\\$global_clock") {
				auto auto_wire = std::make_unique<AstNode>(location, AST_AUTOWIRE);
				auto_wire->str = str;
				current_scope[str] = auto_wire.get();
				current_ast_mod->children.push_back(std::move(auto_wire));
				did_something = true;
			} else {
				input_error("Identifier `%s' is implicitly declared and `default_nettype is set to none.\n", str);
			}
		}
		if (id2ast != current_scope[str]) {
			id2ast = current_scope[str];
			did_something = true;
		}
	}

	// split memory access with bit select to individual statements
	if (AstIdentifier::matches(this) && children.size() == 2 && AstRange::matches(children[0].get()) && AstRange::matches(children[1].get()) && !in_lvalue && stage == 2)
	{
		if (id2ast == nullptr || !AstMemory::matches(id2ast) || children[0]->children.size() != 1)
			input_error("Invalid bit-select on memory access!\n");

		int mem_width, mem_size, addr_bits;
		id2ast->meminfo(mem_width, mem_size, addr_bits);

		AstNode *mem_data_range = AstMemory(id2ast).data_range();
		int data_range_left = mem_data_range->range_left;
		int data_range_right = mem_data_range->range_right;

		if (mem_data_range->range_swapped)
			std::swap(data_range_left, data_range_right);

		std::stringstream sstr;
		sstr << "$mem2bits$" << str << "$" << RTLIL::encode_filename(*location.begin.filename) << ":" << location.begin.line << "$" << (autoidx++);
		std::string wire_id = sstr.str();

		auto wire_owned = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, data_range_left, true), mkconst_int(location, data_range_right, true)));
		auto* wire = wire_owned.get();
		current_ast_mod->children.push_back(std::move(wire_owned));
		wire->str = wire_id;
		if (current_block)
			wire->set_attribute(ID::nosync, AstNode::mkconst_int(location, 1, false));
		while (wire->simplify()) { }

		auto data = clone();
		data->children.pop_back();

		auto assign = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), std::move(data));
		assign->children[0]->str = wire_id;
		assign->children[0]->was_checked = true;

		if (current_block)
		{
			size_t assign_idx = 0;
			while (assign_idx < current_block->children.size() && current_block->children[assign_idx].get() != current_block_child)
				assign_idx++;
			log_assert(assign_idx < current_block->children.size());
			current_block->children.insert(current_block->children.begin()+assign_idx, std::move(assign));
			wire->is_reg = true;
		}
		else
		{
			auto proc = std::make_unique<AstNode>(location, AST_ALWAYS, std::make_unique<AstNode>(location, AST_BLOCK));
			proc->children[0]->children.push_back(std::move(assign));
			current_ast_mod->children.push_back(std::move(proc));
		}

		newNode = std::make_unique<AstNode>(location, AST_IDENTIFIER, children[1]->clone());
		newNode->str = wire_id;
		newNode->integer = integer; // save original number of dimensions for $size() etc.
		newNode->id2ast = wire;
		goto apply_newNode;
	}

	if (AstWhile::matches(this))
		input_error("While loops are only allowed in constant functions!\n");

	if (auto rep = AstRepeat::cast(this)) {
		rep->unroll(stage);
		did_something = true;
	}

	// unroll for loops and generate-for blocks
	if (ForLike::accepts(this) && children.size() != 0)
	{
		// AST_FOR and AST_GENFOR share the same [init, cond, step, body] shape
		// (grammar invariant §1). We index by slot rather than cast to a view
		// because the branch is common to both tags.
		auto& init_ast = children[0];
		auto& while_ast = children[1];
		auto& next_ast = children[2];
		auto* body_ast = children[3].get();

		while (body_ast->type == AST_GENBLOCK && body_ast->str.empty() &&
				body_ast->children.size() == 1 && body_ast->children.at(0)->type == AST_GENBLOCK)
			body_ast = body_ast->children.at(0).get();

		const char* loop_type_str = "procedural";
		const char* var_type_str = "register";
		AstNodeType var_type = AST_WIRE;
		if (type == AST_GENFOR) {
			loop_type_str = "generate";
			var_type_str = "genvar";
			var_type = AST_GENVAR;
		}

		auto init_view = AstAssignEq::cast(init_ast.get());
		if (!init_view)
			input_error("Unsupported 1st expression of %s for-loop!\n", loop_type_str);
		auto next_view = AstAssignEq::cast(next_ast.get());
		if (!next_view)
			input_error("Unsupported 3rd expression of %s for-loop!\n", loop_type_str);

		AstNode *init_lhs = init_view->lhs().get();
		AstNode *next_lhs = next_view->lhs().get();
		if (init_lhs->id2ast == nullptr || init_lhs->id2ast->type != var_type)
			input_error("Left hand side of 1st expression of %s for-loop is not a %s!\n", loop_type_str, var_type_str);
		if (next_lhs->id2ast == nullptr || next_lhs->id2ast->type != var_type)
			input_error("Left hand side of 3rd expression of %s for-loop is not a %s!\n", loop_type_str, var_type_str);

		if (init_lhs->id2ast != next_lhs->id2ast)
			input_error("Incompatible left-hand sides in 1st and 3rd expression of %s for-loop!\n", loop_type_str);

		// eval 1st expression
		auto varbuf = init_view->rhs().get()->clone();
		{
			int expr_width_hint = -1;
			bool expr_sign_hint = true;
			varbuf->detectSignWidth(expr_width_hint, expr_sign_hint);
			while (varbuf->simplify(true, stage, 32, true)) { }
		}

		if (varbuf->type != AST_CONSTANT)
			input_error("Right hand side of 1st expression of %s for-loop is not constant!\n", loop_type_str);

		auto resolved = current_scope.at(init_lhs->str);
		if (resolved->range_valid) {
			int const_size = varbuf->range_left - varbuf->range_right;
			int resolved_size = resolved->range_left - resolved->range_right;
			if (const_size < resolved_size) {
				for (int i = const_size; i < resolved_size; i++)
					varbuf->bits.push_back(resolved->is_signed ? varbuf->bits.back() : State::S0);
				varbuf->range_left = resolved->range_left;
				varbuf->range_right = resolved->range_right;
				varbuf->range_swapped = resolved->range_swapped;
				varbuf->range_valid = resolved->range_valid;
			}
		}

		varbuf = std::make_unique<AstNode>(location, AST_LOCALPARAM, std::move(varbuf));
		varbuf->str = init_lhs->str;
		AstLocalparam varbuf_view(varbuf.get());

		AstNode *backup_scope_varbuf = current_scope[varbuf->str];
		current_scope[varbuf->str] = varbuf.get();

		size_t current_block_idx = 0;
		if (type == AST_FOR) {
			while (current_block_idx < current_block->children.size() &&
					current_block->children[current_block_idx].get() != current_block_child)
				current_block_idx++;
		}

		while (1)
		{
			// eval 2nd expression
			auto buf = while_ast->clone();
			{
				int expr_width_hint = -1;
				bool expr_sign_hint = true;
				buf->detectSignWidth(expr_width_hint, expr_sign_hint);
				while (buf->simplify(true, stage, expr_width_hint, expr_sign_hint)) { }
			}

			if (buf->type != AST_CONSTANT)
				input_error("2nd expression of %s for-loop is not constant!\n", loop_type_str);

			if (buf->integer == 0) {
				break;
			}

			// expand body
			int index = varbuf_view.value()->integer;
			log_assert(BlockOrGenBlock::accepts(body_ast));
			log_assert(!body_ast->str.empty());
			buf = body_ast->clone();

			std::stringstream sstr;
			sstr << buf->str << "[" << index << "].";
			std::string prefix = sstr.str();

			// create a scoped localparam for the current value of the loop variable
			auto local_index = varbuf->clone();
			size_t pos = local_index->str.rfind('.');
			if (pos != std::string::npos) // remove outer prefix
				local_index->str = "\\" + local_index->str.substr(pos + 1);
			local_index->str = prefix_id(prefix, local_index->str);
			current_scope[local_index->str] = local_index.get();
			current_ast_mod->children.push_back(std::move(local_index));

			buf->expand_genblock(prefix);

			if (type == AST_GENFOR) {
				for (size_t i = 0; i < buf->children.size(); i++) {
					buf->children[i]->simplify(const_fold, stage, -1, false);
					current_ast_mod->children.push_back(std::move(buf->children[i]));
				}
			} else {
				for (size_t i = 0; i < buf->children.size(); i++)
					current_block->children.insert(current_block->children.begin() + current_block_idx++, std::move(buf->children[i]));
			}
			buf->children.clear();

			// eval 3rd expression
			buf = next_view->rhs().get()->clone();
			buf->set_in_param_flag(true);
			{
				int expr_width_hint = -1;
				bool expr_sign_hint = true;
				buf->detectSignWidth(expr_width_hint, expr_sign_hint);
				while (buf->simplify(true, stage, expr_width_hint, expr_sign_hint)) { }
			}

			if (buf->type != AST_CONSTANT)
				input_error("Right hand side of 3rd expression of %s for-loop is not constant (%s)!\n", loop_type_str, type2str(buf->type));

			varbuf_view.value_slot().set(std::move(buf));
		}

		if (type == AST_FOR) {
			auto buf = next_ast->clone();
			AstAssignEq(buf.get()).rhs().set(varbuf_view.value()->clone());
			current_block->children.insert(current_block->children.begin() + current_block_idx++, std::move(buf));
		}

		current_scope[varbuf->str] = backup_scope_varbuf;
		delete_children();
		did_something = true;
	}

	// check for local objects in unnamed block
	if (auto blk = AstBlock::cast(this); blk && str.empty())
	{
		for (auto& child : *blk)
			if (CommonDeclarations::accepts(child.get()))
			{
				log_assert(!sv_mode_but_global_and_used_for_literally_one_condition);
				child->input_error("Local declaration in unnamed block is only supported in SystemVerilog mode!\n");
			}
	}

	// transform block with name
	if (auto blk = AstBlock::cast(this); blk && !str.empty())
	{
		expand_genblock(str + ".");

		// if this is an autonamed block is in an always_comb
		if (current_always && current_always->attributes.count(ID::always_comb)
				&& is_autonamed_block(str))
			// track local variables in this block so we can consider adding
			// nosync once the block has been fully elaborated
			for (auto& child : *blk)
				if (child->type == AST_WIRE &&
						!child->attributes.count(ID::nosync))
					mark_auto_nosync(this, child.get());

		std::vector<std::unique_ptr<AstNode>> new_children;
		for (auto& child : *blk)
			if (CommonDeclarations::accepts(child.get())) {
				child->simplify(false, stage, -1, false);
				current_scope[child->str] = child.get();
				current_ast_mod->children.push_back(std::move(child));
			} else
				new_children.push_back(std::move(child));

		children.swap(new_children);
		did_something = true;
		str.clear();
	}

	// simplify unconditional generate block
	if (auto gblk = AstGenBlock::cast(this); gblk && children.size() != 0) {
		gblk->elaborate(const_fold, stage);
		did_something = true;
	}

	// simplify generate-if blocks
	if (auto genif = AstGenIf::cast(this); genif && children.size() != 0) {
		genif->elaborate(stage, width_hint, sign_hint, const_fold);
		did_something = true;
	}

	// simplify generate-case blocks
	if (auto gencase = AstGenCase::cast(this); gencase && children.size() != 0) {
		gencase->elaborate(stage, width_hint, sign_hint, const_fold);
		did_something = true;
	}

	// unroll cell arrays
	if (auto carr = AstCellArray::cast(this)) {
		newNode = carr->unroll();
		goto apply_newNode;
	}

	// replace primitives with assignments
	if (auto prim = AstPrimitive::cast(this)) {
		auto replacement = prim->lower();
		if (replacement)
			newNode = std::move(replacement);
		did_something = true;
	}

	// replace dynamic ranges in left-hand side expressions (e.g. "foo[bar] <= 1'b1;") with
	// either a big case block that selects the correct single-bit assignment, or mask and
	// shift operations.
	if (BlockingAssignLike::accepts(this))
	{
		// Both share the 2-child [lhs, rhs] shape (grammar §15).
		AstNode *lhs_node = children[0].get();
		AstNode *rhs_node = children[1].get();
		auto lhs_id_opt = AstIdentifier::cast(lhs_node);
		if (!lhs_id_opt || !lhs_id_opt->has_bit_select())
			goto skip_dynamic_range_lvalue_expansion;
		AstIdentifier lhs_id = *lhs_id_opt;
		AstNode *range_node = lhs_id.bit_select_raw();
		if (range_node->range_valid || did_something)
			goto skip_dynamic_range_lvalue_expansion;
		if (lhs_node->id2ast == nullptr || lhs_node->id2ast->type != AST_WIRE)
			goto skip_dynamic_range_lvalue_expansion;
		if (!lhs_node->id2ast->range_valid)
			goto skip_dynamic_range_lvalue_expansion;

		AST::AstNode *member_node = lhs_node->get_struct_member();
		int wire_width = member_node ?
			member_node->range_left - member_node->range_right + 1 :
			lhs_node->id2ast->range_left - lhs_node->id2ast->range_right + 1;
		int wire_offset = lhs_node->id2ast->range_right;
		int result_width = 1;

		std::unique_ptr<AstNode> shift_expr = nullptr;

		if (!try_determine_range_width(range_node, result_width))
			input_error("Unsupported expression on dynamic range select on signal `%s'!\n", str);

		if (range_node->children.size() >= 2)
			shift_expr = range_node->children[1]->clone();
		else
			shift_expr = range_node->children[0]->clone();

		bool use_case_method = lhs_node->id2ast->get_bool_attribute(ID::nowrshmsk);

		if (!use_case_method && current_always->detect_latch(lhs_node->str))
			use_case_method = true;

		if (use_case_method) {
			// big case block

			int stride = 1;
			long long bitno_div = stride;

			int case_width_hint;
			bool case_sign_hint;
			shift_expr->detectSignWidth(case_width_hint, case_sign_hint);
			int max_width = case_width_hint;

			if (member_node) {  // Member in packed struct/union
				// Clamp chunk to range of member within struct/union.
				log_assert(!wire_offset && !lhs_node->id2ast->range_swapped);

				// When the (* nowrshmsk *) attribute is set, a CASE block is generated below
				// to select the indexed bit slice. When a multirange array is indexed, the
				// start of each possible slice is separated by the bit stride of the last
				// index dimension, and we can optimize the CASE block accordingly.
				// The dimension of the original array expression is saved in the 'integer' field.
				int dims = lhs_node->integer;
				stride = wire_width;
				for (int dim = 0; dim < dims; dim++) {
					stride /= member_node->dimensions[dim].range_width;
				}
				bitno_div = stride;
			} else {
				// Extract (index)*(width) from non_opt_range pattern ((@selfsz@((index)*(width)))+(0)).
				AstNode *lsb_expr =
					shift_expr->type == AST_ADD && shift_expr->children[0]->type == AST_SELFSZ &&
					shift_expr->children[1]->type == AST_CONSTANT && shift_expr->children[1]->integer == 0 ?
					shift_expr->children[0]->children[0].get() :
					shift_expr.get();

				// Extract stride from indexing of two-dimensional packed arrays and
				// variable slices on the form dst[i*stride +: width] = src.
				if (lsb_expr->type == AST_MUL &&
					(lsb_expr->children[0]->type == AST_CONSTANT ||
					 lsb_expr->children[1]->type == AST_CONSTANT))
				{
					int stride_ix = lsb_expr->children[1]->type == AST_CONSTANT;
					stride = (int)lsb_expr->children[stride_ix]->integer;
					bitno_div = stride != 0 ? stride : 1;

					// Check whether i*stride can overflow.
					int i_width;
					bool i_sign;
					lsb_expr->children[1 - stride_ix]->detectSignWidth(i_width, i_sign);
					int stride_width;
					bool stride_sign;
					lsb_expr->children[stride_ix]->detectSignWidth(stride_width, stride_sign);
					max_width = std::max(i_width, stride_width);
					// Stride width calculated from actual stride value.
					if (stride == 0)
						stride_width = 0;
					else
						stride_width = std::ceil(std::log2(std::abs(stride)));

					if (i_width + stride_width > max_width) {
						// For (truncated) i*stride to be within the range of dst, the following must hold:
						//   i*stride ≡ bitno (mod shift_mod), i.e.
						//   i*stride = k*shift_mod + bitno
						//
						// The Diophantine equation on the form ax + by = c:
						//   stride*i - shift_mod*k = bitno
						// has solutions iff c is a multiple of d = gcd(a, b), i.e.
						//   bitno mod gcd(stride, shift_mod) = 0
						//
						// long long is at least 64 bits in C++11
						long long shift_mod = 1ll << (max_width - case_sign_hint);
						bitno_div = std::gcd((long long)stride, shift_mod);
					}
				}
			}

			// long long is at least 64 bits in C++11
			long long max_offset = (1ll << (max_width - case_sign_hint)) - 1;
			long long min_offset = case_sign_hint ? -(1ll << (max_width - 1)) : 0;

			// A temporary register holds the result of the (possibly complex) rvalue expression,
			// avoiding repetition in each AST_COND below.
			int rvalue_width;
			bool rvalue_sign;
			rhs_node->detectSignWidth(rvalue_width, rvalue_sign);
			auto rvalue = mktemp_logic(location, "$bitselwrite$rvalue$", current_ast_mod, true, rvalue_width - 1, 0, rvalue_sign);
			auto* rvalue_leaky = rvalue.get();
			log("make 1\n");
			auto case_node_owned = std::make_unique<AstNode>(location, AST_CASE, std::move(shift_expr));
			auto* case_node = case_node_owned.get();
			newNode = std::make_unique<AstNode>(location, AST_BLOCK,
						  std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::move(rvalue), rhs_node->clone()),
						  std::move(case_node_owned));

			did_something = true;
			for (int i = 1 - result_width; i < wire_width; i++) {
				// Out of range indexes are handled in genrtlil.cc
				int start_bit = wire_offset + i;
				int end_bit = start_bit + result_width - 1;
				// Check whether the current index can be generated by shift_expr.
				if (start_bit < min_offset || start_bit > max_offset)
					continue;
				if (start_bit%bitno_div != 0 || (stride == 0 && start_bit != 0))
					continue;

				auto cond = std::make_unique<AstNode>(location, AST_COND, mkconst_int(location, start_bit, case_sign_hint, max_width));
				auto lvalue = lhs_node->clone();
				lvalue->delete_children();
				if (member_node)
					lvalue->set_attribute(ID::wiretype, member_node->clone());
				lvalue->children.push_back(std::make_unique<AstNode>(location, AST_RANGE,
						mkconst_int(location, end_bit, true), mkconst_int(location, start_bit, true)));
				cond->children.push_back(std::make_unique<AstNode>(location, AST_BLOCK, std::make_unique<AstNode>(location, std::move(type), std::move(lvalue), rvalue_leaky->clone())));
				case_node->children.push_back(std::move(cond));
			}
		} else {
			// mask and shift operations
			// dst = (dst & ~(width'1 << lsb)) | unsigned'(width'(src)) << lsb)

			auto lvalue = lhs_node->clone();
			lvalue->delete_children();
			if (member_node)
				lvalue->set_attribute(ID::wiretype, member_node->clone());

			auto old_data = lvalue->clone();
			if (type == AST_ASSIGN_LE)
				old_data->lookahead = true;

			int shift_width_hint;
			bool shift_sign_hint;
			shift_expr->detectSignWidth(shift_width_hint, shift_sign_hint);

			// All operations are carried out in a new block.
			newNode = std::make_unique<AstNode>(location, AST_BLOCK);

			// Temporary register holding the result of the bit- or part-select position expression.
			auto pos = mktemp_logic(location, "$bitselwrite$pos$", current_ast_mod, true, shift_width_hint - 1, 0, shift_sign_hint);
			// Calculate lsb from position.
			auto shift_val = pos->clone();

			newNode->children.push_back(std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::move(pos), std::move(shift_expr)));

			// If the expression is signed, we must add an extra bit for possible negation of the most negative number.
			// If the expression is unsigned, we must add an extra bit for sign.
			shift_val = std::make_unique<AstNode>(location, AST_CAST_SIZE, mkconst_int(location, shift_width_hint + 1, true), std::move(shift_val));
			if (!shift_sign_hint)
				shift_val = std::make_unique<AstNode>(location, AST_TO_SIGNED, std::move(shift_val));

			// offset the shift amount by the lower bound of the dimension
			if (wire_offset != 0)
				shift_val = std::make_unique<AstNode>(location, AST_SUB, std::move(shift_val), mkconst_int(location, wire_offset, true));

			// reflect the shift amount if the dimension is swapped
			if (lhs_node->id2ast->range_swapped)
				shift_val = std::make_unique<AstNode>(location, AST_SUB, mkconst_int(location, wire_width - result_width, true), std::move(shift_val));

			// AST_SHIFT uses negative amounts for shifting left
			shift_val = std::make_unique<AstNode>(location, AST_NEG, std::move(shift_val));
			auto also_shift_val = shift_val->clone();

			// dst = (dst & ~(width'1 << lsb)) | unsigned'(width'(src)) << lsb)
			did_something = true;
			auto bitmask = mkconst_bits(location, std::vector<RTLIL::State>(result_width, State::S1), false);
			newNode->children.push_back(
				std::make_unique<AstNode>(location, std::move(type),
						std::move(lvalue),
						std::make_unique<AstNode>(location, AST_BIT_OR,
							std::make_unique<AstNode>(location, AST_BIT_AND,
									std::move(old_data),
									std::make_unique<AstNode>(location, AST_BIT_NOT,
										std::make_unique<AstNode>(location, AST_SHIFT,
												std::move(bitmask),
												std::move(shift_val)))),
							std::make_unique<AstNode>(location, AST_SHIFT,
									std::make_unique<AstNode>(location, AST_TO_UNSIGNED,
										std::make_unique<AstNode>(location, AST_CAST_SIZE,
												mkconst_int(location, result_width, true),
												rhs_node->clone())),
									std::move(also_shift_val)))));

			newNode->fixup_hierarchy_flags(true);
		}

		goto apply_newNode;
	}
skip_dynamic_range_lvalue_expansion:;

	// found right-hand side identifier for memory -> replace with memory read port
	if (auto id_view = AstIdentifier::cast(this);
			stage > 1 && id_view && id2ast != nullptr && id2ast->type == AST_MEMORY && !in_lvalue &&
			id_view->has_bit_select() && id_view->bit_select_raw()->type == AST_RANGE &&
			id_view->bit_select_raw()->children.size() == 1) {
		if (integer < (unsigned)id2ast->unpacked_dimensions)
			input_error("Insufficient number of array indices for %s.\n", log_id(str));
		AstRange range_view(id_view->bit_select_raw());
		newNode = AstMemRd::build(location, range_view.msb()->clone());
		newNode->str = str;
		newNode->id2ast = id2ast;
		goto apply_newNode;
	}

	// assignment with nontrivial member in left-hand concat expression -> split assignment
	if (BlockingAssignLike::accepts(this) && children[0]->type == AST_CONCAT && width_hint > 0)
	{
		AstConcat lhs_concat(children[0].get());
		AstNode *rhs_n = children[1].get();
		bool found_nontrivial_member = false;

		for (auto& child : lhs_concat) {
			if (child->type == AST_IDENTIFIER && child->id2ast != nullptr && child->id2ast->type == AST_MEMORY)
				found_nontrivial_member = true;
		}

		if (found_nontrivial_member)
		{
			newNode = std::make_unique<AstNode>(location, AST_BLOCK);

			auto wire_tmp_owned = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, width_hint-1, true), mkconst_int(location, 0, true)));
			auto wire_tmp = wire_tmp_owned.get();
			wire_tmp->str = stringf("$splitcmplxassign$%s:%d$%d", RTLIL::encode_filename(*location.begin.filename), location.begin.line, autoidx++);
			current_scope[wire_tmp->str] = wire_tmp;
			current_ast_mod->children.push_back(std::move(wire_tmp_owned));
			wire_tmp->set_attribute(ID::nosync, AstNode::mkconst_int(location, 1, false));
			while (wire_tmp->simplify()) { }
			wire_tmp->is_logic = true;

			auto wire_tmp_id_owned = std::make_unique<AstNode>(location, AST_IDENTIFIER);
			auto* wire_tmp_id = wire_tmp_id_owned.get();
			wire_tmp_id->str = wire_tmp->str;

			newNode->children.push_back(std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::move(wire_tmp_id_owned), rhs_n->clone()));
			newNode->children.back()->was_checked = true;

			int cursor = 0;
			for (auto& child : lhs_concat)
			{
				int child_width_hint = -1;
				bool child_sign_hint = true;
				child->detectSignWidth(child_width_hint, child_sign_hint);

				auto rhs = wire_tmp_id->clone();
				rhs->children.push_back(std::make_unique<AstNode>(location, AST_RANGE, AstNode::mkconst_int(location, cursor+child_width_hint-1, true), AstNode::mkconst_int(location, cursor, true)));
				newNode->children.push_back(std::make_unique<AstNode>(location, type, child->clone(), std::move(rhs)));

				cursor += child_width_hint;
			}

			goto apply_newNode;
		}
	}

	// Expand array assignment: arr_out = arr_in OR arr_out = cond ? arr_a : arr_b
	// Supports multi-dimensional unpacked arrays
	if (AstAnyAssign::matches(this) &&
	    is_unexpanded_array_ref(children[0].get()))
	{
		AstNode *lhs = children[0].get();
		AstNode *rhs = children[1].get();
		AstNode *lhs_mem = lhs->id2ast;

		// Case 1: Direct array assignment (b = a)
		bool is_direct_assign = is_unexpanded_array_ref(rhs);

		// Case 2: Ternary array assignment (out = sel ? a : b)
		auto rhs_tern = AstTernary::cast(rhs);
		bool is_ternary_assign = (rhs_tern &&
		                          is_unexpanded_array_ref(rhs_tern->then_().get()) &&
		                          is_unexpanded_array_ref(rhs_tern->else_().get()));

		if (is_direct_assign || is_ternary_assign)
		{
			AstNode *direct_rhs_mem = nullptr;
			AstNode *true_mem = nullptr;
			AstNode *false_mem = nullptr;

			// Validate array compatibility
			if (is_direct_assign) {
				direct_rhs_mem = rhs->id2ast;
				if (!arrays_have_compatible_dims(lhs_mem, direct_rhs_mem))
					input_error("Array dimension mismatch in assignment\n");
			} else {
				true_mem = rhs_tern->then_()->id2ast;
				false_mem = rhs_tern->else_()->id2ast;
				if (!arrays_have_compatible_dims(lhs_mem, true_mem) ||
				    !arrays_have_compatible_dims(lhs_mem, false_mem))
					input_error("Array dimension mismatch in ternary expression\n");
			}

			int num_dims = lhs_mem->unpacked_dimensions;

			// Helper to add index to an identifier clone
			auto add_indices_to_id = [&](std::unique_ptr<AstNode> id, const std::vector<int>& indices) {
				if (num_dims == 1) {
					// Single dimension: use AST_RANGE
					id->children.push_back(std::make_unique<AstNode>(location, AST_RANGE,
						mkconst_int(location, indices[0], true)));
				} else {
					// Multiple dimensions: use AST_MULTIRANGE
					auto multirange = std::make_unique<AstNode>(location, AST_MULTIRANGE);
					for (int idx : indices) {
						multirange->children.push_back(std::make_unique<AstNode>(location, AST_RANGE,
							mkconst_int(location, idx, true)));
					}
					id->children.push_back(std::move(multirange));
				}
				id->integer = num_dims;
				// Reset basic_prep so multirange gets resolved during subsequent simplify passes
				id->basic_prep = false;
				return id;
			};

			// Calculate total number of elements and warn if large
			int total_elements = 1;
			for (int d = 0; d < num_dims; d++)
				total_elements *= lhs_mem->dimensions[d].range_width;
			if (total_elements > 10000)
				log_warning("Expanding array assignment with %d elements at %s, this may be slow.\n",
					total_elements, location.to_string().c_str());

			// Collect all assignments
			std::vector<std::unique_ptr<AstNode>> assignments;

			foreach_array_position(lhs_mem, [&](const std::vector<int>& position) {
				auto lhs_indices = array_indices_from_position(lhs_mem, position);
				auto lhs_idx = add_indices_to_id(lhs->clone(), lhs_indices);

				std::unique_ptr<AstNode> rhs_expr;
				if (is_direct_assign) {
					auto rhs_indices = array_indices_from_position(direct_rhs_mem, position);
					rhs_expr = add_indices_to_id(rhs->clone(), rhs_indices);
				} else {
					// Ternary case
					auto true_indices = array_indices_from_position(true_mem, position);
					auto false_indices = array_indices_from_position(false_mem, position);
					auto true_idx = add_indices_to_id(rhs_tern->then_()->clone(), true_indices);
					auto false_idx = add_indices_to_id(rhs_tern->else_()->clone(), false_indices);

					rhs_expr = AstTernary::build(location, rhs_tern->cond()->clone(),
							std::move(true_idx), std::move(false_idx));
				}

				auto assign = std::make_unique<AstNode>(location, type,
					std::move(lhs_idx), std::move(rhs_expr));
				assign->was_checked = true;
				assignments.push_back(std::move(assign));
			});

			// For continuous assignments, add to module; for procedural, use block
			if (type == AST_ASSIGN) {
				// Add all but last to module
				for (size_t i = 0; i + 1 < assignments.size(); i++)
					current_ast_mod->children.push_back(std::move(assignments[i]));
				// Last one replaces current node
				newNode = std::move(assignments.back());
			} else {
				// Wrap in AST_BLOCK for procedural
				newNode = std::make_unique<AstNode>(location, AST_BLOCK);
				for (auto& assign : assignments)
					newNode->children.push_back(std::move(assign));
			}

			goto apply_newNode;
		}
	}

	// assignment with memory in left-hand side expression -> replace with memory write port
	if (stage > 1 && BlockingAssignLike::accepts(this) && children[0]->type == AST_IDENTIFIER &&
			children[0]->id2ast && children[0]->id2ast->type == AST_MEMORY && children[0]->id2ast->children.size() >= 2 &&
			children[0]->id2ast->children[0]->range_valid && children[0]->id2ast->children[1]->range_valid &&
			(children[0]->children.size() == 1 || children[0]->children.size() == 2) && children[0]->children[0]->type == AST_RANGE)
	{
		AstIdentifier lhs_id(children[0].get());
		AstNode *mem_decl = lhs_id.raw()->id2ast;
		AstMemory mem_view(mem_decl);
		AstNode *rhs_n = children[1].get();
		if (lhs_id.raw()->integer < (unsigned)mem_decl->unpacked_dimensions)
			input_error("Insufficient number of array indices for %s.\n", log_id(str));

		std::stringstream sstr;
		sstr << "$memwr$" << lhs_id.str() << "$" << RTLIL::encode_filename(*location.begin.filename) << ":" << location.begin.line << "$" << (autoidx++);
		std::string id_addr = sstr.str() + "_ADDR", id_data = sstr.str() + "_DATA", id_en = sstr.str() + "_EN";

		int mem_width, mem_size, addr_bits;
		bool mem_signed = mem_decl->is_signed;
		mem_decl->meminfo(mem_width, mem_size, addr_bits);

		newNode = std::make_unique<AstNode>(location, AST_BLOCK);
		auto defNode = std::make_unique<AstNode>(location, AST_BLOCK);

		AstNode *mem_data_range = mem_view.data_range();
		int data_range_left = mem_data_range->range_left;
		int data_range_right = mem_data_range->range_right;
		int mem_data_range_offset = std::min(data_range_left, data_range_right);

		AstRange addr_range_view(lhs_id.bit_select_raw());
		AstNode *addr_expr = addr_range_view.msb().get();
		int addr_width_hint = -1;
		bool addr_sign_hint = true;
		addr_expr->detectSignWidthWorker(addr_width_hint, addr_sign_hint);
		addr_bits = std::max(addr_bits, addr_width_hint);

		std::vector<RTLIL::State> x_bits_addr, x_bits_data, set_bits_en;
		for (int i = 0; i < addr_bits; i++)
			x_bits_addr.push_back(RTLIL::State::Sx);
		for (int i = 0; i < mem_width; i++)
			x_bits_data.push_back(RTLIL::State::Sx);
		for (int i = 0; i < mem_width; i++)
			set_bits_en.push_back(RTLIL::State::S1);

		std::unique_ptr<AstNode> node_addr = nullptr;
		if (addr_expr->isConst()) {
			node_addr = addr_expr->clone();
		} else {
			auto wire_addr_owned = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, addr_bits-1, true), mkconst_int(location, 0, true)));
			auto* wire_addr = wire_addr_owned.get();
			wire_addr->str = id_addr;
			wire_addr->was_checked = true;
			current_ast_mod->children.push_back(std::move(wire_addr_owned));
			current_scope[wire_addr->str] = wire_addr;
			while (wire_addr->simplify()) { }

			auto assign_addr = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), mkconst_bits(location, x_bits_addr, false));
			assign_addr->children[0]->str = id_addr;
			assign_addr->children[0]->was_checked = true;
			defNode->children.push_back(std::move(assign_addr));

			assign_addr = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), addr_expr->clone());
			assign_addr->children[0]->str = id_addr;
			assign_addr->children[0]->was_checked = true;
			newNode->children.push_back(std::move(assign_addr));

			node_addr = std::make_unique<AstNode>(location, AST_IDENTIFIER);
			node_addr->str = id_addr;
		}

		std::unique_ptr<AstNode> node_data = nullptr;
		if (lhs_id.raw()->children.size() == 1 && rhs_n->isConst()) {
			node_data = rhs_n->clone();
		} else {
			auto wire_data_owned = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, mem_width-1, true), mkconst_int(location, 0, true)));
			auto* wire_data = wire_data_owned.get();
			wire_data->str = id_data;
			wire_data->was_checked = true;
			wire_data->is_signed = mem_signed;
			current_scope[wire_data->str] = wire_data;
			current_ast_mod->children.push_back(std::move(wire_data_owned));
			while (wire_data->simplify()) { }

			auto assign_data = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), mkconst_bits(location, x_bits_data, false));
			assign_data->children[0]->str = id_data;
			assign_data->children[0]->was_checked = true;
			defNode->children.push_back(std::move(assign_data));

			node_data = std::make_unique<AstNode>(location, AST_IDENTIFIER);
			node_data->str = id_data;
		}

		auto wire_en_owned = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE, mkconst_int(location, mem_width-1, true), mkconst_int(location, 0, true)));
		auto* wire_en = wire_en_owned.get();
		wire_en->str = id_en;
		wire_en->was_checked = true;
		current_scope[wire_en->str] = wire_en;
		current_ast_mod->children.push_back(std::move(wire_en_owned));
		while (wire_en->simplify()) { }

		auto assign_en_first = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), mkconst_int(location, 0, false, mem_width));
		assign_en_first->children[0]->str = id_en;
		assign_en_first->children[0]->was_checked = true;
		defNode->children.push_back(std::move(assign_en_first));

		auto node_en = std::make_unique<AstNode>(location, AST_IDENTIFIER);
		node_en->str = id_en;

		if (!defNode->children.empty())
			current_top_block->children.insert(current_top_block->children.begin(), std::move(defNode));

		std::unique_ptr<AstNode> assign_data = nullptr;
		std::unique_ptr<AstNode> assign_en = nullptr;
		if (lhs_id.raw()->children.size() == 2)
		{
			AstNode *slice_range = lhs_id.raw()->children[1].get();
			if (slice_range->range_valid)
			{
				int offset = slice_range->range_right;
				int width = slice_range->range_left - offset + 1;
				offset -= mem_data_range_offset;

				std::vector<RTLIL::State> padding_x(offset, RTLIL::State::Sx);

				assign_data = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER),
						std::make_unique<AstNode>(location, AST_CONCAT, mkconst_bits(location, padding_x, false), rhs_n->clone()));
				assign_data->children[0]->str = id_data;
				assign_data->children[0]->was_checked = true;

				for (int i = 0; i < mem_width; i++)
					set_bits_en[i] = offset <= i && i < offset+width ? RTLIL::State::S1 : RTLIL::State::S0;
				assign_en = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), mkconst_bits(location, set_bits_en, false));
				assign_en->children[0]->str = id_en;
				assign_en->children[0]->was_checked = true;
			}
			else
			{
				std::unique_ptr<AstNode> offset_ast;
				int width;

				if (!try_determine_range_width(slice_range, width))
					input_error("Unsupported expression on dynamic range select on signal `%s'!\n", str);

				if (slice_range->children.size() >= 2)
					offset_ast = slice_range->children[1]->clone();
				else
					offset_ast = slice_range->children[0]->clone();

				if (mem_data_range_offset)
					offset_ast = std::make_unique<AstNode>(location, AST_SUB, std::move(offset_ast), mkconst_int(location, mem_data_range_offset, true));

				assign_data = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER),
						std::make_unique<AstNode>(location, AST_SHIFT_LEFT, rhs_n->clone(), offset_ast->clone()));
				assign_data->children[0]->str = id_data;
				assign_data->children[0]->was_checked = true;

				for (int i = 0; i < mem_width; i++)
					set_bits_en[i] = i < width ? RTLIL::State::S1 : RTLIL::State::S0;
				assign_en = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER),
						std::make_unique<AstNode>(location, AST_SHIFT_LEFT, mkconst_bits(location, set_bits_en, false), offset_ast->clone()));
				assign_en->children[0]->str = id_en;
				assign_en->children[0]->was_checked = true;
			}
		}
		else
		{
			if (!(lhs_id.raw()->children.size() == 1 && rhs_n->isConst())) {
				assign_data = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), rhs_n->clone());
				assign_data->children[0]->str = id_data;
				assign_data->children[0]->was_checked = true;
			}

			assign_en = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::make_unique<AstNode>(location, AST_IDENTIFIER), mkconst_bits(location, set_bits_en, false));
			assign_en->children[0]->str = id_en;
			assign_en->children[0]->was_checked = true;
		}
		if (assign_data)
			newNode->children.push_back(std::move(assign_data));
		if (assign_en)
			newNode->children.push_back(std::move(assign_en));

		std::unique_ptr<AstNode> wrnode;
		if (AstInitial::matches(current_always))
			wrnode = AstMemInit::build(location, std::move(node_addr), std::move(node_data), std::move(node_en), mkconst_int(location, 1, false));
		else
			wrnode = AstMemWr::build(location, std::move(node_addr), std::move(node_data), std::move(node_en));
		wrnode->str = lhs_id.str();
		wrnode->id2ast = mem_decl;
		wrnode->location = location;
		if (wrnode->type == AST_MEMWR) {
			int portid = current_memwr_count[wrnode->str]++;
			wrnode->children.push_back(mkconst_int(location, portid, false));
			std::vector<RTLIL::State> priority_mask;
			for (int i = 0; i < portid; i++) {
				bool has_prio = current_memwr_visible[wrnode->str].count(i);
				priority_mask.push_back(State(has_prio));
			}
			wrnode->children.push_back(mkconst_bits(location, priority_mask, false));
			current_memwr_visible[wrnode->str].insert(portid);
			current_always->children.push_back(std::move(wrnode));
		} else {
			current_ast_mod->children.push_back(std::move(wrnode));
		}

		if (newNode->children.empty()) {
			newNode = std::make_unique<AstNode>(location);
		}
		goto apply_newNode;
	}

	// replace function and task calls with the code from the function or task
	if (CallLike::accepts(this) && !str.empty())
	{
		// Both call types share variadic-arg shape; treat 'this' as a generic call.
		if (type == AST_FCALL)
		{
			AstFcall fcall(this);
			if (str == "\\$initstate")
			{
				int myidx = autoidx++;

				auto wire_owned = std::make_unique<AstNode>(location, AST_WIRE);
				auto* wire = wire_owned.get();
				current_ast_mod->children.push_back(std::move(wire_owned));
				wire->str = stringf("$initstate$%d_wire", myidx);
				while (wire->simplify()) { }

				auto cell_owned = std::make_unique<AstNode>(location, AST_CELL, std::make_unique<AstNode>(location, AST_CELLTYPE), std::make_unique<AstNode>(location, AST_ARGUMENT, std::make_unique<AstNode>(location, AST_IDENTIFIER)));
				auto* cell = cell_owned.get();
				cell->str = stringf("$initstate$%d", myidx);
				cell->children[0]->str = "$initstate";
				cell->children[1]->str = "\\Y";
				cell->children[1]->children[0]->str = wire->str;
				cell->children[1]->children[0]->id2ast = wire;
				current_ast_mod->children.push_back(std::move(cell_owned));
				while (cell->simplify()) { }

				newNode = std::make_unique<AstNode>(location, AST_IDENTIFIER);
				newNode->str = wire->str;
				newNode->id2ast = wire;
				goto apply_newNode;
			}

			if (str == "\\$past")
			{
				if (width_hint < 0)
					goto replace_fcall_later;

				int num_steps = 1;

				if (fcall.num_args() != 1 && fcall.num_args() != 2)
					input_error("System function %s got %d arguments, expected 1 or 2.\n",
							RTLIL::unescape_id(str), int(fcall.num_args()));

				if (!current_always_clocked)
					input_error("System function %s is only allowed in clocked blocks.\n",
							RTLIL::unescape_id(str));

				if (fcall.num_args() == 2)
				{
					auto buf = fcall.arg(1)->clone();
					while (buf->simplify(true, stage, -1, false)) { }
					if (buf->type != AST_CONSTANT)
						input_error("Failed to evaluate system function `%s' with non-constant value.\n", str);

					num_steps = buf->asInt(true);
				}

				AstNode *block = nullptr;

				for (auto& child : current_always->children)
					if (child->type == AST_BLOCK)
						block = child.get();

				log_assert(block != nullptr);

				if (num_steps == 0) {
					newNode = fcall.arg(0)->clone();
					goto apply_newNode;
				}

				int myidx = autoidx++;
				AstNode* outreg = nullptr;

				for (int i = 0; i < num_steps; i++)
				{
					auto reg_owned = std::make_unique<AstNode>(location, AST_WIRE, std::make_unique<AstNode>(location, AST_RANGE,
							mkconst_int(location, width_hint-1, true), mkconst_int(location, 0, true)));
					auto* reg = reg_owned.get();
					current_ast_mod->children.push_back(std::move(reg_owned));

					reg->str = stringf("$past$%s:%d$%d$%d", RTLIL::encode_filename(*location.begin.filename), location.begin.line, myidx, i);
					reg->is_reg = true;
					reg->is_signed = sign_hint;


					while (reg->simplify()) { }

					auto regid = std::make_unique<AstNode>(location, AST_IDENTIFIER);
					regid->str = reg->str;
					regid->id2ast = reg;
					regid->was_checked = true;

					std::unique_ptr<AstNode> rhs = nullptr;

					if (outreg == nullptr) {
						rhs = fcall.arg(0)->clone();
					} else {
						rhs = std::make_unique<AstNode>(location, AST_IDENTIFIER);
						rhs->str = outreg->str;
						rhs->id2ast = outreg;
					}

					block->children.push_back(std::make_unique<AstNode>(location, AST_ASSIGN_LE, std::move(regid), std::move(rhs)));
					outreg = reg;
				}

				newNode = std::make_unique<AstNode>(location, AST_IDENTIFIER);
				newNode->str = outreg->str;
				newNode->id2ast = outreg;
				goto apply_newNode;
			}

			if (str == "\\$stable" || str == "\\$rose" || str == "\\$fell" || str == "\\$changed")
			{
				if (fcall.num_args() != 1)
					input_error("System function %s got %d arguments, expected 1.\n",
							RTLIL::unescape_id(str), int(fcall.num_args()));

				if (!current_always_clocked)
					input_error("System function %s is only allowed in clocked blocks.\n",
							RTLIL::unescape_id(str));

				auto present = fcall.arg(0)->clone();
				auto past = clone();
				past->str = "\\$past";

				if (str == "\\$stable")
					newNode = std::make_unique<AstNode>(location, AST_EQ, std::move(past), std::move(present));

				else if (str == "\\$changed")
					newNode = std::make_unique<AstNode>(location, AST_NE, std::move(past), std::move(present));

				else if (str == "\\$rose")
					newNode = std::make_unique<AstNode>(location, AST_LOGIC_AND,
							std::make_unique<AstNode>(location, AST_LOGIC_NOT, std::make_unique<AstNode>(location, AST_BIT_AND, std::move(past), mkconst_int(location, 1, false))),
							std::make_unique<AstNode>(location, AST_BIT_AND, std::move(present), mkconst_int(location, 1, false)));

				else if (str == "\\$fell")
					newNode = std::make_unique<AstNode>(location, AST_LOGIC_AND,
							std::make_unique<AstNode>(location, AST_BIT_AND, std::move(past), mkconst_int(location, 1, false)),
							std::make_unique<AstNode>(location, AST_LOGIC_NOT, std::make_unique<AstNode>(location, AST_BIT_AND, std::move(present), mkconst_int(location, 1, false))));

				else
					log_abort();

				goto apply_newNode;
			}

			// $anyconst and $anyseq are mapped in AstNode::genRTLIL()
			if (str == "\\$anyconst" || str == "\\$anyseq" || str == "\\$allconst" || str == "\\$allseq") {
				recursion_counter--;
				return false;
			}

			if (str == "\\$clog2")
			{
				if (fcall.num_args() != 1)
					input_error("System function %s got %d arguments, expected 1.\n",
							RTLIL::unescape_id(str), int(fcall.num_args()));

				auto buf = fcall.arg(0)->clone();
				while (buf->simplify(true, stage, width_hint, sign_hint)) { }
				if (buf->type != AST_CONSTANT)
					input_error("Failed to evaluate system function `%s' with non-constant value.\n", str);

				RTLIL::Const arg_value = buf->bitsAsConst();
				if (arg_value.as_bool())
					arg_value = const_sub(arg_value, 1, false, false, GetSize(arg_value));

				uint32_t result = 0;
				for (auto i = 0; i < arg_value.size(); i++)
					if (arg_value.at(i) == RTLIL::State::S1)
						result = i + 1;

				newNode = mkconst_int(location, result, true);
				goto apply_newNode;
			}

			if (str == "\\$dimensions" || str == "\\$unpacked_dimensions" ||
				str == "\\$increment" || str == "\\$size" || str == "\\$bits" || str == "\\$high" || str == "\\$low" || str == "\\$left" || str == "\\$right")
			{
				int dim = 1;
				if (str == "\\$dimensions" || str == "\\$unpacked_dimensions" || str == "\\$bits") {
					if (fcall.num_args() != 1)
						input_error("System function %s got %d arguments, expected 1.\n",
								RTLIL::unescape_id(str), int(fcall.num_args()));
				} else {
					if (fcall.num_args() != 1 && fcall.num_args() != 2)
						input_error("System function %s got %d arguments, expected 1 or 2.\n",
							RTLIL::unescape_id(str), int(fcall.num_args()));
					if (fcall.num_args() == 2) {
						auto buf = fcall.arg(1)->clone();
						// Evaluate constant expression
						while (buf->simplify(true, stage, width_hint, sign_hint)) { }
						dim = buf->asInt(false);
					}
				}
				auto buf = fcall.arg(0)->clone();
				int mem_depth = 1;
				int result, high = 0, low = 0, left = 0, right = 0, width = 1; // defaults for a simple wire
				int expr_dimensions = 0, expr_unpacked_dimensions = 0;
				AstNode *id_ast = nullptr;

				buf->detectSignWidth(width_hint, sign_hint);

				if (buf->type == AST_IDENTIFIER) {
					id_ast = buf->id2ast;
					if (id_ast == nullptr && current_scope.count(buf->str))
						id_ast = current_scope.at(buf->str);
					if (!id_ast)
						input_error("Failed to resolve identifier %s for width detection!\n", buf->str);

					if (WireOrMemory::accepts(id_ast)) {
						// Check for item in packed struct / union
						AstNode *item_node = buf->get_struct_member();
						if (item_node)
							id_ast = item_node;

						// The dimension of the original array expression is saved in the 'integer' field
						dim += buf->integer;

						int dims = GetSize(id_ast->dimensions);
						// TODO: IEEE Std 1800-2017 20.7: "If the first argument to an array query function would cause $dimensions to return 0
						// or if the second argument is out of range, then 'x shall be returned."
						if (dim < 1 || dim > dims)
							input_error("Dimension %d out of range in `%s', as it only has %d dimensions!\n", dim, id_ast->str, dims);

						expr_dimensions = dims - dim + 1;
						expr_unpacked_dimensions = std::max(id_ast->unpacked_dimensions - dim + 1, 0);

						right = low  = id_ast->dimensions[dim - 1].range_right;
						left  = high = low + id_ast->dimensions[dim - 1].range_width - 1;
						if (id_ast->dimensions[dim - 1].range_swapped) {
							std::swap(left, right);
						}
						for (int i = dim; i < dims; i++) {
							mem_depth *= id_ast->dimensions[i].range_width;
						}
					}
					width = high - low + 1;
				} else {
					width = width_hint;
					right = low  = 0;
					left  = high = width - 1;
					expr_dimensions = 1;
				}
				if (str == "\\$dimensions")
					result = expr_dimensions;
				else if (str == "\\$unpacked_dimensions")
					result = expr_unpacked_dimensions;
				else if (str == "\\$high")
					result = high;
				else if (str == "\\$low")
					result = low;
				else if (str == "\\$left")
					result = left;
				else if (str == "\\$right")
					result = right;
				else if (str == "\\$increment")
					result = left >= right ? 1 : -1;
				else if (str == "\\$size")
					result = width;
				else { // str == "\\$bits"
					result = width * mem_depth;
				}
				newNode = mkconst_int(location, result, true);
				goto apply_newNode;
			}

			if (str == "\\$ln" || str == "\\$log10" || str == "\\$exp" || str == "\\$sqrt" || str == "\\$pow" ||
					str == "\\$floor" || str == "\\$ceil" || str == "\\$sin" || str == "\\$cos" || str == "\\$tan" ||
					str == "\\$asin" || str == "\\$acos" || str == "\\$atan" || str == "\\$atan2" || str == "\\$hypot" ||
					str == "\\$sinh" || str == "\\$cosh" || str == "\\$tanh" || str == "\\$asinh" || str == "\\$acosh" || str == "\\$atanh" ||
					str == "\\$rtoi" || str == "\\$itor")
			{
				bool func_with_two_arguments = str == "\\$pow" || str == "\\$atan2" || str == "\\$hypot";
				double x = 0, y = 0;

				if (func_with_two_arguments) {
					if (fcall.num_args() != 2)
						input_error("System function %s got %d arguments, expected 2.\n",
								RTLIL::unescape_id(str), int(fcall.num_args()));
				} else {
					if (fcall.num_args() != 1)
						input_error("System function %s got %d arguments, expected 1.\n",
								RTLIL::unescape_id(str), int(fcall.num_args()));
				}

				if (fcall.num_args() >= 1) {
					AstNode *a0 = fcall.arg(0);
					while (a0->simplify(true, stage, width_hint, sign_hint)) { }
					if (!a0->isConst())
						input_error("Failed to evaluate system function `%s' with non-constant argument.\n",
								RTLIL::unescape_id(str));
					int child_width_hint = width_hint;
					bool child_sign_hint = sign_hint;
					a0->detectSignWidth(child_width_hint, child_sign_hint);
					x = a0->asReal(child_sign_hint);
				}

				if (fcall.num_args() >= 2) {
					AstNode *a1 = fcall.arg(1);
					while (a1->simplify(true, stage, width_hint, sign_hint)) { }
					if (!a1->isConst())
						input_error("Failed to evaluate system function `%s' with non-constant argument.\n",
								RTLIL::unescape_id(str));
					int child_width_hint = width_hint;
					bool child_sign_hint = sign_hint;
					a1->detectSignWidth(child_width_hint, child_sign_hint);
					y = a1->asReal(child_sign_hint);
				}

				if (str == "\\$rtoi") {
					newNode = AstNode::mkconst_int(location, x, true);
				} else {
					newNode = std::make_unique<AstNode>(location, AST_REALVALUE);
					if (str == "\\$ln")		 newNode->realvalue = ::log(x);
					else if (str == "\\$log10") newNode->realvalue = ::log10(x);
					else if (str == "\\$exp")   newNode->realvalue = ::exp(x);
					else if (str == "\\$sqrt")  newNode->realvalue = ::sqrt(x);
					else if (str == "\\$pow")   newNode->realvalue = ::pow(x, y);
					else if (str == "\\$floor") newNode->realvalue = ::floor(x);
					else if (str == "\\$ceil")  newNode->realvalue = ::ceil(x);
					else if (str == "\\$sin")   newNode->realvalue = ::sin(x);
					else if (str == "\\$cos")   newNode->realvalue = ::cos(x);
					else if (str == "\\$tan")   newNode->realvalue = ::tan(x);
					else if (str == "\\$asin")  newNode->realvalue = ::asin(x);
					else if (str == "\\$acos")  newNode->realvalue = ::acos(x);
					else if (str == "\\$atan")  newNode->realvalue = ::atan(x);
					else if (str == "\\$atan2") newNode->realvalue = ::atan2(x, y);
					else if (str == "\\$hypot") newNode->realvalue = ::hypot(x, y);
					else if (str == "\\$sinh")  newNode->realvalue = ::sinh(x);
					else if (str == "\\$cosh")  newNode->realvalue = ::cosh(x);
					else if (str == "\\$tanh")  newNode->realvalue = ::tanh(x);
					else if (str == "\\$asinh") newNode->realvalue = ::asinh(x);
					else if (str == "\\$acosh") newNode->realvalue = ::acosh(x);
					else if (str == "\\$atanh") newNode->realvalue = ::atanh(x);
					else if (str == "\\$itor")  newNode->realvalue = x;
					else log_abort();
				}
				goto apply_newNode;
			}

			if (str == "\\$sformatf") {
				Fmt fmt = processFormat(stage, /*sformat_like=*/true);
				newNode = AstNode::mkconst_str(location, fmt.render());
				goto apply_newNode;
			}

			if (str == "\\$countbits") {
				if (fcall.num_args() < 2)
					input_error("System function %s got %d arguments, expected at least 2.\n",
							RTLIL::unescape_id(str), int(fcall.num_args()));

				std::vector<RTLIL::State> control_bits;

				// Determine which bits to count
				for (size_t i = 1; i < fcall.num_args(); i++) {
					AstNode *node = fcall.arg(i);
					while (node->simplify(true, stage, -1, false)) { }
					if (node->type != AST_CONSTANT)
						input_error("Failed to evaluate system function `%s' with non-constant control bit argument.\n", str);
					if (node->bits.size() != 1)
						input_error("Failed to evaluate system function `%s' with control bit width != 1.\n", str);
					control_bits.push_back(node->bits[0]);
				}

				// Detect width of exp (first argument of $countbits)
				int  exp_width = -1;
				bool exp_sign  = false;
				AstNode *exp = fcall.arg(0);
				exp->detectSignWidth(exp_width, exp_sign, nullptr);

				newNode = mkconst_int(location, 0, false);

				for (int i = 0; i < exp_width; i++) {
					// Generate nodes for:  exp << i >> ($size(exp) - 1)
					//						  ^^   ^^
					auto lsh_node = std::make_unique<AstNode>(location, AST_SHIFT_LEFT, exp->clone(), mkconst_int(location, i, false));
					auto rsh_node = std::make_unique<AstNode>(location, AST_SHIFT_RIGHT, std::move(lsh_node), mkconst_int(location, exp_width - 1, false));

					std::unique_ptr<AstNode> or_node = nullptr;

					for (RTLIL::State control_bit : control_bits) {
						// Generate node for:  (exp << i >> ($size(exp) - 1)) === control_bit
						//													^^^
						auto eq_node = std::make_unique<AstNode>(location, AST_EQX, rsh_node->clone(), mkconst_bits(location, {control_bit}, false));

						// Or the result for each checked bit value
						if (or_node)
							or_node = std::make_unique<AstNode>(location, AST_LOGIC_OR, std::move(or_node), std::move(eq_node));
						else
							or_node = std::move(eq_node);
					}

					// We should have at least one element in control_bits,
					// because we checked for the number of arguments above
					log_assert(or_node != nullptr);

					// Generate node for adding with result of previous bit
					newNode = std::make_unique<AstNode>(location, AST_ADD, std::move(newNode), std::move(or_node));
				}

				goto apply_newNode;
			}

			if (str == "\\$countones" || str == "\\$isunknown" || str == "\\$onehot" || str == "\\$onehot0") {
				if (fcall.num_args() != 1)
					input_error("System function %s got %d arguments, expected 1.\n",
							RTLIL::unescape_id(str), int(fcall.num_args()));

				auto countbits = clone();
				countbits->str = "\\$countbits";

				if (str == "\\$countones") {
					countbits->children.push_back(mkconst_bits(location, {RTLIL::State::S1}, false));
					newNode = std::move(countbits);
				} else if (str == "\\$isunknown") {
					countbits->children.push_back(mkconst_bits(location, {RTLIL::Sx}, false));
					countbits->children.push_back(mkconst_bits(location, {RTLIL::Sz}, false));
					newNode = std::make_unique<AstNode>(location, AST_GT, std::move(countbits), mkconst_int(location, 0, false));
				} else if (str == "\\$onehot") {
					countbits->children.push_back(mkconst_bits(location, {RTLIL::State::S1}, false));
					newNode = std::make_unique<AstNode>(location, AST_EQ, std::move(countbits), mkconst_int(location, 1, false));
				} else if (str == "\\$onehot0") {
					countbits->children.push_back(mkconst_bits(location, {RTLIL::State::S1}, false));
					newNode = std::make_unique<AstNode>(location, AST_LE, std::move(countbits), mkconst_int(location, 1, false));
				} else {
					log_abort();
				}

				goto apply_newNode;
			}

			if (current_scope.count(str) != 0 && current_scope[str]->type == AST_DPI_FUNCTION)
			{
				AstNode *dpi_decl = current_scope[str];

				std::string rtype, fname;
				std::vector<std::string> argtypes;
				std::vector<std::unique_ptr<AstNode>> args;

				rtype = RTLIL::unescape_id(dpi_decl->children.at(0)->str);
				fname = RTLIL::unescape_id(dpi_decl->children.at(1)->str);

				for (int i = 2; i < GetSize(dpi_decl->children); i++)
				{
					if (size_t(i-2) >= fcall.num_args())
						input_error("Insufficient number of arguments in DPI function call.\n");

					argtypes.push_back(RTLIL::unescape_id(dpi_decl->children.at(i)->str));
					args.push_back(fcall.arg(i-2)->clone());
					while (args.back()->simplify(true, stage, -1, false)) { }

					if (args.back()->type != AST_CONSTANT && args.back()->type != AST_REALVALUE)
						input_error("Failed to evaluate DPI function with non-constant argument.\n");
				}

				newNode = dpi_call(dpi_decl->location, rtype, fname, argtypes, args);

				goto apply_newNode;
			}

			if (current_scope.count(str) == 0)
				str = try_pop_module_prefix();
			if (current_scope.count(str) == 0 || current_scope[str]->type != AST_FUNCTION)
				input_error("Can't resolve function name `%s'.\n", str);
		}

		if (type == AST_TCALL)
		{
			AstTcall tcall(this);
			if (str == "$finish" || str == "$stop")
			{
				if (!current_always || !AstInitial::matches(current_always))
					input_error("System task `%s' outside initial block is unsupported.\n", str);

				input_error("System task `%s' executed.\n", str);
			}

			if (str == "\\$readmemh" || str == "\\$readmemb")
			{
				if (tcall.num_args() < 2 || tcall.num_args() > 4)
					input_error("System function %s got %d arguments, expected 2-4.\n",
							RTLIL::unescape_id(str), int(tcall.num_args()));

				auto node_filename = tcall.arg(0)->clone();
				while (node_filename->simplify(true, stage, width_hint, sign_hint)) { }
				if (node_filename->type != AST_CONSTANT)
					input_error("Failed to evaluate system function `%s' with non-constant 1st argument.\n", str);

				auto node_memory = tcall.arg(1)->clone();
				while (node_memory->simplify(true, stage, width_hint, sign_hint)) { }
				if (node_memory->type != AST_IDENTIFIER || node_memory->id2ast == nullptr || node_memory->id2ast->type != AST_MEMORY)
					input_error("Failed to evaluate system function `%s' with non-memory 2nd argument.\n", str);

				int start_addr = -1, finish_addr = -1;

				if (tcall.num_args() > 2) {
					auto node_addr = tcall.arg(2)->clone();
					while (node_addr->simplify(true, stage, width_hint, sign_hint)) { }
					if (node_addr->type != AST_CONSTANT)
						input_error("Failed to evaluate system function `%s' with non-constant 3rd argument.\n", str);
					start_addr = int(node_addr->asInt(false));
				}

				if (tcall.num_args() > 3) {
					auto node_addr = tcall.arg(3)->clone();
					while (node_addr->simplify(true, stage, width_hint, sign_hint)) { }
					if (node_addr->type != AST_CONSTANT)
						input_error("Failed to evaluate system function `%s' with non-constant 4th argument.\n", str);
					finish_addr = int(node_addr->asInt(false));
				}

				bool unconditional_init = false;
				if (AstInitial::matches(current_always)) {
					pool<AstNode*> queue;
					log_assert(current_always->children[0]->type == AST_BLOCK);
					queue.insert(current_always->children[0].get());
					while (!unconditional_init && !queue.empty()) {
						pool<AstNode*> next_queue;
						for (auto& n : queue)
						for (auto& c : n->children) {
							if (c.get() == this)
								unconditional_init = true;
							next_queue.insert(c.get());
						}
						next_queue.swap(queue);
					}
				}

				newNode = readmem(str == "\\$readmemh", node_filename->bitsAsConst().decode_string(), node_memory->id2ast, start_addr, finish_addr, unconditional_init);
				goto apply_newNode;
			}

			if (current_scope.count(str) == 0)
				str = try_pop_module_prefix();
			if (current_scope.count(str) == 0 || current_scope[str]->type != AST_TASK)
				input_error("Can't resolve task name `%s'.\n", str);
		}


		std::stringstream sstr;
		sstr << str << "$func$" << RTLIL::encode_filename(*location.begin.filename) << ":" << location.begin.line << "$" << (autoidx++) << '.';
		std::string prefix = sstr.str();

		auto* decl = current_scope[str];
		if (unevaluated_tern_branch && decl->is_recursive_function())
			goto replace_fcall_later;
		auto decl_clone = decl->clone();
		decl = decl_clone.get(); // sketchy?
		decl->replace_result_wire_name_in_function(str, "$result"); // enables recursion
		decl->expand_genblock(prefix);

		if (decl->type == AST_FUNCTION && !decl->attributes.count(ID::via_celltype))
		{
			bool require_const_eval = decl->has_const_only_constructs();
			bool all_args_const = true;
			for (auto& child : children) {
				while (child->simplify()) { }
				if (child->type != AST_CONSTANT && child->type != AST_REALVALUE)
					all_args_const = false;
			}

			if (all_args_const) {
				auto func_workspace = decl->clone();
				func_workspace->set_in_param_flag(true);
				func_workspace->str = prefix_id(prefix, "$result");
				// func_workspace->dumpAst(stdout, "func_workspace ");
				newNode = func_workspace->eval_const_function(this, in_param || require_const_eval);
				if (newNode) {
					goto apply_newNode;
				}
			}

			if (in_param)
				input_error("Non-constant function call in constant expression.\n");
			if (require_const_eval)
				input_error("Function %s can only be called with constant arguments.\n", str);
		}

		size_t arg_count = 0;
		dict<std::string, AstNode*> wire_cache;
		vector<std::unique_ptr<AstNode>> new_stmts;
		vector<std::unique_ptr<AstNode>> output_assignments;

		if (current_block == nullptr)
		{
			log_assert(type == AST_FCALL);

			std::unique_ptr<AstNode> wire = nullptr;
			std::string res_name = prefix_id(prefix, "$result");
			for (auto& child : decl->children)
				if (child->type == AST_WIRE && child->str == res_name)
					wire = child->clone();
			log_assert(wire != nullptr);

			wire->port_id = 0;
			wire->is_input = false;
			wire->is_output = false;

			auto* wire_leaky = wire.get();
			current_scope[wire->str] = wire_leaky;
			current_ast_mod->children.push_back(std::move(wire));
			while (wire_leaky->simplify()) { }

			auto lvalue = std::make_unique<AstNode>(location, AST_IDENTIFIER);
			lvalue->str = wire_leaky->str;

			auto always = std::make_unique<AstNode>(location, AST_ALWAYS, std::make_unique<AstNode>(location, AST_BLOCK,
					std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::move(lvalue), clone())));
			always->children[0]->children[0]->was_checked = true;

			current_ast_mod->children.push_back(std::move(always));

			goto replace_fcall_with_id;
		}

		if (decl->attributes.count(ID::via_celltype))
		{
			std::string celltype = decl->attributes.at(ID::via_celltype)->asAttrConst().decode_string();
			std::string outport = str;

			if (celltype.find(' ') != std::string::npos) {
				int pos = celltype.find(' ');
				outport = RTLIL::escape_id(celltype.substr(pos+1));
				celltype = RTLIL::escape_id(celltype.substr(0, pos));
			} else
				celltype = RTLIL::escape_id(celltype);

			auto cell = std::make_unique<AstNode>(location, AST_CELL, std::make_unique<AstNode>(location, AST_CELLTYPE));
			cell->str = prefix.substr(0, GetSize(prefix)-1);
			cell->children[0]->str = celltype;

			for (auto& attr : decl->attributes)
				if (attr.first.str().rfind("\\via_celltype_defparam_", 0) == 0)
				{
					auto cell_arg = std::make_unique<AstNode>(location, AST_PARASET, attr.second->clone());
					cell_arg->str = RTLIL::escape_id(attr.first.substr(strlen("\\via_celltype_defparam_")));
					cell->children.push_back(std::move(cell_arg));
				}

			for (auto& child : decl->children)
				if (child->type == AST_WIRE && (child->is_input || child->is_output || (type == AST_FCALL && child->str == str)))
				{
					auto wire = child->clone();
					wire->port_id = 0;
					wire->is_input = false;
					wire->is_output = false;
					current_ast_mod->children.push_back(std::move(wire));
					while (wire->simplify()) { }

					auto wire_id = std::make_unique<AstNode>(location, AST_IDENTIFIER);
					wire_id->str = wire->str;

					if ((child->is_input || child->is_output) && arg_count < children.size())
					{
						auto arg = children[arg_count++]->clone();
						auto assign = child->is_input ?
								std::make_unique<AstNode>(location, AST_ASSIGN_EQ, wire_id->clone(), std::move(arg)) :
								std::make_unique<AstNode>(location, AST_ASSIGN_EQ, std::move(arg), wire_id->clone());
						assign->children[0]->was_checked = true;

						for (auto it = current_block->children.begin(); it != current_block->children.end(); it++) {
							if (it->get() != current_block_child)
								continue;
							current_block->children.insert(it, std::move(assign));
							break;
						}
					}

					auto cell_arg = std::make_unique<AstNode>(location, AST_ARGUMENT, std::move(wire_id));
					cell_arg->str = child->str == str ? outport : child->str;
					cell->children.push_back(std::move(cell_arg));
				}

			current_ast_mod->children.push_back(std::move(cell));
			goto replace_fcall_with_id;
		}

		// Function/task locals declared in the decl: wires, memories, parameters,
		// localparams, and enum items. Each is materialized as a wire in the
		// surrounding module scope, with input/output args wired to assignments.
		using DeclLocalConstraint = ChildConstraint<AST_WIRE, AST_MEMORY, AST_PARAMETER, AST_LOCALPARAM, AST_ENUM_ITEM>;

		for (auto& child : decl->children)
			if (DeclLocalConstraint::accepts(child.get()))
			{
				AstNode *wire = nullptr;

				if (wire_cache.count(child->str))
				{
					wire = wire_cache.at(child->str);
					bool contains_value = wire->type == AST_LOCALPARAM;
					if (wire->children.size() == contains_value) {
						for (auto& c : child->children)
							wire->children.push_back(c->clone());
					} else if (!child->children.empty()) {
						while (child->simplify(true, stage, -1, false)) { }
						if (GetSize(child->children) == GetSize(wire->children) - contains_value) {
							for (int i = 0; i < GetSize(child->children); i++)
								if (*child->children.at(i) != *wire->children.at(i + contains_value))
									goto tcall_incompatible_wires;
						} else {
					tcall_incompatible_wires:
							input_error("Incompatible re-declaration of wire %s.\n", child->str);
						}
					}
				}
				else
				{
					current_ast_mod->children.push_back(child->clone());
					wire = current_ast_mod->children.back().get();
					wire->port_id = 0;
					wire->is_input = false;
					wire->is_output = false;
					wire->is_reg = true;
					wire->set_attribute(ID::nosync, AstNode::mkconst_int(location, 1, false));
					if (child->type == AST_ENUM_ITEM)
						wire->set_attribute(ID::enum_base_type, std::move(child->attributes[ID::enum_base_type]));

					wire_cache[child->str] = wire;

					current_scope[wire->str] = wire;
				}

				while (wire->simplify()) { }

				if ((child->is_input || child->is_output) && arg_count < children.size())
				{
					auto arg = children[arg_count++]->clone();
					// convert purely constant arguments into localparams
					if (child->is_input && child->type == AST_WIRE && arg->type == AST_CONSTANT && node_contains_assignment_to(decl, child.get())) {
						wire->type = AST_LOCALPARAM;
						wire->attributes.erase(ID::nosync);
						wire->children.insert(wire->children.begin(), arg->clone());
						// args without a range implicitly have width 1
						if (wire->children.back()->type != AST_RANGE) {
							// check if this wire is redeclared with an explicit size
							bool uses_explicit_size = false;
							for (auto& other_child : decl->children)
								if (other_child->type == AST_WIRE && child->str == other_child->str
										&& !other_child->children.empty()
										&& other_child->children.back()->type == AST_RANGE) {
									uses_explicit_size = true;
									break;
								}
							if (!uses_explicit_size) {
								auto range = std::make_unique<AstNode>(location);
								range->type = AST_RANGE;
								range->children.push_back(mkconst_int(location, 0, true));
								range->children.push_back(mkconst_int(location, 0, true));
								wire->children.push_back(std::move(range));
							}
						}
						wire->fixup_hierarchy_flags();
						// updates the sizing
						while (wire->simplify()) { }
						continue;
					}

					auto wire_id = std::make_unique<AstNode>(location, AST_IDENTIFIER);
					wire_id->str = wire->str;
					if (child->is_input) {
						auto assign = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, wire_id->clone(), arg->clone());
						assign->children[0]->was_checked = true;
						new_stmts.push_back(std::move(assign));
					}

					if (child->is_output) {
						auto assign = std::make_unique<AstNode>(location, AST_ASSIGN_EQ, arg->clone(), wire_id->clone());
						assign->children[0]->was_checked = true;
						output_assignments.push_back(std::move(assign));
					}
				}
			}

		// Note: AST_ENUM_ITEM is intentionally not excluded here; the original
		// emitter clones it as a body statement (in addition to materializing it
		// as a wire above) to register the enum value attribute. Preserve that.
		using DeclWireDecl = ChildConstraint<AST_WIRE, AST_MEMORY, AST_PARAMETER, AST_LOCALPARAM>;
		for (auto& child : decl->children)
			if (!DeclWireDecl::accepts(child.get()))
				new_stmts.push_back(child->clone());

		new_stmts.reserve(new_stmts.size() + output_assignments.size());
		std::move(output_assignments.begin(), output_assignments.end(), std::back_inserter(new_stmts));

		for (auto it = current_block->children.begin(); ; it++) {
			log_assert(it != current_block->children.end());
			if (it->get() == current_block_child) {
				current_block->children.insert(it,
					std::make_move_iterator(new_stmts.begin()),
					std::make_move_iterator(new_stmts.end()));
				break;
			}
		}

	replace_fcall_with_id:
		if (type == AST_FCALL) {
			// FCALL → IDENTIFIER: reshape discards the arg children and yields a bare id.
			reshape_as<AST_IDENTIFIER>(this);
			str = prefix_id(prefix, "$result");
		}
		if (type == AST_TCALL)
			str = "";
		did_something = true;
	}

replace_fcall_later:;

	// perform const folding when activated
	if (const_fold)
	{
		bool string_op;
		std::vector<RTLIL::State> tmp_bits;
		RTLIL::Const (*const_func)(const RTLIL::Const&, const RTLIL::Const&, bool, bool, int);
		RTLIL::Const dummy_arg;

		switch (type)
		{
		case AST_IDENTIFIER:
			if (auto it = current_scope.find(str); it != current_scope.end())
			if (auto param = AstAnyParamLike::cast(it->second)) {
				AstNode *param_node = param->node;
				AstNode *param_value = param->value();
				if (param_value->type == AST_CONSTANT) {
					if (children.size() != 0 && children[0]->type == AST_RANGE && children[0]->range_valid) {
						std::vector<RTLIL::State> data;
						bool param_upto = param_node->range_valid && param_node->range_swapped;
						int param_offset = param_node->range_valid ? param_node->range_right : 0;
						int param_width = param_node->range_valid ? param_node->range_left - param_node->range_right + 1 :
								GetSize(param_value->bits);
						int tmp_range_left = children[0]->range_left, tmp_range_right = children[0]->range_right;
						if (param_upto) {
							tmp_range_left = (param_width + 2*param_offset) - children[0]->range_right - 1;
							tmp_range_right = (param_width + 2*param_offset) - children[0]->range_left - 1;
						}
						AstNode *member_node = get_struct_member();
						int chunk_offset = member_node ? member_node->range_right : 0;
						log_assert(!(chunk_offset && param_upto));
						for (int i = tmp_range_right; i <= tmp_range_left; i++) {
							int index = i - param_offset;
							if (0 <= index && index < param_width)
								data.push_back(param_value->bits[chunk_offset + index]);
							else
								data.push_back(RTLIL::State::Sx);
						}
						newNode = mkconst_bits(location, data, false);
					} else
					if (children.size() == 0)
						newNode = param_value->clone();
				} else
				if (param_value->isConst())
					newNode = param_value->clone();
			}
			break;
		case AST_BIT_NOT:
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = RTLIL::const_not(children[0]->bitsAsConst(width_hint, sign_hint), dummy_arg, sign_hint, false, width_hint);
				newNode = mkconst_bits(location, y.to_bits(), sign_hint);
			}
			break;
		case AST_TO_SIGNED:
		case AST_TO_UNSIGNED:
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = children[0]->bitsAsConst(width_hint, sign_hint);
				newNode = mkconst_bits(location, y.to_bits(), type == AST_TO_SIGNED);
			}
			break;
		if (0) { case AST_BIT_AND:  const_func = RTLIL::const_and;  }
		if (0) { case AST_BIT_OR:   const_func = RTLIL::const_or;   }
		if (0) { case AST_BIT_XOR:  const_func = RTLIL::const_xor;  }
		if (0) { case AST_BIT_XNOR: const_func = RTLIL::const_xnor; }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(children[0]->bitsAsConst(width_hint, sign_hint),
						children[1]->bitsAsConst(width_hint, sign_hint), sign_hint, sign_hint, width_hint);
				newNode = mkconst_bits(location, y.to_bits(), sign_hint);
			}
			break;
		if (0) { case AST_REDUCE_AND:  const_func = RTLIL::const_reduce_and;  }
		if (0) { case AST_REDUCE_OR:   const_func = RTLIL::const_reduce_or;   }
		if (0) { case AST_REDUCE_XOR:  const_func = RTLIL::const_reduce_xor;  }
		if (0) { case AST_REDUCE_XNOR: const_func = RTLIL::const_reduce_xnor; }
		if (0) { case AST_REDUCE_BOOL: const_func = RTLIL::const_reduce_bool; }
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(RTLIL::Const(children[0]->bits), dummy_arg, false, false, -1);
				newNode = mkconst_bits(location, y.to_bits(), false);
			}
			break;
		case AST_LOGIC_NOT:
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = RTLIL::const_logic_not(RTLIL::Const(children[0]->bits), dummy_arg, children[0]->is_signed, false, -1);
				newNode = mkconst_bits(location, y.to_bits(), false);
			} else
			if (children[0]->isConst()) {
				newNode = mkconst_int(location, children[0]->asReal(sign_hint) == 0, false, 1);
			}
			break;
		if (0) { case AST_LOGIC_AND: const_func = RTLIL::const_logic_and; }
		if (0) { case AST_LOGIC_OR:  const_func = RTLIL::const_logic_or;  }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(RTLIL::Const(children[0]->bits), RTLIL::Const(children[1]->bits),
						children[0]->is_signed, children[1]->is_signed, -1);
				newNode = mkconst_bits(location, y.to_bits(), false);
			} else
			if (children[0]->isConst() && children[1]->isConst()) {
				if (type == AST_LOGIC_AND)
					newNode = mkconst_int(location, (children[0]->asReal(sign_hint) != 0) && (children[1]->asReal(sign_hint) != 0), false, 1);
				else
					newNode = mkconst_int(location, (children[0]->asReal(sign_hint) != 0) || (children[1]->asReal(sign_hint) != 0), false, 1);
			}
			break;
		if (0) { case AST_SHIFT_LEFT:   const_func = RTLIL::const_shl;  }
		if (0) { case AST_SHIFT_RIGHT:  const_func = RTLIL::const_shr;  }
		if (0) { case AST_SHIFT_SLEFT:  const_func = RTLIL::const_sshl; }
		if (0) { case AST_SHIFT_SRIGHT: const_func = RTLIL::const_sshr; }
		if (0) { case AST_POW:		  const_func = RTLIL::const_pow; }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(children[0]->bitsAsConst(width_hint, sign_hint),
						RTLIL::Const(children[1]->bits), sign_hint, type == AST_POW ? children[1]->is_signed : false, width_hint);
				newNode = mkconst_bits(location, y.to_bits(), sign_hint);
			} else
			if (type == AST_POW && children[0]->isConst() && children[1]->isConst()) {
				newNode = std::make_unique<AstNode>(location, AST_REALVALUE);
				newNode->realvalue = pow(children[0]->asReal(sign_hint), children[1]->asReal(sign_hint));
			}
			break;
		if (0) { case AST_LT:  const_func = RTLIL::const_lt; }
		if (0) { case AST_LE:  const_func = RTLIL::const_le; }
		if (0) { case AST_EQ:  const_func = RTLIL::const_eq; }
		if (0) { case AST_NE:  const_func = RTLIL::const_ne; }
		if (0) { case AST_EQX: const_func = RTLIL::const_eqx; }
		if (0) { case AST_NEX: const_func = RTLIL::const_nex; }
		if (0) { case AST_GE:  const_func = RTLIL::const_ge; }
		if (0) { case AST_GT:  const_func = RTLIL::const_gt; }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				int cmp_width = max(children[0]->bits.size(), children[1]->bits.size());
				bool cmp_signed = children[0]->is_signed && children[1]->is_signed;
				RTLIL::Const y = const_func(children[0]->bitsAsConst(cmp_width, cmp_signed),
						children[1]->bitsAsConst(cmp_width, cmp_signed), cmp_signed, cmp_signed, 1);
				newNode = mkconst_bits(location, y.to_bits(), false);
			} else
			if (children[0]->isConst() && children[1]->isConst()) {
				bool cmp_signed = (children[0]->type == AST_REALVALUE || children[0]->is_signed) && (children[1]->type == AST_REALVALUE || children[1]->is_signed);
				switch (type) {
				case AST_LT:  newNode = mkconst_int(location, children[0]->asReal(cmp_signed) <  children[1]->asReal(cmp_signed), false, 1); break;
				case AST_LE:  newNode = mkconst_int(location, children[0]->asReal(cmp_signed) <= children[1]->asReal(cmp_signed), false, 1); break;
				case AST_EQ:  newNode = mkconst_int(location, children[0]->asReal(cmp_signed) == children[1]->asReal(cmp_signed), false, 1); break;
				case AST_NE:  newNode = mkconst_int(location, children[0]->asReal(cmp_signed) != children[1]->asReal(cmp_signed), false, 1); break;
				case AST_EQX: newNode = mkconst_int(location, children[0]->asReal(cmp_signed) == children[1]->asReal(cmp_signed), false, 1); break;
				case AST_NEX: newNode = mkconst_int(location, children[0]->asReal(cmp_signed) != children[1]->asReal(cmp_signed), false, 1); break;
				case AST_GE:  newNode = mkconst_int(location, children[0]->asReal(cmp_signed) >= children[1]->asReal(cmp_signed), false, 1); break;
				case AST_GT:  newNode = mkconst_int(location, children[0]->asReal(cmp_signed) >  children[1]->asReal(cmp_signed), false, 1); break;
				default: log_abort();
				}
			}
			break;
		if (0) { case AST_ADD: const_func = RTLIL::const_add; }
		if (0) { case AST_SUB: const_func = RTLIL::const_sub; }
		if (0) { case AST_MUL: const_func = RTLIL::const_mul; }
		if (0) { case AST_DIV: const_func = RTLIL::const_div; }
		if (0) { case AST_MOD: const_func = RTLIL::const_mod; }
			if (children[0]->type == AST_CONSTANT && children[1]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(children[0]->bitsAsConst(width_hint, sign_hint),
						children[1]->bitsAsConst(width_hint, sign_hint), sign_hint, sign_hint, width_hint);
				newNode = mkconst_bits(location, y.to_bits(), sign_hint);
			} else
			if (children[0]->isConst() && children[1]->isConst()) {
				newNode = std::make_unique<AstNode>(location, AST_REALVALUE);
				switch (type) {
				case AST_ADD: newNode->realvalue = children[0]->asReal(sign_hint) + children[1]->asReal(sign_hint); break;
				case AST_SUB: newNode->realvalue = children[0]->asReal(sign_hint) - children[1]->asReal(sign_hint); break;
				case AST_MUL: newNode->realvalue = children[0]->asReal(sign_hint) * children[1]->asReal(sign_hint); break;
				case AST_DIV: newNode->realvalue = children[0]->asReal(sign_hint) / children[1]->asReal(sign_hint); break;
				case AST_MOD: newNode->realvalue = fmod(children[0]->asReal(sign_hint), children[1]->asReal(sign_hint)); break;
				default: log_abort();
				}
			}
			break;
		if (0) { case AST_SELFSZ: const_func = RTLIL::const_pos; }
		if (0) { case AST_POS: const_func = RTLIL::const_pos; }
		if (0) { case AST_NEG: const_func = RTLIL::const_neg; }
			if (children[0]->type == AST_CONSTANT) {
				RTLIL::Const y = const_func(children[0]->bitsAsConst(width_hint, sign_hint), dummy_arg, sign_hint, false, width_hint);
				newNode = mkconst_bits(location, y.to_bits(), sign_hint);
			} else
			if (children[0]->isConst()) {
				newNode = std::make_unique<AstNode>(location, AST_REALVALUE);
				if (type == AST_NEG)
					newNode->realvalue = -children[0]->asReal(sign_hint);
				else
					newNode->realvalue = +children[0]->asReal(sign_hint);
			}
			break;
		case AST_TERNARY: {
			AstTernary tern(this);
			AstNode *cond_n = tern.cond().get();
			AstNode *then_n = tern.then_().get();
			AstNode *else_n = tern.else_().get();
			if (cond_n->isConst())
			{
				auto pair = get_tern_choice();
				AstNode *choice = pair.first;
				AstNode *not_choice = pair.second;

				if (choice != nullptr) {
					if (choice->type == AST_CONSTANT) {
						int other_width_hint = width_hint;
						bool other_sign_hint = sign_hint, other_real = false;
						not_choice->detectSignWidth(other_width_hint, other_sign_hint, &other_real);
						if (other_real) {
							newNode = std::make_unique<AstNode>(location, AST_REALVALUE);
							choice->detectSignWidth(width_hint, sign_hint);
							newNode->realvalue = choice->asReal(sign_hint);
						} else {
							RTLIL::Const y = choice->bitsAsConst(width_hint, sign_hint);
							if (choice->is_string && y.size() % 8 == 0 && sign_hint == false)
								newNode = mkconst_str(location, y.to_bits());
							else
								newNode = mkconst_bits(location, y.to_bits(), sign_hint);
						}
					} else
					if (choice->isConst()) {
						newNode = choice->clone();
					}
				} else if (then_n->type == AST_CONSTANT && else_n->type == AST_CONSTANT) {
					RTLIL::Const a = then_n->bitsAsConst(width_hint, sign_hint);
					RTLIL::Const b = else_n->bitsAsConst(width_hint, sign_hint);
					log_assert(a.size() == b.size());
					for (auto i = 0; i < a.size(); i++)
						if (a[i] != b[i])
							a.set(i, RTLIL::State::Sx);
					newNode = mkconst_bits(location, a.to_bits(), sign_hint);
				} else if (then_n->isConst() && else_n->isConst()) {
					newNode = std::make_unique<AstNode>(location, AST_REALVALUE);
					if (then_n->asReal(sign_hint) == else_n->asReal(sign_hint))
						newNode->realvalue = then_n->asReal(sign_hint);
					else
						// IEEE Std 1800-2012 Sec. 11.4.11 states that the entry in Table 7-1 for
						// the data type in question should be returned if the ?: is ambiguous. The
						// value in Table 7-1 for the 'real' type is 0.0.
						newNode->realvalue = 0.0;
				}
			}
			break;
		}
		case AST_CAST_SIZE: {
			AstCastSize cast_view(this);
			AstNode *target = cast_view.target().get();
			AstNode *expr   = cast_view.expr().get();
			if (target->type == AST_CONSTANT && expr->type == AST_CONSTANT) {
				int width = target->bitsAsConst().as_int();
				RTLIL::Const val = expr->is_unsized
					? expr->bitsAsUnsizedConst(width)
					: expr->bitsAsConst(width);
				newNode = mkconst_bits(location, val.to_bits(), expr->is_signed);
			}
			break;
		}
		case AST_CONCAT: {
			AstConcat cat(this);
			string_op = !cat.empty();
			for (auto& part_owned : cat) {
				AstNode *part = part_owned.get();
				if (part->type != AST_CONSTANT)
					goto not_const;
				if (!part->is_string)
					string_op = false;
				tmp_bits.insert(tmp_bits.end(), part->bits.begin(), part->bits.end());
			}
			newNode = string_op ? mkconst_str(location, tmp_bits) : mkconst_bits(location, tmp_bits, false);
			break;
		}
		case AST_REPLICATE: {
			AstReplicate rep(this);
			AstNode *count_n = rep.count().get();
			AstNode *pat_n   = rep.pattern().get();
			if (count_n->type != AST_CONSTANT || pat_n->type != AST_CONSTANT)
				goto not_const;
			for (int i = 0; i < count_n->bitsAsConst().as_int(); i++)
				tmp_bits.insert(tmp_bits.end(), pat_n->bits.begin(), pat_n->bits.end());
			newNode = pat_n->is_string ? mkconst_str(location, tmp_bits) : mkconst_bits(location, tmp_bits, false);
			break;
		}
		default:
		not_const:
			break;
		}
	}

	// if any of the above set 'newNode' -> use 'newNode' as template to update 'this'
	if (newNode) {
apply_newNode:
		// fprintf(stderr, "----\n");
		// dumpAst(stderr, "- ");
		// newNode->dumpAst(stderr, "+ ");
		log_assert(newNode != nullptr);
		newNode->location.begin.filename = location.begin.filename;
		newNode->location = location;
		newNode->cloneInto(*this);
		fixup_hierarchy_flags();
		did_something = true;
	}

	if (!did_something)
		basic_prep = true;

	recursion_counter--;
	return did_something;
}

YOSYS_NAMESPACE_END
