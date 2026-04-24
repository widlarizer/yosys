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

protected:
	// Factory helper for derived views to use in their build() functions.
	// Uses C++17 fold expressions to efficiently pack children.
	template <typename... Args>
	static std::unique_ptr<AstNode> make_node(const AstSrcLocType &loc, Args&&... child_nodes) {
		auto n = std::make_unique<AstNode>(loc, Tag);
		n->location = loc;

		if constexpr (sizeof...(Args) > 0) {
			n->children.reserve(sizeof...(Args));
			(n->children.push_back(std::forward<Args>(child_nodes)), ...);
		}
		return n;
	}
};


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

#define DEFINE_AST_CAST(StructName, Tag) \
		static std::optional<StructName> cast(AstNode *n) { \
			return AstView<Tag>::matches(n) ? std::optional<StructName>(StructName(n)) : std::nullopt; \
		}

#define DEFINE_AST_VIEW_1(StructName, Tag, arg0) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot arg0() { return {node, 0}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0) { \
			return make_node(loc, std::move(arg0)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_2(StructName, Tag, arg0, arg1) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot arg0() { return {node, 0}; } \
		ChildSlot arg1() { return {node, 1}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1) { \
			return make_node(loc, std::move(arg0), std::move(arg1)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_3(StructName, Tag, arg0, arg1, arg2) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot arg0() { return {node, 0}; } \
		ChildSlot arg1() { return {node, 1}; } \
		ChildSlot arg2() { return {node, 2}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1, std::unique_ptr<AstNode> arg2) { \
			return make_node(loc, std::move(arg0), std::move(arg1), std::move(arg2)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_4(StructName, Tag, arg0, arg1, arg2, arg3) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot arg0() { return {node, 0}; } \
		ChildSlot arg1() { return {node, 1}; } \
		ChildSlot arg2() { return {node, 2}; } \
		ChildSlot arg3() { return {node, 3}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1, std::unique_ptr<AstNode> arg2, std::unique_ptr<AstNode> arg3) { \
			return make_node(loc, std::move(arg0), std::move(arg1), std::move(arg2), std::move(arg3)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

DEFINE_AST_VIEW_4(AstFor,      AST_FOR,       init, cond, step, body)
DEFINE_AST_VIEW_2(AstWhile,    AST_WHILE,     cond, body)

DEFINE_AST_VIEW_2(AstAssign,   AST_ASSIGN,    lhs, rhs)
DEFINE_AST_VIEW_2(AstAssignEq, AST_ASSIGN_EQ, lhs, rhs)
DEFINE_AST_VIEW_2(AstAssignLe, AST_ASSIGN_LE, lhs, rhs)

DEFINE_AST_VIEW_3(AstTernary,  AST_TERNARY,   cond, then_, else_)

DEFINE_AST_VIEW_2(AstRange,    AST_RANGE,     msb, lsb)

DEFINE_AST_VIEW_2(AstRepeat,   AST_REPEAT,    count, body)

DEFINE_AST_VIEW_2(AstPrefix,   AST_PREFIX,    index, suffix)

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
	static std::optional<AstCondBase> cast(AstNode *n) {
		return AstView<Tag>::matches(n) ? std::optional<AstCondBase>(AstCondBase(n)) : std::nullopt;
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
