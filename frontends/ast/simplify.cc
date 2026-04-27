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

void AstNode::set_in_lvalue_flag(bool flag, bool no_descend)
{
	if (flag != in_lvalue_from_above) {
		in_lvalue_from_above = flag;
		if (!no_descend)
			fixup_hierarchy_flags();
	}
}

void AstNode::set_in_param_flag(bool flag, bool no_descend)
{
	if (flag != in_param_from_above) {
		in_param_from_above = flag;
		if (!no_descend)
			fixup_hierarchy_flags();
	}
}

void AstNode::fixup_hierarchy_flags(bool force_descend)
{
	// With forced descend, we disable the implicit
	// descend from within the set_* functions, instead
	// we do an explicit descend at the end of this function

	in_param = in_param_from_above;

	switch (type) {
	case AST_PARAMETER:
	case AST_LOCALPARAM:
	case AST_DEFPARAM:
	case AST_PARASET:
	case AST_PREFIX:
		in_param = true;
		for (auto& child : children)
			child->set_in_param_flag(true, force_descend);
		break;

	case AST_REPLICATE:
	case AST_WIRE:
	case AST_GENIF:
	case AST_GENCASE:
		for (auto& child : children)
			child->set_in_param_flag(in_param, force_descend);
		if (children.size() >= 1)
			children[0]->set_in_param_flag(true, force_descend);
		break;

	case AST_GENFOR:
	case AST_FOR:
		for (auto& child : children) {
			log_assert((bool)child);
			child->set_in_param_flag(in_param, force_descend);
		}
		if (children.size() >= 2)
			children[1]->set_in_param_flag(true, force_descend);
		break;

	default:
		in_param = in_param_from_above;
		for (auto& child : children)
			child->set_in_param_flag(in_param, force_descend);
	}

	for (auto& attr : attributes)
		attr.second->set_in_param_flag(true, force_descend);

	in_lvalue = in_lvalue_from_above;

	switch (type) {
	case AST_ASSIGN:
	case AST_ASSIGN_EQ:
	case AST_ASSIGN_LE:
		if (children.size() >= 1)
			children[0]->set_in_lvalue_flag(true, force_descend);
		if (children.size() >= 2)
			children[1]->set_in_lvalue_flag(in_lvalue, force_descend);
		break;

	default:
		for (auto& child : children)
			child->set_in_lvalue_flag(in_lvalue, force_descend);
	}

	if (force_descend) {
		for (auto& child : children)
			child->fixup_hierarchy_flags(true);
		for (auto& attr : attributes)
			attr.second->fixup_hierarchy_flags(true);
	}
}

// Process a format string and arguments for $display, $write, $sprintf, etc

Fmt AstNode::processFormat(int stage, bool sformat_like, int default_base, size_t first_arg_at, bool may_fail) {
	std::vector<VerilogFmtArg> args;
	for (size_t index = first_arg_at; index < children.size(); index++) {
		AstNode *node_arg = children[index].get();
		while (node_arg->simplify(true, stage, -1, false)) { }

		VerilogFmtArg arg = {};
		arg.filename = *location.begin.filename;
		arg.first_line = location.begin.line;
		if (node_arg->type == AST_CONSTANT && node_arg->is_string) {
			arg.type = VerilogFmtArg::STRING;
			arg.str = node_arg->bitsAsConst().decode_string();
			// and in case this will be used as an argument...
			arg.sig = node_arg->bitsAsConst();
			arg.signed_ = false;
		} else if (node_arg->type == AST_IDENTIFIER && node_arg->str == "$time") {
			arg.type = VerilogFmtArg::TIME;
		} else if (node_arg->type == AST_IDENTIFIER && node_arg->str == "$realtime") {
			arg.type = VerilogFmtArg::TIME;
			arg.realtime = true;
		} else if (node_arg->type == AST_CONSTANT) {
			arg.type = VerilogFmtArg::INTEGER;
			arg.sig = node_arg->bitsAsConst();
			arg.signed_ = node_arg->is_signed;
		} else if (may_fail) {
			log_file_info(*location.begin.filename, location.begin.line, "Skipping system task `%s' with non-constant argument at position %zu.\n", str, index + 1);
			return Fmt();
		} else {
			log_file_error(*location.begin.filename, location.begin.line, "Failed to evaluate system task `%s' with non-constant argument at position %zu.\n", str, index + 1);
		}
		args.push_back(arg);
	}

	Fmt fmt;
	fmt.parse_verilog(args, sformat_like, default_base, /*task_name=*/str, current_module->name);
	return fmt;
}

void AstNode::annotateTypedEnums(AstNode *template_node)
{
	//check if enum
	if (template_node->attributes.count(ID::enum_type)) {
		//get reference to enum node:
		std::string enum_type = template_node->attributes[ID::enum_type]->str.c_str();
		//			log("enum_type=%s (count=%lu)\n", enum_type, current_scope.count(enum_type));
		//			log("current scope:\n");
		//			for (auto &it : current_scope)
		//				log("  %s\n", it.first);
		log_assert(current_scope.count(enum_type) == 1);
		AstNode *enum_node = current_scope.at(enum_type);
		log_assert(enum_node->type == AST_ENUM);
		while (enum_node->simplify()) { }
		//get width from 1st enum item:
		log_assert(enum_node->children.size() >= 1);
		AstNode *enum_item0 = enum_node->children[0].get();
		log_assert(enum_item0->type == AST_ENUM_ITEM);
		int width;
		if (!enum_item0->range_valid)
			width = 1;
		else if (enum_item0->range_swapped)
			width = enum_item0->range_right - enum_item0->range_left + 1;
		else
			width = enum_item0->range_left - enum_item0->range_right + 1;
		log_assert(width > 0);
		//add declared enum items:
		for (auto& enum_item : enum_node->children){
			log_assert(enum_item->type == AST_ENUM_ITEM);
			//get is_signed
			bool is_signed;
			if (enum_item->children.size() == 1){
				is_signed = false;
			} else if (enum_item->children.size() == 2){
				log_assert(enum_item->children[1]->type == AST_RANGE);
				is_signed = enum_item->children[1]->is_signed;
			} else {
				log_error("enum_item children size==%zu, expected 1 or 2 for %s (%s)\n",
						  (size_t) enum_item->children.size(),
						  enum_item->str.c_str(), enum_node->str.c_str()
				);
			}
			//start building attribute string
			std::string enum_item_str = "\\enum_value_";
			//get enum item value
			if(enum_item->children[0]->type != AST_CONSTANT){
				log_error("expected const, got %s for %s (%s)\n",
						  type2str(enum_item->children[0]->type).c_str(),
						  enum_item->str.c_str(), enum_node->str.c_str()
						);
			}
			RTLIL::Const val = enum_item->children[0]->bitsAsConst(width, is_signed);
			enum_item_str.append(val.as_string());
			//set attribute for available val to enum item name mappings
			set_attribute(enum_item_str.c_str(), mkconst_str(location, enum_item->str));
		}
	}
}

