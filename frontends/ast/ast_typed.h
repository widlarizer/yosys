/* -*- c++ -*-
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Typed views over AstNode.
 *
 *  Each view is a zero-cost wrapper around an already-type-checked AstNode*
 *  that exposes the positional slots described in frontends/verilog/verilog_parser.y
 *  as named getters. See g/ast-grammar.md for the invariants that back
 *  these accessors.
 */

#ifndef AST_TYPED_H
#define AST_TYPED_H

#include "frontends/ast/ast.h"
#include <optional>

YOSYS_NAMESPACE_BEGIN
namespace AST {

// Internal base. A view is an AstNode* that has been validated to have a
// specific AstNodeType. Cheap to copy; does not own the underlying node.
template <AstNodeType Tag>
struct AstView {
	AstNode *node;

	explicit AstView(AstNode *n) : node(n) {
		log_assert(n && n->type == Tag);
	}

	AstNode *raw() const { return node; }
	const AstSrcLocType &loc() const { return node->location; }
	const std::string &str() const { return node->str; }
	std::string &str() { return node->str; }

	static bool matches(const AstNode *n) {
		return n && n->type == Tag;
	}
};

// A typed handle to a specific positional child slot. Lets callers read,
// swap, or replace the slot while keeping unique_ptr ownership intact.
struct ChildSlot {
	AstNode *parent;
	size_t index;

	AstNode *get() const { return parent->children.at(index).get(); }
	AstNode *operator->() const { return get(); }
	explicit operator bool() const {
		return index < parent->children.size() && parent->children[index];
	}

	std::unique_ptr<AstNode> take() {
		return std::move(parent->children.at(index));
	}
	void set(std::unique_ptr<AstNode> n) {
		parent->children.at(index) = std::move(n);
	}
};

// ---------------- Statement-shaped views ----------------

struct AstFor : AstView<AST_FOR> {
	using AstView::AstView;
	// Grammar: children[0]=init (AST_ASSIGN_EQ), [1]=cond, [2]=step (AST_ASSIGN_EQ), [3]=body (AST_BLOCK)
	ChildSlot init() { return {node, 0}; }
	ChildSlot cond() { return {node, 1}; }
	ChildSlot step() { return {node, 2}; }
	ChildSlot body() { return {node, 3}; }

	static std::optional<AstFor> cast(AstNode *n) {
		return matches(n) ? std::optional<AstFor>(AstFor(n)) : std::nullopt;
	}
};

struct AstWhile : AstView<AST_WHILE> {
	using AstView::AstView;
	// Grammar: [0]=cond, [1]=body
	ChildSlot cond() { return {node, 0}; }
	ChildSlot body() { return {node, 1}; }
	static std::optional<AstWhile> cast(AstNode *n) {
		return matches(n) ? std::optional<AstWhile>(AstWhile(n)) : std::nullopt;
	}
};

struct AstRepeat : AstView<AST_REPEAT> {
	using AstView::AstView;
	ChildSlot count() { return {node, 0}; }
	ChildSlot body() { return {node, 1}; }
	static std::optional<AstRepeat> cast(AstNode *n) {
		return matches(n) ? std::optional<AstRepeat>(AstRepeat(n)) : std::nullopt;
	}
};

struct AstAssignEq : AstView<AST_ASSIGN_EQ> {
	using AstView::AstView;
	ChildSlot lhs() { return {node, 0}; }
	ChildSlot rhs() { return {node, 1}; }
	static std::optional<AstAssignEq> cast(AstNode *n) {
		return matches(n) ? std::optional<AstAssignEq>(AstAssignEq(n)) : std::nullopt;
	}
};

struct AstAssignLe : AstView<AST_ASSIGN_LE> {
	using AstView::AstView;
	ChildSlot lhs() { return {node, 0}; }
	ChildSlot rhs() { return {node, 1}; }
	static std::optional<AstAssignLe> cast(AstNode *n) {
		return matches(n) ? std::optional<AstAssignLe>(AstAssignLe(n)) : std::nullopt;
	}
};

struct AstAssign : AstView<AST_ASSIGN> {
	using AstView::AstView;
	ChildSlot lhs() { return {node, 0}; }
	ChildSlot rhs() { return {node, 1}; }
	static std::optional<AstAssign> cast(AstNode *n) {
		return matches(n) ? std::optional<AstAssign>(AstAssign(n)) : std::nullopt;
	}
};

struct AstTernary : AstView<AST_TERNARY> {
	using AstView::AstView;
	ChildSlot cond() { return {node, 0}; }
	ChildSlot then_() { return {node, 1}; }
	ChildSlot else_() { return {node, 2}; }
	static std::optional<AstTernary> cast(AstNode *n) {
		return matches(n) ? std::optional<AstTernary>(AstTernary(n)) : std::nullopt;
	}
};

// ---------------- Range-shaped views ----------------

struct AstRange : AstView<AST_RANGE> {
	using AstView::AstView;
	// After checkRange, AST_RANGE always has 2 children: msb, lsb.
	ChildSlot msb() { return {node, 0}; }
	ChildSlot lsb() { return {node, 1}; }
	bool is_single_bit() const { return node->children.size() == 1; }
	static std::optional<AstRange> cast(AstNode *n) {
		return matches(n) ? std::optional<AstRange>(AstRange(n)) : std::nullopt;
	}
};

// ---------------- case / cond ----------------

struct AstCase : AstView<AST_CASE> {
	using AstView::AstView;
	AstNode *selector() const { return node->children.at(0).get(); }
	// Iterate AST_COND / AST_CONDX / AST_CONDZ children (skip the selector).
	auto conditions_begin() { return node->children.begin() + 1; }
	auto conditions_end() { return node->children.end(); }
	size_t num_conditions() const {
		return node->children.empty() ? 0 : node->children.size() - 1;
	}
	static std::optional<AstCase> cast(AstNode *n) {
		return matches(n) ? std::optional<AstCase>(AstCase(n)) : std::nullopt;
	}
};

// AST_COND / AST_CONDX / AST_CONDZ share shape: zero-or-more label children
// followed by exactly one AST_BLOCK body at the end.
template <AstNodeType Tag>
struct AstCondBase : AstView<Tag> {
	using AstView<Tag>::AstView;
	using AstView<Tag>::node;

