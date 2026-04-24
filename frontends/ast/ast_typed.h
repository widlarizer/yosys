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


// ChildSlot: a (parent, index) handle into parent->children. Cheap to copy.
// Provides read access (get/operator->/deref) and ownership-moving
// take()/set() for rewriting the slot without touching the rest of the vector.
struct ChildSlot {
	AstNode *parent;
	size_t index;

	AstNode *get() const { return parent->children.at(index).get(); }
	AstNode *operator->() const { return get(); }
	AstNode &operator*() const { return *get(); }
	explicit operator bool() const {
		return index < parent->children.size() && parent->children[index];
	}

	std::unique_ptr<AstNode> take() {
		return std::move(parent->children.at(index));
	}
	void set(std::unique_ptr<AstNode> n) {
		parent->children.at(index) = std::move(n);
	}

	// Typed reinterpret of the slot contents; asserts type.
	template <typename View>
	View as() const { return View(get()); }
};

// reshape_as: mutate `node` in-place so that it becomes a View of kind Tag with
// exactly the supplied children. The node's own identity (pointer, location,
// attributes, str, bits, ...) is preserved — only `type` and `children` are
// overwritten. This is how the simplifier "morphs" one AstNode into a node of
// a different type while keeping the unique_ptr that owns it intact.
template <AstNodeType Tag, typename... Args>
AstView<Tag> reshape_as(AstNode *node, Args&&... new_children) {
	log_assert(node);
	node->type = Tag;
	node->children.clear();
	if constexpr (sizeof...(Args) > 0) {
		node->children.reserve(sizeof...(Args));
		(node->children.push_back(std::forward<Args>(new_children)), ...);
	}
	return AstView<Tag>(node);
}

// Variadic form: pass a ready-made vector of children.
template <AstNodeType Tag>
AstView<Tag> reshape_as_vec(AstNode *node, std::vector<std::unique_ptr<AstNode>> new_children) {
	log_assert(node);
	node->type = Tag;
	node->children = std::move(new_children);
	return AstView<Tag>(node);
}

#define DEFINE_AST_CAST(StructName, Tag) \
		static std::optional<StructName> cast(AstNode *n) { \
			return AstView<Tag>::matches(n) ? std::optional<StructName>(StructName(n)) : std::nullopt; \
		}