static std::unique_ptr<AstNode> make_range(AstSrcLocType loc, int left, int right, bool is_signed = false)
{
	// generate a pre-validated range node for a fixed signal range.
	auto range = std::make_unique<AstNode>(loc, AST_RANGE);
	range->range_left = left;
	range->range_right = right;
	range->range_valid = true;
	range->children.push_back(AstNode::mkconst_int(loc, left, true));
	range->children.push_back(AstNode::mkconst_int(loc, right, true));
	range->is_signed = is_signed;
	return range;
}

int AST_INTERNAL::range_width(AstNode *node, AstNode *rnode)
{
	log_assert(rnode->type==AST_RANGE);
	if (!rnode->range_valid) {
		node->input_error("Non-constant range in declaration of %s\n", node->str);
	}
	// note: range swapping has already been checked for
	return rnode->range_left - rnode->range_right + 1;
}

int AST_INTERNAL::add_dimension(AstNode *node, AstNode *rnode)
{
	int width = range_width(node, rnode);
	node->dimensions.push_back({ rnode->range_right, width, rnode->range_swapped });
	return width;
}

[[noreturn]] static void struct_array_packing_error(AstNode *node)
{
	node->input_error("Unpacked array in packed struct/union member %s\n", node->str);
}

// Check if node is an unexpanded array reference (AST_IDENTIFIER -> AST_MEMORY without indexing)
bool AST_INTERNAL::is_unexpanded_array_ref(AstNode *node)
{
	if (node->type != AST_IDENTIFIER)
		return false;
	if (node->id2ast == nullptr || node->id2ast->type != AST_MEMORY)
		return false;
	// No indexing children = whole array reference
	return node->children.empty();
}

// Check if two memories have compatible unpacked dimensions for array assignment
bool AST_INTERNAL::arrays_have_compatible_dims(AstNode *mem_a, AstNode *mem_b)
{
	if (mem_a->unpacked_dimensions != mem_b->unpacked_dimensions)
		return false;
	for (int i = 0; i < mem_a->unpacked_dimensions; i++) {
		if (mem_a->dimensions[i].range_width != mem_b->dimensions[i].range_width)
			return false;
	}
	// Also check packed dimensions (element width)
	int a_width, a_size, a_bits;
	int b_width, b_size, b_bits;
	mem_a->meminfo(a_width, a_size, a_bits);
	mem_b->meminfo(b_width, b_size, b_bits);
	return a_width == b_width;
}

// Convert per-dimension element positions to declared index values.
// Position 0 is the first declared element for each unpacked dimension.
std::vector<int> AST_INTERNAL::array_indices_from_position(AstNode *mem, const std::vector<int> &position)
{
	int num_dims = mem->unpacked_dimensions;
	log_assert(GetSize(position) == num_dims);

	std::vector<int> indices(num_dims);
	for (int d = 0; d < num_dims; d++) {
		int low = mem->dimensions[d].range_right;
		int high = low + mem->dimensions[d].range_width - 1;
		indices[d] = mem->dimensions[d].range_swapped ? (low + position[d]) : (high - position[d]);
	}
	return indices;
}

// Generate all element positions for a multi-dimensional unpacked array and
// call callback once for each combination.
void AST_INTERNAL::foreach_array_position(AstNode *mem, std::function<void(const std::vector<int>&)> callback)
{
	int num_dims = mem->unpacked_dimensions;
	if (num_dims == 0) {
		callback({});
		return;
	}

	std::vector<int> position(num_dims, 0);
	std::vector<int> sizes(num_dims);

	for (int d = 0; d < num_dims; d++)
		sizes[d] = mem->dimensions[d].range_width;

	// Iterate through all position combinations (rightmost dimension fastest).
	while (true) {
		callback(position);

		int d = num_dims - 1;
		while (d >= 0) {
			position[d]++;
			if (position[d] < sizes[d])
				break;
			position[d] = 0;
			d--;
		}
		if (d < 0)
			break;
	}
}

