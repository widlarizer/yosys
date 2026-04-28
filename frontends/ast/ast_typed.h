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


// A type-level set of acceptable node types for a child slot.
template <AstNodeType... Allowed>
struct ChildConstraint {
	static bool accepts(const AstNode *n) {
		return n && ((n->type == Allowed) || ...);
	}
};

// Compose multiple constraints with OR semantics.
template <typename... Constraints>
struct ChildConstraintOr {
	static bool accepts(const AstNode *n) {
		return (Constraints::accepts(n) || ...);
	}
};

// Expression: all nodes that can be returned from the expr production in the grammar.
// This includes arithmetic, bitwise, logical, comparison ops, casts, literals, function calls, etc.
using Expression = ChildConstraint<
	// Literals & Constants
	AST_CONSTANT,
	// Variables & Identifiers
	AST_IDENTIFIER,
	// Function and task calls
	AST_FCALL, AST_TCALL,
	// Unary arithmetic
	AST_POS, AST_NEG,
	// Unary bitwise
	AST_BIT_NOT,
	AST_REDUCE_AND, AST_REDUCE_OR, AST_REDUCE_XOR, AST_REDUCE_XNOR,
	// Binary arithmetic
	AST_ADD, AST_SUB, AST_MUL, AST_DIV, AST_MOD, AST_POW,
	// Binary bitwise
	AST_BIT_AND, AST_BIT_OR, AST_BIT_XOR, AST_BIT_XNOR,
	// Shift operations
	AST_SHIFT_LEFT, AST_SHIFT_RIGHT, AST_SHIFT_SLEFT, AST_SHIFT_SRIGHT,
	// Comparison
	AST_LT, AST_LE, AST_EQ, AST_NE, AST_EQX, AST_NEX, AST_GE, AST_GT,
	// Logical
	AST_LOGIC_AND, AST_LOGIC_OR, AST_LOGIC_NOT,
	// Ternary conditional
	AST_TERNARY,
	// Concatenation and replication
	AST_CONCAT, AST_REPLICATE,
	// Casting and conversion
	AST_TO_BITS, AST_TO_SIGNED, AST_TO_UNSIGNED, AST_CAST_SIZE,
	// Special expression forms
	AST_PREFIX, AST_REALVALUE, AST_SELFSZ,
	// Post-simplify only
	AST_MEMRD, AST_MEMWR, AST_AUTOWIRE, AST_SHIFTX, AST_SHIFT
>;

// Post-simplify unused: AST_MEMINIT

// Shared constraint: declarations present in both behavioral and module contexts.
using CommonDeclarations = ChildConstraint<
	AST_PARAMETER, AST_LOCALPARAM,
	AST_WIRE, AST_MEMORY, AST_TYPEDEF
>;

// Shared constraint: formal assertions present in both behavioral and module contexts.
using FormalAssertions = ChildConstraint<
	AST_ASSERT, AST_ASSUME, AST_LIVE, AST_FAIR, AST_COVER
>;

// Shared constraint: generic structures (if/for/case/block) present in both contexts.
using GenericStructures = ChildConstraint<
	AST_GENBLOCK, AST_GENIF, AST_GENFOR, AST_GENCASE
>;

// BehavioralStatement: nodes that can appear inside behavioral blocks (always/function/task).
// Composed from shared constraints and behavioral-specific nodes.
using BehavioralStatement = ChildConstraintOr<
	ChildConstraint<AST_ASSIGN_EQ, AST_ASSIGN_LE>,
	ChildConstraint<AST_BLOCK>,
	GenericStructures,
	ChildConstraint<AST_COND, AST_CONDX, AST_CONDZ, AST_FOR, AST_WHILE, AST_REPEAT>,
	ChildConstraint<AST_CASE>,
	FormalAssertions,
	ChildConstraint<AST_TCALL>,
	CommonDeclarations
>;

// ModuleBodyStatement: nodes that can appear at module level.
// Composed from shared constraints and module-level-specific nodes.
using ModuleBodyStatement = ChildConstraintOr<
	ChildConstraint<AST_DEFPARAM>,
	CommonDeclarations,
	ChildConstraint<AST_ALWAYS, AST_INITIAL>,
	ChildConstraint<AST_CELL>,
	ChildConstraint<AST_ASSIGN>,
	FormalAssertions,
	GenericStructures,
	ChildConstraint<AST_ENUM, AST_STRUCT>,
	ChildConstraint<AST_BIND>