#define DEFINE_AST_VIEW_1(StructName, Tag, arg0) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot arg0() const { return {node, 0}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0) { \
			return make_node(loc, std::move(arg0)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_2(StructName, Tag, arg0, arg1) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot arg0() const { return {node, 0}; } \
		ChildSlot arg1() const { return {node, 1}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1) { \
			return make_node(loc, std::move(arg0), std::move(arg1)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_3(StructName, Tag, arg0, arg1, arg2) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot arg0() const { return {node, 0}; } \
		ChildSlot arg1() const { return {node, 1}; } \
		ChildSlot arg2() const { return {node, 2}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1, std::unique_ptr<AstNode> arg2) { \
			return make_node(loc, std::move(arg0), std::move(arg1), std::move(arg2)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_4(StructName, Tag, arg0, arg1, arg2, arg3) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot arg0() const { return {node, 0}; } \
		ChildSlot arg1() const { return {node, 1}; } \
		ChildSlot arg2() const { return {node, 2}; } \
		ChildSlot arg3() const { return {node, 3}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1, std::unique_ptr<AstNode> arg2, std::unique_ptr<AstNode> arg3) { \
			return make_node(loc, std::move(arg0), std::move(arg1), std::move(arg2), std::move(arg3)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

// Loops / assigns / ternary / range / prefix / repeat
DEFINE_AST_VIEW_4(AstFor,      AST_FOR,       init, cond, step, body)
DEFINE_AST_VIEW_4(AstGenFor,   AST_GENFOR,    init, cond, step, body)
DEFINE_AST_VIEW_2(AstWhile,    AST_WHILE,     cond, body)

DEFINE_AST_VIEW_2(AstAssign,   AST_ASSIGN,    lhs, rhs)
DEFINE_AST_VIEW_2(AstAssignEq, AST_ASSIGN_EQ, lhs, rhs)
DEFINE_AST_VIEW_2(AstAssignLe, AST_ASSIGN_LE, lhs, rhs)

DEFINE_AST_VIEW_3(AstTernary,  AST_TERNARY,   cond, then_, else_)

DEFINE_AST_VIEW_2(AstRange,    AST_RANGE,     msb, lsb)

DEFINE_AST_VIEW_2(AstRepeat,   AST_REPEAT,    count, body)

DEFINE_AST_VIEW_2(AstPrefix,   AST_PREFIX,    index, suffix)

DEFINE_AST_VIEW_2(AstReplicate, AST_REPLICATE, count, pattern)

DEFINE_AST_VIEW_2(AstCastSize, AST_CAST_SIZE, target, expr)
DEFINE_AST_VIEW_2(AstToBits,   AST_TO_BITS,   size, expr)

DEFINE_AST_VIEW_1(AstSelfSz,   AST_SELFSZ,    expr)

// Sensitivity events
DEFINE_AST_VIEW_1(AstPosedge,  AST_POSEDGE,   expr)
DEFINE_AST_VIEW_1(AstNegedge,  AST_NEGEDGE,   expr)
DEFINE_AST_VIEW_1(AstEdge,     AST_EDGE,      expr)

// Cell array wrapper
DEFINE_AST_VIEW_2(AstCellArray, AST_CELLARRAY, range, cell)

// Assertions / formal — single-predicate
DEFINE_AST_VIEW_1(AstAssert,   AST_ASSERT,    predicate)
DEFINE_AST_VIEW_1(AstAssume,   AST_ASSUME,    predicate)
DEFINE_AST_VIEW_1(AstLive,     AST_LIVE,      predicate)
DEFINE_AST_VIEW_1(AstFair,     AST_FAIR,      predicate)
DEFINE_AST_VIEW_1(AstCover,    AST_COVER,     predicate)

// Typedef (single child: underlying type)
DEFINE_AST_VIEW_1(AstTypedef,  AST_TYPEDEF,   underlying)

// Initial (single child: AST_BLOCK body)
DEFINE_AST_VIEW_1(AstInitial,  AST_INITIAL,   body)

// ---------------- Unary / binary op templates ----------------

// All unary/binary op types share a trivial shape. A tiny typed view for each
// lets call sites say `AstNeg{node}.operand()` instead of `children[0]`.
template <AstNodeType Tag>
struct AstUnaryOp : AstView<Tag> {
	using AstView<Tag>::AstView;
	using AstView<Tag>::node;
	ChildSlot operand() const { return {node, 0}; }
	static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg) {
		return AstView<Tag>::make_node(loc, std::move(arg));
	}
	static std::optional<AstUnaryOp> cast(AstNode *n) {
		return AstView<Tag>::matches(n) ? std::optional<AstUnaryOp>(AstUnaryOp(n)) : std::nullopt;
	}
};

template <AstNodeType Tag>
struct AstBinaryOp : AstView<Tag> {
	using AstView<Tag>::AstView;
	using AstView<Tag>::node;
	ChildSlot lhs() const { return {node, 0}; }
	ChildSlot rhs() const { return {node, 1}; }
	static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> l, std::unique_ptr<AstNode> r) {
		return AstView<Tag>::make_node(loc, std::move(l), std::move(r));
	}
	static std::optional<AstBinaryOp> cast(AstNode *n) {
		return AstView<Tag>::matches(n) ? std::optional<AstBinaryOp>(AstBinaryOp(n)) : std::nullopt;
	}
};

using AstBitNot       = AstUnaryOp<AST_BIT_NOT>;
using AstReduceAnd    = AstUnaryOp<AST_REDUCE_AND>;
using AstReduceOr     = AstUnaryOp<AST_REDUCE_OR>;
using AstReduceXor    = AstUnaryOp<AST_REDUCE_XOR>;
using AstReduceXnor   = AstUnaryOp<AST_REDUCE_XNOR>;
using AstReduceBool   = AstUnaryOp<AST_REDUCE_BOOL>;
using AstLogicNot     = AstUnaryOp<AST_LOGIC_NOT>;
using AstPos          = AstUnaryOp<AST_POS>;
using AstNeg          = AstUnaryOp<AST_NEG>;
using AstToSigned     = AstUnaryOp<AST_TO_SIGNED>;
using AstToUnsigned   = AstUnaryOp<AST_TO_UNSIGNED>;

using AstBitAnd       = AstBinaryOp<AST_BIT_AND>;
using AstBitOr        = AstBinaryOp<AST_BIT_OR>;
using AstBitXor       = AstBinaryOp<AST_BIT_XOR>;
using AstBitXnor      = AstBinaryOp<AST_BIT_XNOR>;
using AstShiftLeft    = AstBinaryOp<AST_SHIFT_LEFT>;
using AstShiftRight   = AstBinaryOp<AST_SHIFT_RIGHT>;
using AstShiftSLeft   = AstBinaryOp<AST_SHIFT_SLEFT>;
using AstShiftSRight  = AstBinaryOp<AST_SHIFT_SRIGHT>;
using AstLt           = AstBinaryOp<AST_LT>;
using AstLe           = AstBinaryOp<AST_LE>;
using AstEq           = AstBinaryOp<AST_EQ>;
using AstNe           = AstBinaryOp<AST_NE>;
using AstEqX          = AstBinaryOp<AST_EQX>;
using AstNeX          = AstBinaryOp<AST_NEX>;
using AstGe           = AstBinaryOp<AST_GE>;
using AstGt           = AstBinaryOp<AST_GT>;
using AstAdd          = AstBinaryOp<AST_ADD>;
using AstSub          = AstBinaryOp<AST_SUB>;
using AstMul          = AstBinaryOp<AST_MUL>;
using AstDiv          = AstBinaryOp<AST_DIV>;
using AstMod          = AstBinaryOp<AST_MOD>;
using AstPow          = AstBinaryOp<AST_POW>;
using AstLogicAnd     = AstBinaryOp<AST_LOGIC_AND>;
using AstLogicOr      = AstBinaryOp<AST_LOGIC_OR>;

// ---------------- case / cond ----------------

struct AstCase : AstView<AST_CASE> {
	using AstView::AstView;
	AstNode *selector() const { return node->children.at(0).get(); }
	ChildSlot selector_slot() const { return {node, 0}; }
	auto conditions_begin() { return node->children.begin() + 1; }
	auto conditions_end() { return node->children.end(); }
	size_t num_conditions() const {
		return node->children.empty() ? 0 : node->children.size() - 1;
	}
	static std::optional<AstCase> cast(AstNode *n) {
		return matches(n) ? std::optional<AstCase>(AstCase(n)) : std::nullopt;
	}
};

// AST_GENCASE mirrors AST_CASE with GENBLOCK bodies.
struct AstGenCase : AstView<AST_GENCASE> {
	using AstView::AstView;
	AstNode *selector() const { return node->children.at(0).get(); }
	ChildSlot selector_slot() const { return {node, 0}; }
	auto conditions_begin() { return node->children.begin() + 1; }
	auto conditions_end() { return node->children.end(); }
	size_t num_conditions() const {
		return node->children.empty() ? 0 : node->children.size() - 1;
	}
	static std::optional<AstGenCase> cast(AstNode *n) {
		return matches(n) ? std::optional<AstGenCase>(AstGenCase(n)) : std::nullopt;
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
	ChildSlot body_slot() const {
		log_assert(!node->children.empty());
		return {node, node->children.size() - 1};
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

using AstCond  = AstCondBase<AST_COND>;
using AstCondX = AstCondBase<AST_CONDX>;
using AstCondZ = AstCondBase<AST_CONDZ>;

struct AstGenIf : AstView<AST_GENIF> {
	using AstView::AstView;
	ChildSlot cond()      const { return {node, 0}; }
	ChildSlot then_body() const { return {node, 1}; }
	bool has_else_body() const { return node->children.size() > 2; }
	ChildSlot else_body() const {
		log_assert(node->children.size() > 2);
		return {node, 2};
	}
	DEFINE_AST_CAST(AstGenIf, AST_GENIF)
};

struct AstIdentifier : AstView<AST_IDENTIFIER> {
	using AstView::AstView;
	// Grammar: zero or one child, which if present is AST_RANGE or AST_MULTIRANGE.
	bool has_bit_select() const { return !node->children.empty(); }
	AstNode *bit_select_raw() const {
		return node->children.empty() ? nullptr : node->children[0].get();
	}
	std::optional<ChildSlot> bit_select_slot() const {
		return node->children.empty() ? std::nullopt
		                              : std::optional<ChildSlot>(ChildSlot{node, 0});
	}
	static std::optional<AstIdentifier> cast(AstNode *n) {
		return matches(n) ? std::optional<AstIdentifier>(AstIdentifier(n)) : std::nullopt;
	}
};

struct AstArgument : AstView<AST_ARGUMENT> {
	using AstView::AstView;
	// Grammar: 0 or 1 child (the connected expression).
	bool is_positional() const { return node->str.empty(); }
	bool has_expr() const { return !node->children.empty(); }
	AstNode *expr_or_null() const {
		return node->children.empty() ? nullptr : node->children[0].get();
	}
	std::optional<ChildSlot> expr_slot() const {
		return node->children.empty() ? std::nullopt
		                              : std::optional<ChildSlot>(ChildSlot{node, 0});
	}
	static std::optional<AstArgument> cast(AstNode *n) {
		return matches(n) ? std::optional<AstArgument>(AstArgument(n)) : std::nullopt;
	}
};

struct AstParaset : AstView<AST_PARASET> {
	using AstView::AstView;
	AstNode *expr() const {
		log_assert(!node->children.empty());
		return node->children[0].get();
	}
	ChildSlot expr_slot() const { return {node, 0}; }
	bool is_positional() const { return node->str.empty(); }
	static std::optional<AstParaset> cast(AstNode *n) {
		return matches(n) ? std::optional<AstParaset>(AstParaset(n)) : std::nullopt;
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
	ChildSlot celltype_slot() const { return {node, 0}; }
	// Iterate children[1..]; caller dispatches on AST_PARASET vs AST_ARGUMENT.
	auto body_begin() { return node->children.begin() + 1; }
	auto body_end()   { return node->children.end(); }
	static std::optional<AstCell> cast(AstNode *n) {
		return matches(n) ? std::optional<AstCell>(AstCell(n)) : std::nullopt;
	}
};

struct AstBlock : AstView<AST_BLOCK> {
	using AstView::AstView;
	// Variadic: children are behavioral statements in declaration order.
	size_t size() const { return node->children.size(); }
	AstNode *at(size_t i) const { return node->children.at(i).get(); }
	ChildSlot slot(size_t i) const { return {node, i}; }
	auto begin() { return node->children.begin(); }
	auto end()   { return node->children.end(); }
	static std::optional<AstBlock> cast(AstNode *n) {
		return matches(n) ? std::optional<AstBlock>(AstBlock(n)) : std::nullopt;
	}
};

struct AstGenBlock : AstView<AST_GENBLOCK> {
	using AstView::AstView;
	size_t size() const { return node->children.size(); }
	AstNode *at(size_t i) const { return node->children.at(i).get(); }
	ChildSlot slot(size_t i) const { return {node, i}; }
	auto begin() { return node->children.begin(); }
	auto end()   { return node->children.end(); }
	static std::optional<AstGenBlock> cast(AstNode *n) {
		return matches(n) ? std::optional<AstGenBlock>(AstGenBlock(n)) : std::nullopt;
	}
};

struct AstConcat : AstView<AST_CONCAT> {
	using AstView::AstView;
	size_t size() const { return node->children.size(); }
	AstNode *at(size_t i) const { return node->children.at(i).get(); }
	ChildSlot slot(size_t i) const { return {node, i}; }
	auto begin() { return node->children.begin(); }
	auto end()   { return node->children.end(); }
	static std::optional<AstConcat> cast(AstNode *n) {
		return matches(n) ? std::optional<AstConcat>(AstConcat(n)) : std::nullopt;
	}
};

// AST_MULTIRANGE: variadic list of AST_RANGE children (>= 2).
struct AstMultirange : AstView<AST_MULTIRANGE> {
	using AstView::AstView;
	size_t size() const { return node->children.size(); }
	AstNode *at(size_t i) const { return node->children.at(i).get(); }
	ChildSlot slot(size_t i) const { return {node, i}; }
	auto begin() { return node->children.begin(); }
	auto end()   { return node->children.end(); }
	static std::optional<AstMultirange> cast(AstNode *n) {
		return matches(n) ? std::optional<AstMultirange>(AstMultirange(n)) : std::nullopt;
	}
};

// AST_ALWAYS: final child is AST_BLOCK; preceding children are sensitivity
// events.
struct AstAlways : AstView<AST_ALWAYS> {
	using AstView::AstView;
	AstNode *body() const {
		log_assert(!node->children.empty());
		AstNode *last = node->children.back().get();
		log_assert(last->type == AST_BLOCK);
		return last;
	}
	ChildSlot body_slot() const {
		log_assert(!node->children.empty());
		return {node, node->children.size() - 1};
	}
	auto sensitivity_begin() { return node->children.begin(); }
	auto sensitivity_end() {
		log_assert(!node->children.empty());
		return node->children.end() - 1;
	}
	static std::optional<AstAlways> cast(AstNode *n) {
		return matches(n) ? std::optional<AstAlways>(AstAlways(n)) : std::nullopt;
	}
};

} // namespace AST
YOSYS_NAMESPACE_END

#endif