int AST_INTERNAL::size_packed_struct(AstNode *snode, int base_offset)
{
	// Struct members will be laid out in the structure contiguously from left to right.
	// Union members all have zero offset from the start of the union.
	// Determine total packed size and assign offsets.  Store these in the member node.
	bool is_union = (snode->type == AST_UNION);
	int offset = 0;
	int packed_width = -1;
	// examine members from last to first
	for (auto it = snode->children.rbegin(); it != snode->children.rend(); ++it) {
		auto node = it->get();
		int width;
		if (node->type == AST_STRUCT || node->type == AST_UNION) {
			// embedded struct or union
			width = size_packed_struct(node, base_offset + offset);
		}
		else {
			log_assert(node->type == AST_STRUCT_ITEM);
			if (node->children.size() > 0 && node->children[0]->type == AST_RANGE) {
				// member width e.g. bit [7:0] a
				width = range_width(node, node->children[0].get());
				if (node->children.size() == 2) {
					// Unpacked array. Note that this is a Yosys extension; only packed data types
					// and integer data types are allowed in packed structs / unions in SystemVerilog.
					if (node->children[1]->type == AST_RANGE) {
						// Unpacked array, e.g. bit [63:0] a [0:3]
						// Pretend it's declared as a packed array, e.g. bit [0:3][63:0] a
						auto rnode = node->children[1].get();
						if (rnode->children.size() == 1) {
							// C-style array size, e.g. bit [63:0] a [4]
							node->dimensions.push_back({ 0, rnode->range_left, true });
							width *= rnode->range_left;
						} else {
							width *= add_dimension(node, rnode);
						}
						add_dimension(node, node->children[0].get());
					}
					else {
						// The Yosys extension for unpacked arrays in packed structs / unions
						// only supports memories, i.e. e.g. logic [7:0] a [256] - see above.
						struct_array_packing_error(node);
					}
				} else {
					// Vector
					add_dimension(node, node->children[0].get());
				}
				// range nodes are now redundant
				node->children.clear();
			}
			else if (node->children.size() > 0 && node->children[0]->type == AST_MULTIRANGE) {
				// Packed array, e.g. bit [3:0][63:0] a
				if (node->children.size() != 1) {
					// The Yosys extension for unpacked arrays in packed structs / unions
					// only supports memories, i.e. e.g. logic [7:0] a [256] - see above.
					struct_array_packing_error(node);
				}
				width = 1;
				for (auto& rnode : node->children[0]->children) {
					width *= add_dimension(node, rnode.get());
				}
				// range nodes are now redundant
				node->children.clear();
			}
			else if (node->range_left < 0) {
				// 1 bit signal: bit, logic or reg
				width = 1;
				node->dimensions.push_back({ 0, width, false });
			}
			else {
				// already resolved and compacted
				width = node->range_left - node->range_right + 1;
			}
			if (is_union) {
				node->range_right = base_offset;
				node->range_left = base_offset + width - 1;
			}
			else {
				node->range_right = base_offset + offset;
				node->range_left = base_offset + offset + width - 1;
			}
			node->range_valid = true;
		}
		if (is_union) {
			// check that all members have the same size
			if (packed_width == -1) {
				// first member
				packed_width = width;
			}
			else {
				if (packed_width != width)
					node->input_error("member %s of a packed union has %d bits, expecting %d\n", node->str, width, packed_width);
			}
		}
		else {
			offset += width;
		}
	}

	int width = is_union ? packed_width : offset;

	snode->range_right = base_offset;
	snode->range_left = base_offset + width - 1;
	snode->range_valid = true;
	snode->dimensions.push_back({ 0, width, false });

	return width;
}

static std::unique_ptr<AstNode> node_int(AstSrcLocType loc, int ival)
{
	return AstNode::mkconst_int(loc, ival, true);
}

static std::unique_ptr<AstNode> multiply_by_const(std::unique_ptr<AstNode> expr_node, int stride)
{
	auto loc = expr_node->location;
	return std::make_unique<AstNode>(loc, AST_MUL, std::move(expr_node), node_int(loc, stride));
}

static std::unique_ptr<AstNode> normalize_index(AstNode *expr, AstNode *decl_node, int dimension)
{
	auto new_expr = expr->clone();
	auto loc = expr->location;

	int offset = decl_node->dimensions[dimension].range_right;
	if (offset) {
		new_expr = std::make_unique<AstNode>(loc, AST_SUB, std::move(new_expr), node_int(loc, offset));
	}

	// Packed dimensions are normally indexed by lsb, while unpacked dimensions are normally indexed by msb.
	if ((dimension < decl_node->unpacked_dimensions) ^ decl_node->dimensions[dimension].range_swapped) {
		// Swap the index if the dimension is declared the "wrong" way.
		int left = decl_node->dimensions[dimension].range_width - 1;
		new_expr = std::make_unique<AstNode>(loc, AST_SUB, node_int(loc, left), std::move(new_expr));
	}

	return new_expr;
}

static std::unique_ptr<AstNode> index_offset(std::unique_ptr<AstNode> offset, AstNode *rnode, AstNode *decl_node, int dimension, int &stride)
{
	stride /= decl_node->dimensions[dimension].range_width;
	auto right = normalize_index(rnode->children.back().get(), decl_node, dimension);
	auto add_offset = stride > 1 ? multiply_by_const(std::move(right), stride) : std::move(right);
	return offset ? std::make_unique<AstNode>(rnode->location, AST_ADD, std::move(offset), std::move(add_offset)) : std::move(add_offset);
}

static std::unique_ptr<AstNode> index_msb_offset(std::unique_ptr<AstNode> lsb_offset, AstNode *rnode, AstNode *decl_node, int dimension, int stride)
{
	log_assert(rnode->children.size() <= 2);
	auto loc = rnode->location;

	// Offset to add to LSB
	std::unique_ptr<AstNode> add_offset;
	if (rnode->children.size() == 1) {
		// Index, e.g. s.a[i]
		add_offset = node_int(rnode->location, stride - 1);
	}
	else {
		// rnode->children.size() == 2
		// Slice, e.g. s.a[i:j]
		auto left = normalize_index(rnode->children[0].get(), decl_node, dimension);
		auto right = normalize_index(rnode->children[1].get(), decl_node, dimension);
		add_offset = std::make_unique<AstNode>(loc, AST_SUB, std::move(left), std::move(right));
		if (stride > 1) {
			// offset = (msb - lsb + 1)*stride - 1
			auto slice_width = std::make_unique<AstNode>(loc, AST_ADD, std::move(add_offset), node_int(loc, 1));
			add_offset = std::make_unique<AstNode>(loc, AST_SUB, multiply_by_const(std::move(slice_width), stride), node_int(loc, 1));
		}
	}

	return std::make_unique<AstNode>(loc, AST_ADD, std::move(lsb_offset), std::move(add_offset));
}