>;

// // Statement: union of behavioral and module-level for backward compatibility.
// using Statement = ChildConstraintOr<
// 	BehavioralStatement,
// 	ModuleBodyStatement
// >;

struct Anything {
	static bool accepts(AstNode* node) {
		(void)node; return true;
	}
};

// ChildSlot: a (parent, index) handle into parent->children. Cheap to copy.
// Provides read access (get/operator->/deref) and ownership-moving
// take()/set() for rewriting the slot without touching the rest of the vector.
template <typename Constraint>
struct ChildSlot {
	AstNode *parent;
	size_t index;

	AstNode *get() const {
		auto *c = parent->children.at(index).get();
		if (!c)
			log_error("Null node\n");
		if (!Constraint::accepts(c))
			log_error("Improper node type (%d)\n", (int)c->type);
		return c;
	}

	void set(std::unique_ptr<AstNode> n) {
		log_assert(!n || Constraint::accepts(n.get())); // validate on write
		parent->children.at(index) = std::move(n);
	}
	AstNode *operator->() const { return get(); }
	AstNode &operator*() const { return *get(); }
	explicit operator bool() const {
		return index < parent->children.size() && parent->children[index];
	}

	std::unique_ptr<AstNode> take() {
		return std::move(parent->children.at(index));
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

#define DEFINE_AST_VIEW_1(StructName, Tag, arg0, con0) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot<con0> arg0() const { return {node, 0}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0) { \
			return make_node(loc, std::move(arg0)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_2(StructName, Tag, arg0, con0, arg1, con1) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot<con0> arg0() const { return {node, 0}; } \
		ChildSlot<con1> arg1() const { return {node, 1}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1) { \
			return make_node(loc, std::move(arg0), std::move(arg1)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_3(StructName, Tag, arg0, con0, arg1, con1, arg2, con2) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot<con0> arg0() const { return {node, 0}; } \
		ChildSlot<con1> arg1() const { return {node, 1}; } \
		ChildSlot<con2> arg2() const { return {node, 2}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1, std::unique_ptr<AstNode> arg2) { \
			return make_node(loc, std::move(arg0), std::move(arg1), std::move(arg2)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

#define DEFINE_AST_VIEW_4(StructName, Tag, arg0, con0, arg1, con1, arg2, con2, arg3, con3) \
	struct StructName : AstView<Tag> { \
		using AstView<Tag>::AstView; \
		ChildSlot<con0> arg0() const { return {node, 0}; } \
		ChildSlot<con1> arg1() const { return {node, 1}; } \
		ChildSlot<con2> arg2() const { return {node, 2}; } \
		ChildSlot<con3> arg3() const { return {node, 3}; } \
		static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> arg0, std::unique_ptr<AstNode> arg1, std::unique_ptr<AstNode> arg2, std::unique_ptr<AstNode> arg3) { \
			return make_node(loc, std::move(arg0), std::move(arg1), std::move(arg2), std::move(arg3)); \
		} \
		DEFINE_AST_CAST(StructName, Tag) \
	};

// VariadicView: shared base for nodes whose grammar is "zero-or-more children of a
// uniform constraint". Used directly for AST_BLOCK, AST_CONCAT, AST_MULTIRANGE,
// AST_STRUCT, AST_UNION, AST_GENBLOCK; subclassed (or aliased) for FCALL/TCALL/PRIMITIVE
// which add domain-specific names like arg(i).
template <AstNodeType Tag, typename Constraint = Anything>
struct VariadicView : AstView<Tag> {
	using AstView<Tag>::AstView;
	using AstView<Tag>::node;

	size_t size() const { return node->children.size(); }
	bool empty() const { return node->children.empty(); }
	AstNode *at(size_t i) const { return node->children.at(i).get(); }
	ChildSlot<Constraint> slot(size_t i) const { return {node, i}; }
	auto begin() const { return node->children.begin(); }
	auto end()   const { return node->children.end(); }
	auto &raw_children() const { return node->children; }

	// num_args/arg are the names used by call-shaped nodes (FCALL/TCALL/PRIMITIVE).
	// Provided here so any VariadicView reads naturally as "a list of arguments".
	size_t num_args() const { return node->children.size(); }
	AstNode *arg(size_t i) const { return node->children.at(i).get(); }
	ChildSlot<Constraint> arg_slot(size_t i) const { return {node, i}; }

	static std::optional<VariadicView> cast(AstNode *n) {
		return AstView<Tag>::matches(n)
			? std::optional<VariadicView>(VariadicView(n))
			: std::nullopt;
	}
};

// ParamLikeView: AST_PARAMETER, AST_LOCALPARAM, AST_ENUM_ITEM all share the
// shape [value, range_or_wiretype_or_realvalue?]. (The third "type tag" child is
// not part of this template — callers query has_range()/has_wiretype()/etc.)
template <AstNodeType Tag>
struct ParamLikeView : AstView<Tag> {
	using AstView<Tag>::AstView;
	using AstView<Tag>::node;

	bool has_value() const { return !node->children.empty(); }
	AstNode *value() const {
		log_assert(!node->children.empty());
		return node->children[0].get();
	}
	AstNode *value_or_null() const {
		return node->children.empty() ? nullptr : node->children[0].get();
	}
	ChildSlot<Anything> value_slot() const { return {node, 0}; }

	// children[1] (if present) is one of AST_RANGE / AST_WIRETYPE / AST_REALVALUE.
	AstNode *second_or_null() const {
		return node->children.size() < 2 ? nullptr : node->children[1].get();
	}

	bool has_range() const {
		return node->children.size() >= 2 && node->children[1]->type == AST_RANGE;
	}
	AstNode *range_or_null() const {
		return has_range() ? node->children[1].get() : nullptr;
	}
	ChildSlot<Anything> range_slot() const {
		log_assert(has_range());
		return {node, 1};
	}

	bool has_wiretype() const {
		return node->children.size() >= 2 && node->children[1]->type == AST_WIRETYPE;
	}
	AstNode *wiretype_or_null() const {
		return has_wiretype() ? node->children[1].get() : nullptr;
	}

	bool has_realvalue() const {
		return node->children.size() >= 2 && node->children[1]->type == AST_REALVALUE;
	}

	static std::optional<ParamLikeView> cast(AstNode *n) {
		return AstView<Tag>::matches(n) ? std::optional<ParamLikeView>(ParamLikeView(n)) : std::nullopt;
	}
};

DEFINE_AST_VIEW_4(AstFor,      AST_FOR,       init, Anything, cond, Expression, step, Anything, body, BehavioralStatement)
DEFINE_AST_VIEW_4(AstGenFor,   AST_GENFOR,    init, Anything, cond, Anything, step, Anything, body, Anything)
DEFINE_AST_VIEW_2(AstWhile,    AST_WHILE,     cond, Expression, body, BehavioralStatement)

DEFINE_AST_VIEW_2(AstAssign,   AST_ASSIGN,    lhs, Expression, rhs, Expression)
DEFINE_AST_VIEW_2(AstAssignEq, AST_ASSIGN_EQ, lhs, Expression, rhs, Expression)
DEFINE_AST_VIEW_2(AstAssignLe, AST_ASSIGN_LE, lhs, Expression, rhs, Expression)

DEFINE_AST_VIEW_3(AstTernary,  AST_TERNARY,   cond, Expression, then_, Expression, else_, Expression)

DEFINE_AST_VIEW_2(AstRange,    AST_RANGE,     msb, Expression, lsb, Expression)

DEFINE_AST_VIEW_2(AstRepeat,   AST_REPEAT,    count, Expression, body, BehavioralStatement)

DEFINE_AST_VIEW_2(AstPrefix,   AST_PREFIX,    index, Expression, suffix, Anything)

DEFINE_AST_VIEW_2(AstReplicate, AST_REPLICATE, count, Expression, pattern, Expression)

DEFINE_AST_VIEW_2(AstCastSize, AST_CAST_SIZE, target, Anything, expr, Expression)
DEFINE_AST_VIEW_2(AstToBits,   AST_TO_BITS,   size, Expression, expr, Expression)

DEFINE_AST_VIEW_1(AstSelfSz,   AST_SELFSZ,    expr, Expression)

// Sensitivity events
DEFINE_AST_VIEW_1(AstPosedge,  AST_POSEDGE,   expr, Expression)
DEFINE_AST_VIEW_1(AstNegedge,  AST_NEGEDGE,   expr, Expression)
DEFINE_AST_VIEW_1(AstEdge,     AST_EDGE,      expr, Expression)

// Cell array wrapper
DEFINE_AST_VIEW_2(AstCellArray, AST_CELLARRAY, range, Anything, cell, Anything)

// Assertions / formal — single-predicate
DEFINE_AST_VIEW_1(AstAssert,   AST_ASSERT,    predicate, Expression)
DEFINE_AST_VIEW_1(AstAssume,   AST_ASSUME,    predicate, Expression)
DEFINE_AST_VIEW_1(AstLive,     AST_LIVE,      predicate, Expression)
DEFINE_AST_VIEW_1(AstFair,     AST_FAIR,      predicate, Expression)
DEFINE_AST_VIEW_1(AstCover,    AST_COVER,     predicate, Expression)

// Typedef (single child: underlying type)
DEFINE_AST_VIEW_1(AstTypedef,  AST_TYPEDEF,   underlying, Anything)

// Initial (single child: AST_BLOCK body) - body contains behavioral statements
DEFINE_AST_VIEW_1(AstInitial,  AST_INITIAL,   body, BehavioralStatement)

// Declarations
struct AstWire : AstView<AST_WIRE> {
	using AstView::AstView;
	// Grammar: optional packed range, optional wiretype, optional unpacked range
	// children[0?] = AST_RANGE | AST_MULTIRANGE  (packed)
	// children[1?] = AST_WIRETYPE                (if custom type)
	// children[2?] = AST_RANGE                   (unpacked)
	bool has_packed() const { return !node->children.empty() &&
		(node->children[0]->type == AST_RANGE || node->children[0]->type == AST_MULTIRANGE); }
	bool has_unpacked() const {
		if (node->children.size() < 2) return false;
		size_t start_idx = 0;
		if (has_packed()) start_idx++;
		if (start_idx < node->children.size() && node->children[start_idx]->type == AST_WIRETYPE) start_idx++;
		return start_idx < node->children.size() && node->children[start_idx]->type == AST_RANGE;
	}
	static std::optional<AstWire> cast(AstNode *n) {
		return matches(n) ? std::optional<AstWire>(AstWire(n)) : std::nullopt;
	}
};

struct AstMemory : AstView<AST_MEMORY> {
	using AstView::AstView;
	// Grammar: packed range, optional wiretype, address range
	// children[0] = AST_RANGE (element width)
	// children[1?] = AST_WIRETYPE
	// children[2] = AST_RANGE | AST_MULTIRANGE (address)
	AstNode *data_range() const {
		log_assert(!node->children.empty());
		return node->children[0].get();
	}
	AstNode *addr_range() const {
		log_assert(node->children.size() >= 2);
		return node->children[node->children.size() - 1].get();
	}
	static std::optional<AstMemory> cast(AstNode *n) {
		return matches(n) ? std::optional<AstMemory>(AstMemory(n)) : std::nullopt;
	}
};

// Grammar: children[0] = default value expr; children[1] (optional) is AST_RANGE
// (sized parameter), AST_WIRETYPE (custom-typed), or AST_REALVALUE (real-typed).
using AstParameter  = ParamLikeView<AST_PARAMETER>;
using AstLocalparam = ParamLikeView<AST_LOCALPARAM>;

// Tag-agnostic view over any of AST_PARAMETER / AST_LOCALPARAM / AST_ENUM_ITEM.
// Useful when the simplifier treats the three identically (e.g. resolving an
// AST_IDENTIFIER reference whose target may be a parameter-like declaration).
struct AstAnyParamLike {
	AstNode *node;
	static bool matches(const AstNode *n) {
		return n && (n->type == AST_PARAMETER || n->type == AST_LOCALPARAM || n->type == AST_ENUM_ITEM);
	}
	explicit AstAnyParamLike(AstNode *n) : node(n) { log_assert(matches(n)); }
	bool has_value() const { return !node->children.empty(); }
	AstNode *value() const {
		log_assert(!node->children.empty());
		return node->children[0].get();
	}
	bool has_range() const {
		return node->children.size() >= 2 && node->children[1]->type == AST_RANGE;
	}
	AstNode *range_or_null() const {
		return has_range() ? node->children[1].get() : nullptr;
	}
	bool has_realvalue() const {
		return node->children.size() >= 2 && node->children[1]->type == AST_REALVALUE;
	}
	bool has_wiretype() const {
		return node->children.size() >= 2 && node->children[1]->type == AST_WIRETYPE;
	}
	static std::optional<AstAnyParamLike> cast(AstNode *n) {
		return matches(n) ? std::optional<AstAnyParamLike>(AstAnyParamLike(n)) : std::nullopt;
	}
};

struct AstEnum : AstView<AST_ENUM> {
	using AstView::AstView;
	// Grammar: optional range, then AST_ENUM_ITEM children
	size_t num_items() const {
		size_t start = 0;
		if (!node->children.empty() && node->children[0]->type == AST_RANGE) start++;
		return node->children.size() - start;
	}
	auto items_begin() {
		size_t start = 0;
		if (!node->children.empty() && node->children[0]->type == AST_RANGE) start++;
		return node->children.begin() + start;
	}
	auto items_end() { return node->children.end(); }
	static std::optional<AstEnum> cast(AstNode *n) {
		return matches(n) ? std::optional<AstEnum>(AstEnum(n)) : std::nullopt;
	}
};

// Grammar: children[0] = value expr (or AST_NONE if no init); children[1]
// (optional) = AST_RANGE bound on the discriminant width.
struct AstEnumItem : ParamLikeView<AST_ENUM_ITEM> {
	using ParamLikeView::ParamLikeView;
	// The discriminant signedness is taken from the optional AST_RANGE child:
	// `enum logic signed [3:0] { ... }` propagates `is_signed` to each item.
	bool is_signed_via_range() const {
		AstNode *r = range_or_null();
		return r && r->is_signed;
	}
	static std::optional<AstEnumItem> cast(AstNode *n) {
		return matches(n) ? std::optional<AstEnumItem>(AstEnumItem(n)) : std::nullopt;
	}
};

// Constant value
struct AstConstant : AstView<AST_CONSTANT> {
	using AstView::AstView;
	// Leaf node: no children, bits/integer/is_signed carry the value
	static std::optional<AstConstant> cast(AstNode *n) {
		return matches(n) ? std::optional<AstConstant>(AstConstant(n)) : std::nullopt;
	}
	bool asBool() const {
		for (auto &bit : raw()->bits)
			if (bit == RTLIL::State::S1)
				return true;
		return false;
	}
};

// Function and task calls. Variadic children are argument expressions.
using AstFcall = VariadicView<AST_FCALL, Expression>;
using AstTcall = VariadicView<AST_TCALL, Expression>;

// Defparam (defparam lvalue = value)
struct AstDefparam : AstView<AST_DEFPARAM> {
	using AstView::AstView;
	// Grammar: children[0] = lvalue, children[1] = value expr, optional children[2] = range
	AstNode *lvalue() const {
		log_assert(!node->children.empty());
		return node->children[0].get();
	}
	AstNode *value() const {
		log_assert(node->children.size() >= 2);
		return node->children[1].get();
	}
	bool has_range() const { return node->children.size() >= 3; }
	AstNode *range_or_null() const {
		return has_range() ? node->children[2].get() : nullptr;
	}
	static std::optional<AstDefparam> cast(AstNode *n) {
		return matches(n) ? std::optional<AstDefparam>(AstDefparam(n)) : std::nullopt;
	}
};

// Gate-level primitives (and, or, buf, not, nand, nor, xor, xnor, bufif*, notif*, tran).
// Grammar: variadic AST_ARGUMENT children. Distinguished from AST_CELL because
// the parser resolves the cell type only after primitive dispatch in simplify.
using AstPrimitive = VariadicView<AST_PRIMITIVE, ChildConstraint<AST_ARGUMENT>>;

// Memory access nodes synthesized by simplify (post-parser).
//   AST_MEMRD   : children = [addr]                    (read port)
//   AST_MEMWR   : children = [addr, data, en, portid, prio_mask] (write port)
//   AST_MEMINIT : children = [addr, data, en, count]
// Common shape: addr is children[0]. Genrtlil consumes the trailing slots.
template <AstNodeType Tag>
struct AstMemAccess : AstView<Tag> {
	using AstView<Tag>::AstView;
	using AstView<Tag>::node;
	AstNode *addr() const {
		log_assert(!node->children.empty());
		return node->children[0].get();
	}
	ChildSlot<Anything> addr_slot() const { return {node, 0}; }
	template <typename... Args>
	static std::unique_ptr<AstNode> build(const AstSrcLocType &loc, std::unique_ptr<AstNode> addr, Args&&... rest) {
		return AstView<Tag>::make_node(loc, std::move(addr), std::forward<Args>(rest)...);
	}
	static std::optional<AstMemAccess> cast(AstNode *n) {
		return AstView<Tag>::matches(n) ? std::optional<AstMemAccess>(AstMemAccess(n)) : std::nullopt;
	}
};
using AstMemRd   = AstMemAccess<AST_MEMRD>;
using AstMemWr   = AstMemAccess<AST_MEMWR>;
using AstMemInit = AstMemAccess<AST_MEMINIT>;

// MemberContainer: shared base for AST_STRUCT and AST_UNION, whose grammar is
// "ordered list of AST_STRUCT_ITEM (with possibly nested AST_STRUCT/AST_UNION)".
template <AstNodeType Tag>
struct MemberContainerView : AstView<Tag> {
	using AstView<Tag>::AstView;
	using AstView<Tag>::node;
	size_t num_members() const { return node->children.size(); }
	AstNode *member(size_t i) const { return node->children.at(i).get(); }
	auto members_begin() const { return node->children.begin(); }
	auto members_end() const { return node->children.end(); }
	auto members_rbegin() const { return node->children.rbegin(); }
	auto members_rend() const { return node->children.rend(); }
	static std::optional<MemberContainerView> cast(AstNode *n) {
		return AstView<Tag>::matches(n) ? std::optional<MemberContainerView>(MemberContainerView(n)) : std::nullopt;
	}
};

using AstStruct = MemberContainerView<AST_STRUCT>;
using AstUnion  = MemberContainerView<AST_UNION>;

// Tag-agnostic view over either AST_STRUCT or AST_UNION. Used where the
// simplifier treats the two interchangeably (e.g. size_packed_struct walks
// both in the same loop).
struct AstStructLike {
	AstNode *node;
	static bool matches(const AstNode *n) {
		return n && (n->type == AST_STRUCT || n->type == AST_UNION);
	}
	explicit AstStructLike(AstNode *n) : node(n) { log_assert(matches(n)); }
	bool is_union() const { return node->type == AST_UNION; }
	size_t num_members() const { return node->children.size(); }
	AstNode *member(size_t i) const { return node->children.at(i).get(); }
	auto members_begin() const { return node->children.begin(); }
	auto members_end() const { return node->children.end(); }
	auto members_rbegin() const { return node->children.rbegin(); }
	auto members_rend() const { return node->children.rend(); }
	AstNode *raw() const { return node; }
	static std::optional<AstStructLike> cast(AstNode *n) {
		return matches(n) ? std::optional<AstStructLike>(AstStructLike(n)) : std::nullopt;
	}
};

// Grammar: optional packed range or multirange in children[0], with at most one
// trailing AST_RANGE for an (extension) unpacked array. After packing the
// children are cleared and the geometry is recorded on the node itself.
struct AstStructItem : AstView<AST_STRUCT_ITEM> {
	using AstView::AstView;

	bool has_any_children() const { return !node->children.empty(); }
	AstNode *first_child() const { return node->children[0].get(); }

	bool has_packed_range() const {
		return has_any_children() && first_child()->type == AST_RANGE;
	}
	bool has_packed_multirange() const {
		return has_any_children() && first_child()->type == AST_MULTIRANGE;
	}

	// True for the Yosys extension `bit [W-1:0] x [N]` shape: two children,
	// children[0]=AST_RANGE element width, children[1]=AST_RANGE unpacked size.
	bool has_unpacked_range() const {
		return node->children.size() == 2 && node->children[1]->type == AST_RANGE;
	}
	AstNode *unpacked_range() const {
		log_assert(has_unpacked_range());
		return node->children[1].get();
	}

	void clear_range_children() { node->children.clear(); }

	static std::optional<AstStructItem> cast(AstNode *n) {
		return matches(n) ? std::optional<AstStructItem>(AstStructItem(n)) : std::nullopt;
	}
};

// RealValue (floating point literal)
struct AstRealvalue : AstView<AST_REALVALUE> {
	using AstView::AstView;
	// Leaf node: realvalue field holds the double, no children
	static std::optional<AstRealvalue> cast(AstNode *n) {
		return matches(n) ? std::optional<AstRealvalue>(AstRealvalue(n)) : std::nullopt;
	}
};

// ---------------- Unary / binary op templates ----------------

// All unary/binary op types share a trivial shape. A tiny typed view for each
// lets call sites say `AstNeg{node}.operand()` instead of `children[0]`.
template <AstNodeType Tag>
struct AstUnaryOp : AstView<Tag> {
	using AstView<Tag>::AstView;
	using AstView<Tag>::node;
	ChildSlot<Anything> operand() const { return {node, 0}; }
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
	ChildSlot<Anything> lhs() const { return {node, 0}; }
	ChildSlot<Anything> rhs() const { return {node, 1}; }
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
	ChildSlot<Anything> selector_slot() const { return {node, 0}; }
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
	ChildSlot<Anything> selector_slot() const { return {node, 0}; }
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
	ChildSlot<Anything> body_slot() const {
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

// Untagged view over any AST_COND / AST_CONDX / AST_CONDZ. Useful when iterating
// the conditions of an AST_CASE without dispatching on the exact tag.
struct AstAnyCond {
	AstNode *node;
	static bool matches(const AstNode *n) {
		return n && (n->type == AST_COND || n->type == AST_CONDX || n->type == AST_CONDZ);
	}
	explicit AstAnyCond(AstNode *n) : node(n) { log_assert(matches(n)); }
	AstNode *body() const {
		log_assert(!node->children.empty());
		return node->children.back().get();
	}
	bool is_default() const {
		return !node->children.empty() && node->children.front()->type == AST_DEFAULT;
	}
	size_t num_labels() const {
		return node->children.empty() ? 0 : node->children.size() - 1;
	}
	AstNode *label(size_t i) const {
		log_assert(i + 1 < node->children.size());
		return node->children.at(i).get();
	}
	static std::optional<AstAnyCond> cast(AstNode *n) {
		return matches(n) ? std::optional<AstAnyCond>(AstAnyCond(n)) : std::nullopt;
	}
};

struct AstGenIf : AstView<AST_GENIF> {
	using AstView::AstView;
	ChildSlot<Anything> cond()      const { return {node, 0}; }
	ChildSlot<Anything> then_body() const { return {node, 1}; }
	bool has_else_body() const { return node->children.size() > 2; }
	ChildSlot<Anything> else_body() const {
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
	std::optional<ChildSlot<Anything>> bit_select_slot() const {
		return node->children.empty() ? std::nullopt
									: std::optional<ChildSlot<Anything>>(ChildSlot<Anything>{node, 0});
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
	std::optional<ChildSlot<Anything>> expr_slot() const {
		return node->children.empty() ? std::nullopt
									: std::optional<ChildSlot<Anything>>(ChildSlot<Anything>{node, 0});
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
	ChildSlot<Anything> expr_slot() const { return {node, 0}; }
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
	ChildSlot<Anything> celltype_slot() const { return {node, 0}; }
	// Iterate children[1..]; caller dispatches on AST_PARASET vs AST_ARGUMENT.
	auto body_begin() { return node->children.begin() + 1; }
	auto body_end()   { return node->children.end(); }
	static std::optional<AstCell> cast(AstNode *n) {
		return matches(n) ? std::optional<AstCell>(AstCell(n)) : std::nullopt;
	}
};

// Variadic: children are behavioral statements in declaration order.
using AstBlock = VariadicView<AST_BLOCK, BehavioralStatement>;

// Variadic: children are module-level statements in declaration order.
using AstGenBlock = VariadicView<AST_GENBLOCK, ModuleBodyStatement>;

// AST_CONCAT: variadic list of expression children.
using AstConcat = VariadicView<AST_CONCAT, Expression>;

// AST_MULTIRANGE: variadic list of AST_RANGE children (>= 2).
using AstMultirange = VariadicView<AST_MULTIRANGE, ChildConstraint<AST_RANGE>>;

// ----------------------------------------------------------------------------
// Hierarchy-flag propagation rules
//
// `in_param_from_above` and `in_lvalue_from_above` are inherited contexts:
// "this subtree appears inside a parameter-evaluation expression" and "this
// subtree is the LHS of an assignment". The rules below capture which AST
// types *force* their children into one of those contexts regardless of the
// inherited flag — i.e. type-specific knowledge that lives most naturally
// next to the per-type grammar in this file.
//
// AstNode::fixup_hierarchy_flags is now a thin dispatcher around
// hierarchy_flags::apply(); update the rules here, not there.
// ----------------------------------------------------------------------------
namespace hierarchy_flags {

// Whole-subtree parameter contexts: the node and *all* of its children must
// be `in_param=true` regardless of what was inherited. These are the nodes
// whose body is evaluated at elaboration time.
inline bool whole_subtree_is_param(AstNodeType t) {
	switch (t) {
	case AST_PARAMETER:
	case AST_LOCALPARAM:
	case AST_DEFPARAM:
	case AST_PARASET:
	case AST_PREFIX:
		return true;
	default:
		return false;
	}
}

// Index of a single child that must be in_param=true regardless of inherited
// flag, or -1 if none. Captures "header" expressions whose value must be
// statically known: AST_REPLICATE count, AST_WIRE packed range, AST_GENIF
// condition, AST_GENCASE selector, AST_FOR/AST_GENFOR loop condition.
inline int param_forced_child_index(AstNodeType t) {
	switch (t) {
	case AST_REPLICATE:
	case AST_WIRE:
	case AST_GENIF:
	case AST_GENCASE:
		return 0;
	case AST_FOR:
	case AST_GENFOR:
		return 1;
	default:
		return -1;
	}
}

// Index of a child that must be in_lvalue=true regardless of inherited flag.
// Always children[0] for the assignment family (the LHS).
inline int lvalue_forced_child_index(AstNodeType t) {
	switch (t) {
	case AST_ASSIGN:
	case AST_ASSIGN_EQ:
	case AST_ASSIGN_LE:
		return 0;
	default:
		return -1;
	}
}

// Apply one level of flag propagation: set this node's in_param/in_lvalue from
// the inherited *_from_above flags, then push the appropriate flags down into
// children and attributes. Does not recurse on its own — recursion happens
// via set_in_param_flag/set_in_lvalue_flag (or the explicit force_descend
// pass driven by fixup_hierarchy_flags).
inline void apply(AstNode *n, bool force_descend) {
	// --- in_param ---
	if (whole_subtree_is_param(n->type)) {
		n->in_param = true;
		for (auto& child : n->children)
			child->set_in_param_flag(true, force_descend);
	} else {
		n->in_param = n->in_param_from_above;
		for (auto& child : n->children)
			child->set_in_param_flag(n->in_param, force_descend);
		int forced = param_forced_child_index(n->type);
		if (forced >= 0 && (size_t)forced < n->children.size())
			n->children[forced]->set_in_param_flag(true, force_descend);
	}
	// Attributes are always parameter-context: their values feed elaboration.
	for (auto& attr : n->attributes)
		attr.second->set_in_param_flag(true, force_descend);

	// --- in_lvalue ---
	n->in_lvalue = n->in_lvalue_from_above;
	int lv_forced = lvalue_forced_child_index(n->type);
	if (lv_forced >= 0) {
		// ASSIGN family: children[0]=LHS forced true; children[1]=RHS inherits.
		if ((size_t)lv_forced < n->children.size())
			n->children[lv_forced]->set_in_lvalue_flag(true, force_descend);
		size_t rhs_idx = (size_t)lv_forced + 1;
		if (rhs_idx < n->children.size())
			n->children[rhs_idx]->set_in_lvalue_flag(n->in_lvalue, force_descend);
	} else {
		for (auto& child : n->children)
			child->set_in_lvalue_flag(n->in_lvalue, force_descend);
	}
}

} // namespace hierarchy_flags

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
	ChildSlot<Anything> body_slot() const {
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