	AstNode *body() const {
		log_assert(!node->children.empty());
		AstNode *last = node->children.back().get();
		log_assert(last->type == AST_BLOCK || last->type == AST_GENBLOCK);
		return last;
	}
	size_t num_labels() const {
		return node->children.empty() ? 0 : node->children.size() - 1;
	}
	auto labels_begin() { return node->children.begin(); }
	auto labels_end() {
		log_assert(!node->children.empty());
		return node->children.end() - 1;
	}
};

using AstCond = AstCondBase<AST_COND>;
using AstCondX = AstCondBase<AST_CONDX>;
using AstCondZ = AstCondBase<AST_CONDZ>;

// ---------------- Identifiers / prefix / bit selects ----------------

struct AstIdentifier : AstView<AST_IDENTIFIER> {
	using AstView::AstView;
	// Grammar: zero or one child, which if present is AST_RANGE or AST_MULTIRANGE.
	bool has_bit_select() const { return !node->children.empty(); }
	AstNode *bit_select_raw() const {
		return node->children.empty() ? nullptr : node->children[0].get();
	}
	static std::optional<AstIdentifier> cast(AstNode *n) {
		return matches(n) ? std::optional<AstIdentifier>(AstIdentifier(n)) : std::nullopt;
	}
};

struct AstPrefix : AstView<AST_PREFIX> {
	using AstView::AstView;
	ChildSlot index() { return {node, 0}; }
	ChildSlot suffix() { return {node, 1}; }
	static std::optional<AstPrefix> cast(AstNode *n) {
		return matches(n) ? std::optional<AstPrefix>(AstPrefix(n)) : std::nullopt;
	}
};

// ---------------- Cells & arguments ----------------

struct AstArgument : AstView<AST_ARGUMENT> {
	using AstView::AstView;
	// Grammar: 0 or 1 child (the connected expression).
	bool is_positional() const { return node->str.empty(); }
	bool has_expr() const { return !node->children.empty(); }
	AstNode *expr_or_null() const {
		return node->children.empty() ? nullptr : node->children[0].get();
	}
	static std::optional<AstArgument> cast(AstNode *n) {
		return matches(n) ? std::optional<AstArgument>(AstArgument(n)) : std::nullopt;
	}
};

struct AstCell : AstView<AST_CELL> {
	using AstView::AstView;
	// Grammar: children[0]=AST_CELLTYPE, then runs of AST_PARASET, then AST_ARGUMENT.
	AstNode *celltype() const {
		log_assert(!node->children.empty());
		log_assert(node->children[0]->type == AST_CELLTYPE);
		return node->children[0].get();
	}
	static std::optional<AstCell> cast(AstNode *n) {
		return matches(n) ? std::optional<AstCell>(AstCell(n)) : std::nullopt;
	}
};

// ---------------- Blocks ----------------

struct AstBlock : AstView<AST_BLOCK> {
	using AstView::AstView;
	// Variadic: children are behavioral statements in declaration order.
	size_t size() const { return node->children.size(); }
	AstNode *at(size_t i) const { return node->children.at(i).get(); }
	static std::optional<AstBlock> cast(AstNode *n) {
		return matches(n) ? std::optional<AstBlock>(AstBlock(n)) : std::nullopt;
	}
};

} // namespace AST
YOSYS_NAMESPACE_END

#endif