std::unique_ptr<AstNode> AstNode::make_index_range(AstNode *decl_node, bool unpacked_range)
{
	// Work out the range in the packed array that corresponds to a struct member
	// taking into account any range operations applicable to the current node
	// such as array indexing or slicing
	if (children.empty()) {
		// no range operations apply, return the whole width
		return make_range(decl_node->location, decl_node->range_left - decl_node->range_right, 0);
	}

	log_assert(children.size() == 1);

	// Range operations
	AstNode *rnode = children[0].get();
	std::unique_ptr<AstNode> offset = nullptr;
	int dim = unpacked_range ? 0 : decl_node->unpacked_dimensions;
	int max_dim = unpacked_range ? decl_node->unpacked_dimensions : GetSize(decl_node->dimensions);

	int stride = 1;
	for (int i = dim; i < max_dim; i++) {
		stride *= decl_node->dimensions[i].range_width;
	}

	// Calculate LSB offset for the final index / slice
	if (rnode->type == AST_RANGE) {
		offset = index_offset(std::move(offset), rnode, decl_node, dim, stride);
	}
	else if (rnode->type == AST_MULTIRANGE) {
		// Add offset for each dimension
		AstNode *mrnode = rnode;
		int stop_dim = std::min(GetSize(mrnode->children), max_dim);
		for (; dim < stop_dim; dim++) {
			rnode = mrnode->children[dim].get();
			offset = index_offset(std::move(offset), rnode, decl_node, dim, stride);
		}
		dim--;  // Step back to the final index / slice
	}
	else {
		input_error("Unsupported range operation for %s\n", str);
	}

	std::unique_ptr<AstNode> index_range = std::make_unique<AstNode>(rnode->location, AST_RANGE);

	if (!unpacked_range && (stride > 1 || GetSize(rnode->children) == 2)) {
		// Calculate MSB offset for the final index / slice of packed dimensions.
		std::unique_ptr<AstNode>msb_offset = index_msb_offset(offset->clone(), rnode, decl_node, dim, stride);
		index_range->children.push_back(std::move(msb_offset));
	}

	index_range->children.push_back(std::move(offset));

	return index_range;
}

AstNode *AstNode::get_struct_member() const
{
	AstNode *member_node;
	if (attributes.count(ID::wiretype) && (member_node = attributes.at(ID::wiretype).get()) &&
		(member_node->type == AST_STRUCT_ITEM || member_node->type == AST_STRUCT || member_node->type == AST_UNION))
	{
		return member_node;
	}
	return nullptr;
}

void AST_INTERNAL::add_members_to_scope(AstNode *snode, std::string name)
{
	// add all the members in a struct or union to local scope
	// in case later referenced in assignments
	log_assert(snode->type==AST_STRUCT || snode->type==AST_UNION);
	for (auto &node : snode->children) {
		auto member_name = name + "." + node->str;
		current_scope[member_name] = node.get();
		if (node->type != AST_STRUCT_ITEM) {
			// embedded struct or union
			add_members_to_scope(node.get(), name + "." + node->str);
		}
	}
}

std::unique_ptr<AstNode> AST_INTERNAL::make_packed_struct(AstNode *template_node, std::string &name, decltype(AstNode::attributes) &attributes)
{
	// create a wire for the packed struct
	auto loc = template_node->location;
	auto wnode = std::make_unique<AstNode>(loc, AST_WIRE, make_range(loc, template_node->range_left, 0));
	wnode->str = name;
	wnode->is_logic = true;
	wnode->range_valid = true;
	wnode->is_signed = template_node->is_signed;
	for (auto &pair : attributes) {
		wnode->set_attribute(pair.first, pair.second->clone());
	}
	// resolve packed dimension
	while (wnode->simplify()) {}
	// make sure this node is the one in scope for this name
	current_scope[name] = wnode.get();
	// add all the struct members to scope under the wire's name
	add_members_to_scope(template_node, name);
	return wnode;
}

void AST_INTERNAL::prepend_ranges(std::unique_ptr<AstNode> &range, AstNode *range_add)
{
	// Convert range to multirange.
	auto loc = range->location;
	if (range->type == AST_RANGE)
		range = std::make_unique<AstNode>(loc, AST_MULTIRANGE, std::move(range));

	// Add range or ranges.
	if (range_add->type == AST_RANGE)
		range->children.insert(range->children.begin(), range_add->clone());
	else {
		int i = 0;
		for (auto& child : range_add->children)
			range->children.insert(range->children.begin() + i++, child->clone());
	}
}

// check if a node or its children contains an assignment to the given variable
bool AST_INTERNAL::node_contains_assignment_to(const AstNode* node, const AstNode* var)
{
	if (node->type == AST_ASSIGN_EQ || node->type == AST_ASSIGN_LE) {
		// current node is iteslf an assignment
		log_assert(node->children.size() >= 2);
		const AstNode* lhs = node->children[0].get();
		if (lhs->type == AST_IDENTIFIER && lhs->str == var->str)
			return false;
	}
	for (auto& child : node->children) {
		// if this child shadows the given variable
		if (child.get() != var && child->str == var->str && child->type == AST_WIRE)
			break; // skip the remainder of this block/scope
		// depth-first short circuit
		if (!node_contains_assignment_to(child.get(), var))
			return false;
	}
	return true;
}

std::string AST_INTERNAL::prefix_id(const std::string &prefix, const std::string &str)
{
	log_assert(!prefix.empty() && (prefix.front() == '$' || prefix.front() == '\\'));
	log_assert(!str.empty() && (str.front() == '$' || str.front() == '\\'));
	log_assert(prefix.back() == '.');
	if (str.front() == '\\')
		return prefix + str.substr(1);
	return prefix + str;
}

// direct access to this global should be limited to the following two functions
// lol why is it a fucking global then
const RTLIL::Design *AST_INTERNAL::simplify_design_context = nullptr;

void AST::set_simplify_design_context(const RTLIL::Design *design)
{
	log_assert(!simplify_design_context || !design);
	simplify_design_context = design;
}

// lookup the module with the given name in the current design context
static const RTLIL::Module* lookup_module(const std::string &name)
{
	return simplify_design_context->module(name);
}

const RTLIL::Module* AstNode::lookup_cell_module()
{
	log_assert(type == AST_CELL);

	auto reprocess_after = [this] (const std::string &modname) {
		if (!attributes.count(ID::reprocess_after))
			set_attribute(ID::reprocess_after, AstNode::mkconst_str(location, modname));
	};

	AstCell self(this);
	const AstNode *celltype = self.celltype();

	const RTLIL::Module *module = lookup_module(celltype->str);
	if (!module)
		module = lookup_module("$abstract" + celltype->str);
	if (!module) {
		if (celltype->str.at(0) != '$')
			reprocess_after(celltype->str);
		return nullptr;
	}

	// build a mapping from true param name to param value
	size_t para_counter = 0;
	dict<RTLIL::IdString, RTLIL::Const> cell_params_map;
	for (auto it = self.body_begin(); it != self.body_end(); ++it) {
		auto pset = AstParaset::cast(it->get());
		if (!pset)
			continue;

		if (pset->is_positional() && para_counter >= module->avail_parameters.size())
			return nullptr; // let hierarchy handle this error
		IdString paraname = pset->is_positional() ? module->avail_parameters[para_counter++] : pset->str();

		const AstNode *value = pset->expr();
		if (value->type != AST_REALVALUE && value->type != AST_CONSTANT)
			return nullptr; // let genrtlil handle this error
		cell_params_map[paraname] = value->asParaConst();
	}

	// put the parameters in order and generate the derived module name
	std::vector<std::pair<RTLIL::IdString, RTLIL::Const>> named_parameters;
	for (RTLIL::IdString param : module->avail_parameters) {
		auto it = cell_params_map.find(param);
		if (it != cell_params_map.end())
			named_parameters.emplace_back(it->first, it->second);
	}
	std::string modname = celltype->str;
	if (cell_params_map.size()) // not named_parameters to cover hierarchical defparams
		modname = derived_module_name(celltype->str, named_parameters);

	// try to find the resolved module
	module = lookup_module(modname);
	if (!module) {
		reprocess_after(modname);
		return nullptr;
	}
	return module;
}

// returns whether an expression contains an unbased unsized literal; does not
// check the literal exists in a self-determined context
bool AST_INTERNAL::contains_unbased_unsized(const AstNode *node)
{
	if (node->type == AST_CONSTANT)
		return node->is_unsized;
	for (auto& child : node->children)
		if (contains_unbased_unsized(child.get()))
			return true;
	return false;
}

// adds a wire to the current module with the given name that matches the
// dimensions of the given wire reference
void AST_INTERNAL::add_wire_for_ref(Location loc, const RTLIL::Wire *ref, const std::string &str)
{
	auto left = AstNode::mkconst_int(loc, ref->width - 1 + ref->start_offset, true);
	auto right = AstNode::mkconst_int(loc, ref->start_offset, true);
	if (ref->upto)
		std::swap(left, right);
	auto range = std::make_unique<AstNode>(loc, AST_RANGE, std::move(left), std::move(right));

	auto wire = std::make_unique<AstNode>(loc, AST_WIRE, std::move(range));
	wire->is_signed = ref->is_signed;
	wire->is_logic = true;
	wire->str = str;

	current_scope[str] = wire.get();
	current_ast_mod->children.push_back(std::move(wire));
}

enum class IdentUsage {
	NotReferenced, // target variable is neither read or written in the block
	Assigned, // target variable is always assigned before use
	SyncRequired, // target variable may be used before it has been assigned
};

// determines whether a local variable a block is always assigned before it is
// used, meaning the nosync attribute can automatically be added to that
// variable
static IdentUsage always_asgn_before_use(const AstNode *node, const std::string &target)
{
	// This variable has been referenced before it has necessarily been assigned
	// a value in this procedure.
	if (node->type == AST_IDENTIFIER && node->str == target)
		return IdentUsage::SyncRequired;

	// For case statements (which are also used for if/else), we check each
	// possible branch. If the variable is assigned in all branches, then it is
	// assigned, and a sync isn't required. If it used before assignment in any
	// branch, then a sync is required.
	if (node->type == AST_CASE) {
		bool all_defined = true;
		bool any_used = false;
		bool has_default = false;
		for (auto& child : node->children) {
			if (child->type == AST_COND && child->children.at(0)->type == AST_DEFAULT)
				has_default = true;
			IdentUsage nested = always_asgn_before_use(child.get(), target);
			if (nested != IdentUsage::Assigned && child->type == AST_COND)
				all_defined = false;
			if (nested == IdentUsage::SyncRequired)
				any_used = true;
		}
		if (any_used)
			return IdentUsage::SyncRequired;
		else if (all_defined && has_default)
			return IdentUsage::Assigned;
		else
			return IdentUsage::NotReferenced;
	}

	// Check if this is an assignment to the target variable. For simplicity, we
	// don't analyze sub-ranges of the variable.
	if (node->type == AST_ASSIGN_EQ) {
		auto& ident = node->children.at(0);
		if (ident->type == AST_IDENTIFIER && ident->str == target)
			return IdentUsage::Assigned;
	}

	for (auto& child : node->children) {
		IdentUsage nested = always_asgn_before_use(child.get(), target);
		if (nested != IdentUsage::NotReferenced)
			return nested;
	}
	return IdentUsage::NotReferenced;
}

std::unique_ptr<AstNode> AstNode::clone_at_zero()
{
	int width_hint;
	bool sign_hint;
	AstNode *pointee;

	switch (type) {
	case AST_IDENTIFIER:
		if (id2ast)
			pointee = id2ast;
		else if (current_scope.count(str))
			pointee = current_scope[str];
		else
			break;

		if (pointee->type != AST_WIRE &&
				pointee->type != AST_AUTOWIRE &&
				pointee->type != AST_MEMORY)
			break;

		YS_FALLTHROUGH
	case AST_MEMRD:
		detectSignWidth(width_hint, sign_hint);
		return mkconst_int(location, 0, sign_hint, width_hint);

	default:
		break;
	}

	auto that = clone();
	for (auto &it : that->children)
		it = it->clone_at_zero();
	for (auto &it : that->attributes)
		it.second = it.second->clone();

	that->set_in_lvalue_flag(false);
	that->set_in_param_flag(false);
	that->fixup_hierarchy_flags();

	return that;
}

bool AST_INTERNAL::try_determine_range_width(AstNode *range, int &result_width)
{
	log_assert(range->type == AST_RANGE);

	if (range->children.size() == 1) {
		result_width = 1;
		return true;
	}

	AstRange r(range);
	auto left_at_zero_ast = r.msb()->clone_at_zero();
	auto right_at_zero_ast = r.lsb()->clone_at_zero();

	while (left_at_zero_ast->simplify()) {}
	while (right_at_zero_ast->simplify()) {}

	bool ok = false;
	if (left_at_zero_ast->type == AST_CONSTANT
			&& right_at_zero_ast->type == AST_CONSTANT) {
		ok = true;
		result_width = abs(int(left_at_zero_ast->integer - right_at_zero_ast->integer)) + 1;
	}

	return ok;
}

const std::string AST_INTERNAL::auto_nosync_prefix = "\\AutoNosync";

// mark a local variable in an always_comb block for automatic nosync
// consideration
void AST_INTERNAL::mark_auto_nosync(AstNode *block, const AstNode *wire)
{
	log_assert(block->type == AST_BLOCK);
	log_assert(wire->type == AST_WIRE);
	block->set_attribute(auto_nosync_prefix + wire->str, AstNode::mkconst_int(block->location, 1, false));
}

// block names can be prefixed with an explicit scope during elaboration
bool AST_INTERNAL::is_autonamed_block(const std::string &str) {
	size_t last_dot = str.rfind('.');
	// unprefixed names: autonamed if the first char is a dollar sign
	if (last_dot == std::string::npos)
		return str.at(0) == '$'; // e.g., `$fordecl_block$1`
	// prefixed names: autonamed if the final chunk begins with a dollar sign
	return str.rfind(".$") == last_dot; // e.g., `\foo.bar.$fordecl_block$1`
}

// check a procedural block for auto-nosync markings, remove them, and add
// nosync to local variables as necessary
void AST_INTERNAL::check_auto_nosync(AstNode *node)
{
	std::vector<RTLIL::IdString> attrs_to_drop;
	for (const auto& elem : node->attributes) {
		// skip attributes that don't begin with the prefix
		if (elem.first.compare(0, auto_nosync_prefix.size(),
					auto_nosync_prefix.c_str()))
			continue;

		// delete and remove the attribute once we're done iterating
		attrs_to_drop.push_back(elem.first);

		// find the wire based on the attribute
		std::string wire_name = elem.first.substr(auto_nosync_prefix.size());
		auto it = current_scope.find(wire_name);
		if (it == current_scope.end())
			continue;

		// analyze the usage of the local variable in this block
		IdentUsage ident_usage = always_asgn_before_use(node, wire_name);
		if (ident_usage != IdentUsage::Assigned)
			continue;

		// mark the wire with `nosync`
		AstNode *wire = it->second;
		log_assert(wire->type == AST_WIRE);
		wire->set_attribute(ID::nosync, AstNode::mkconst_int(wire->location, 1, false));
	}

	// remove the attributes we've "consumed"
	for (RTLIL::IdString str : attrs_to_drop) {
		auto it = node->attributes.find(str);
		node->attributes.erase(it);
	}

	// check local variables in any nested blocks
	for (auto& child : node->children)
		check_auto_nosync(child.get());
}

void AstNode::replace_result_wire_name_in_function(const std::string &from, const std::string &to)
{
	for (auto& child : children)
		child->replace_result_wire_name_in_function(from, to);
	if (str == from && type != AST_FCALL && type != AST_TCALL)
		str = to;
}

// annotate the names of all wires and other named objects in a named generate
// or procedural block; nested blocks are themselves annotated such that the
// prefix is carried forward, but resolution of their children is deferred
void AstNode::expand_genblock(const std::string &prefix)
{
	if (type == AST_IDENTIFIER || type == AST_FCALL || type == AST_TCALL || type == AST_WIRETYPE || type == AST_PREFIX) {
		log_assert(!str.empty());

		// search starting in the innermost scope and then stepping outward
		for (size_t ppos = prefix.size() - 1; ppos; --ppos) {
			if (prefix.at(ppos) != '.') continue;

			std::string new_prefix = prefix.substr(0, ppos + 1);
			auto attempt_resolve = [&new_prefix](const std::string &ident) -> std::string {
				std::string new_name = prefix_id(new_prefix, ident);
				if (current_scope.count(new_name))
					return new_name;
				return {};
			};

			// attempt to resolve the full identifier
			std::string resolved = attempt_resolve(str);
			if (!resolved.empty()) {
				str = resolved;
				break;
			}

			// attempt to resolve hierarchical prefixes within the identifier,
			// as the prefix could refer to a local scope which exists but
			// hasn't yet been elaborated
			for (size_t spos = str.size() - 1; spos; --spos) {
				if (str.at(spos) != '.') continue;
				resolved = attempt_resolve(str.substr(0, spos));
				if (!resolved.empty()) {
					str = resolved + str.substr(spos);
					ppos = 1; // break outer loop
					break;
				}
			}

		}
	}

	auto prefix_node = [&prefix](AstNode* child) {
		if (child->str.empty()) return;
		std::string new_name = prefix_id(prefix, child->str);
		if (child->type == AST_FUNCTION)
			child->replace_result_wire_name_in_function(child->str, new_name);
		else
			child->str = new_name;
		current_scope[new_name] = child;
	};

	for (size_t i = 0; i < children.size(); i++) {
		auto* child = children[i].get();

		switch (child->type) {
		case AST_WIRE:
		case AST_AUTOWIRE:
		case AST_MEMORY:
		case AST_STRUCT:
		case AST_UNION:
		case AST_PARAMETER:
		case AST_LOCALPARAM:
		case AST_FUNCTION:
		case AST_TASK:
		case AST_CELL:
		case AST_TYPEDEF:
		case AST_ENUM_ITEM:
		case AST_GENVAR:
			prefix_node(child);
			break;

		case AST_BLOCK:
		case AST_GENBLOCK:
			if (!child->str.empty())
				prefix_node(child);
			break;

		case AST_ENUM:
			current_scope[child->str] = child;
			for (auto& enode : child->children){
				log_assert(enode->type == AST_ENUM_ITEM);
				prefix_node(enode.get());
			}
			break;

		case AST_IDENTIFIER:
			if (!child->str.empty() && prefix.size() > 0) {
				bool is_resolved = false;
				std::string identifier_str = child->str;
				if (current_ast_mod != nullptr && identifier_str.compare(0, current_ast_mod->str.size(), current_ast_mod->str) == 0) {
					if (identifier_str.at(current_ast_mod->str.size()) == '.') {
						identifier_str = '\\' + identifier_str.substr(current_ast_mod->str.size()+1, identifier_str.size());
					}
				}
				// search starting in the innermost scope and then stepping outward
				for (size_t ppos = prefix.size() - 1; ppos; --ppos) {
					if (prefix.at(ppos) != '.') continue;

					std::string new_prefix = prefix.substr(0, ppos + 1);
					auto attempt_resolve = [&new_prefix](const std::string &ident) -> std::string {
						std::string new_name = prefix_id(new_prefix, ident);
						if (current_scope.count(new_name))
							return new_name;
						return {};
					};

					// attempt to resolve the full identifier
					std::string resolved = attempt_resolve(identifier_str);
					if (!resolved.empty()) {
						is_resolved = true;
						break;
					}
					// attempt to resolve hierarchical prefixes within the identifier,
					// as the prefix could refer to a local scope which exists but
					// hasn't yet been elaborated
					for (size_t spos = identifier_str.size() - 1; spos; --spos) {
						if (identifier_str.at(spos) != '.') continue;
						resolved = attempt_resolve(identifier_str.substr(0, spos));
						if (!resolved.empty()) {
							is_resolved = true;
							identifier_str = resolved + identifier_str.substr(spos);
							ppos = 1; // break outer loop
							break;
						}
					}
					if (current_scope.count(identifier_str) == 0) {
						AstNode *current_scope_ast = (current_ast_mod == nullptr) ? current_ast : current_ast_mod;
						for (auto& node : current_scope_ast->children) {
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
								if (prefix_id(new_prefix, identifier_str) == node->str) {
									is_resolved = true;
									current_scope[node->str] = node.get();
								}
								break;
							case AST_ENUM:
								current_scope[node->str] = node.get();
								for (auto& enum_node : node->children) {
									log_assert(enum_node->type==AST_ENUM_ITEM);
									if (prefix_id(new_prefix, identifier_str) == enum_node->str) {
										is_resolved = true;
										current_scope[enum_node->str] = enum_node.get();
									}
								}
								break;
							default:
								break;
							}
						}
					}
				}
				if ((current_scope.count(identifier_str) == 0) && is_resolved == false) {
					if (current_ast_mod == nullptr) {
						input_error("Identifier `%s' is implicitly declared outside of a module.\n", child->str.c_str());
					} else if (flag_autowire || identifier_str == "\\$global_clock") {
						auto auto_wire = std::make_unique<AstNode>(child->location, AST_AUTOWIRE);
						auto_wire->str = identifier_str;
						children.push_back(std::move(auto_wire));
					} else {
						input_error("Identifier `%s' is implicitly declared and `default_nettype is set to none.\n", identifier_str.c_str());
					}
				}
			}
			break;
		default:
			break;
		}
	}

	for (size_t i = 0; i < children.size(); i++) {
		auto& child = children[i];
		// AST_PREFIX member names should not be prefixed; we recurse into them
		// as normal to ensure indices and ranges are properly resolved, and
		// then restore the previous string
		if (type == AST_PREFIX && i == 1) {
			std::string backup_scope_name = child->str;
			child->expand_genblock(prefix);
			child->str = backup_scope_name;
			continue;
		}
		// functions/tasks may reference wires, constants, etc. in this scope
		if (child->type == AST_FUNCTION || child->type == AST_TASK)
			continue;
		// named blocks pick up the current prefix and will expanded later
		if ((child->type == AST_GENBLOCK || child->type == AST_BLOCK) && !child->str.empty())
			continue;

		child->expand_genblock(prefix);
	}
}

// add implicit AST_GENBLOCK names according to IEEE 1364-2005 Section 12.4.3 or
// IEEE 1800-2017 Section 27.6
void AstNode::label_genblks(std::set<std::string>& existing, int &counter)
{
	switch (type) {
	case AST_GENIF:
	case AST_GENFOR:
	case AST_GENCASE:
		// seeing a proper generate control flow construct increments the
		// counter once
		++counter;
		for (auto& child : children)
			child->label_genblks(existing, counter);
		break;

	case AST_GENBLOCK: {
		// if this block is unlabeled, generate its corresponding unique name
		for (int padding = 0; str.empty(); ++padding) {
			std::string candidate = "\\genblk";
			for (int i = 0; i < padding; ++i)
				candidate += '0';
			candidate += std::to_string(counter);
			if (!existing.count(candidate))
				str = candidate;
		}
		// within a genblk, the counter starts fresh
		std::set<std::string> existing_local = existing;
		int counter_local = 0;
		for (auto& child : children)
			child->label_genblks(existing_local, counter_local);
		break;
	}

	default:
		// track names which could conflict with implicit genblk names
		if (str.rfind("\\genblk", 0) == 0)
			existing.insert(str);
		for (auto& child : children)
			child->label_genblks(existing, counter);
		break;
	}
}

bool AstNode::detect_latch(const std::string &var)
{
	switch (type)
	{
	case AST_ALWAYS:
		for (auto &c : children)
		{
			switch (c->type)
			{
			case AST_POSEDGE:
			case AST_NEGEDGE:
				return false;
			case AST_EDGE:
				break;
			case AST_BLOCK:
				if (!c->detect_latch(var))
					return false;
				break;
			default:
				log_abort();
			}
		}
		return true;
	case AST_BLOCK:
		for (auto &c : children)
			if (!c->detect_latch(var))
				return false;
		return true;
	case AST_CASE:
		{
			bool r = true;
			for (auto &c : children) {
				if (auto cond = AstCond::cast(c.get())) {
					if (cond->body()->detect_latch(var))
						return true;
					r = false;
				}
				if (c->type == AST_DEFAULT) {
					if (c->children.at(0)->detect_latch(var))
						return true;
					r = false;
				}
			}
			return r;
		}
	case AST_ASSIGN_EQ:
	case AST_ASSIGN_LE:
		if (children.at(0)->type == AST_IDENTIFIER &&
				children.at(0)->children.empty() && children.at(0)->str == var)
			return false;
		return true;
	default:
		return true;
	}
}

// helper function for AstNode::eval_const_function()
bool AstNode::replace_variables(std::map<std::string, AstNode::varinfo_t> &variables, AstNode *fcall, bool must_succeed)
{
	if (type == AST_IDENTIFIER && variables.count(str)) {
		int offset = variables.at(str).offset, width = variables.at(str).val.size();
		if (!children.empty()) {
			if (children.size() != 1 || children.at(0)->type != AST_RANGE) {
				if (!must_succeed)
					return false;
				input_error("Memory access in constant function is not supported\n%s: ...called from here.\n",
						fcall->loc_string().c_str());
			}
			if (!children.at(0)->replace_variables(variables, fcall, must_succeed))
				return false;
			while (simplify(true, 1, -1, false)) { }
			if (!children.at(0)->range_valid) {
				if (!must_succeed)
					return false;
				input_error("Non-constant range\n%s: ... called from here.\n",
						fcall->loc_string().c_str());
			}
			offset = min(children.at(0)->range_left, children.at(0)->range_right);
			width = min(std::abs(children.at(0)->range_left - children.at(0)->range_right) + 1, width);
		}
		offset -= variables.at(str).offset;
		if (variables.at(str).range_swapped)
			offset = -offset;
		const RTLIL::Const &val = variables.at(str).val;
		std::vector<RTLIL::State> new_bits;
		new_bits.reserve(width);
		for (int i = 0; i < width; i++)
			new_bits.push_back(val[offset+i]);
		auto newNode = mkconst_bits(location, new_bits, variables.at(str).is_signed);
		newNode->cloneInto(*this);
		return true;
	}

	for (auto &child : children)
		if (!child->replace_variables(variables, fcall, must_succeed))
			return false;
	return true;
}

void AstNode::allocateDefaultEnumValues()
{
	log_assert(type==AST_ENUM);
	log_assert(children.size() > 0);
	if (children.front()->attributes.count(ID::enum_base_type))
		return; // already elaborated
	int last_enum_int = -1;
	for (auto& child : children) {
		AstEnumItem item(child.get());
		AstNode *node = item.raw();
		node->set_attribute(ID::enum_base_type, mkconst_str(node->location, str));
		for (size_t i = 0; i < node->children.size(); i++) {
			switch (node->children[i]->type) {
			case AST_NONE:
				// replace with auto-incremented constant
				node->children[i] = AstNode::mkconst_int(node->location, ++last_enum_int, true);
				break;
			case AST_CONSTANT:
				// explicit constant (or folded expression)
				// TODO: can't extend 'x or 'z item
				last_enum_int = node->children[i]->integer;
				break;
			default:
				// ignore ranges
				break;
			}
			// TODO: range check
		}
	}
}

bool AstNode::is_recursive_function() const
{
	std::set<const AstNode *> visited;
	std::function<bool(const AstNode *node)> visit = [&](const AstNode *node) {
		if (visited.count(node))
			return node == this;
		visited.insert(node);
		if (node->type == AST_FCALL) {
			auto it = current_scope.find(node->str);
			if (it != current_scope.end() && visit(it->second))
				return true;
		}
		for (auto& child : node->children) {
			if (visit(child.get()))
				return true;
		}
		return false;
	};

	log_assert(type == AST_FUNCTION);
	return visit(this);
}

std::pair<AstNode*, AstNode*> AstNode::get_tern_choice()
{
	AstTernary tern(this);
	AstNode *cond_n = tern.cond().get();
	AstNode *then_n = tern.then_().get();
	AstNode *else_n = tern.else_().get();

	if (!cond_n->isConst())
		return {};

	bool found_sure_true = false;
	bool found_maybe_true = false;

	if (cond_n->type == AST_CONSTANT)
		for (auto &bit : cond_n->bits) {
			if (bit == RTLIL::State::S1)
				found_sure_true = true;
			if (bit > RTLIL::State::S1)
				found_maybe_true = true;
		}
	else
		found_sure_true = cond_n->asReal(true) != 0;

	AstNode *choice = nullptr, *not_choice = nullptr;
	if (found_sure_true)
		choice = then_n, not_choice = else_n;
	else if (!found_maybe_true)
		choice = else_n, not_choice = then_n;

	return {choice, not_choice};
}

std::string AstNode::try_pop_module_prefix() const
{
	AstNode *current_scope_ast = (current_ast_mod == nullptr) ? current_ast : current_ast_mod;
	size_t pos = str.find('.', 1);
	if (str[0] == '\\' && pos != std::string::npos) {
		std::string new_str = "\\" + str.substr(pos + 1);
		if (current_scope.count(new_str)) {
			std::string prefix = str.substr(0, pos);
			auto it = current_scope_ast->attributes.find(ID::hdlname);
			if ((it != current_scope_ast->attributes.end() && it->second->str == prefix.substr(1))
					|| prefix == current_scope_ast->str)
				return new_str;
		}
	}
	return str;
}

YOSYS_NAMESPACE_END
