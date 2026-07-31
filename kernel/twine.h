#ifndef YOSYS_TWINE_H
#define YOSYS_TWINE_H

#include "kernel/yosys_common.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

YOSYS_NAMESPACE_BEGIN

struct Twine;
struct TwinePool;
struct IdString;
struct SrcRef;

struct NullIdString {
	constexpr operator IdString() const;
	constexpr bool operator==(IdString ref) const;
};

struct NullSrcRef {
	constexpr operator SrcRef() const;
	constexpr bool operator==(SrcRef ref) const;
};

struct IdString {
	size_t value;

	static constexpr NullIdString Null{};

	// Publicity tag carried on name handles. Pool nodes store name *content*
	// (no '\' escape); whether a name is public lives in this bit of the
	// handle, never inside the pool. TwinePool strips it on every entry path.
	static constexpr size_t kPublicBit = 1ULL << 62;
	static constexpr size_t kNull      = ~size_t{0};

	constexpr IdString() : value(kNull) {}
	explicit constexpr IdString(size_t val) : value(val) {}

	constexpr bool operator==(const IdString&) const = default;
	constexpr std::strong_ordering operator<=>(const IdString &rhs) const {
		if (auto cmp = (value & ~kPublicBit) <=> (rhs.value & ~kPublicBit); cmp != 0)
			return cmp;
		return (value & kPublicBit) <=> (rhs.value & kPublicBit);
	}

	template <typename... Args>
	constexpr bool in(const Args&... args) const {
		return ((*this == args) || ...);
	}

	// The following is a helper key_compare class. Instead of for example std::set<Cell*>
	// use std::set<Cell*, IdString::compare_ptr_by_name<Cell>> if the order of cells in the
	// set has an influence on the algorithm.

	template<typename T> struct compare_ptr_by_name {
		bool operator()(const T *a, const T *b) const {
			return (a == nullptr || b == nullptr) ? (a < b) : (a->name < b->name);
		}
	};

	// A ref is "empty" when it names nothing at all.
	constexpr bool empty() const { return value == kNull; }

	constexpr bool isPublic() const { return value != kNull && (value & kPublicBit); }

	constexpr IdString untag() const {
		return value == kNull ? *this : IdString(value & ~kPublicBit);
	}
	constexpr IdString tag(bool pub) const {
		return value == kNull ? *this : IdString(pub ? (value | kPublicBit) : (value & ~kPublicBit));
	}

	Hasher hash_into(Hasher h) const { h.hash64(value); return h; }
};

struct SrcRef {
	size_t value;

	static constexpr NullSrcRef Null{};

	static constexpr size_t kNull = ~size_t{0};

	constexpr SrcRef() : value(kNull) {}
	explicit constexpr SrcRef(size_t val) : value(val) {}

	constexpr bool operator==(const SrcRef&) const = default;
	constexpr auto operator<=>(const SrcRef&) const = default;

	constexpr bool empty() const { return value == kNull; }

	Hasher hash_into(Hasher h) const { h.hash64(value); return h; }
};

constexpr NullIdString::operator IdString() const { return IdString(); }
constexpr bool NullIdString::operator==(IdString ref) const { return ref.value == IdString::kNull; }

constexpr NullSrcRef::operator SrcRef() const { return SrcRef(); }
constexpr bool NullSrcRef::operator==(SrcRef ref) const { return ref.value == SrcRef::kNull; }

enum : short {
	// STATIC_TWINE_BEGIN = 0,
#define X(N) IDX_##N,
#include "kernel/constids.inc"
#undef X
	STATIC_TWINE_END
};

struct ID {
// Static ids are name handles: non-'$' constids were '\'-escaped publics,
// so their handles carry the publicity bit baked in at compile time.
#define X(N) static constexpr IdString N = IdString(IDX_##N).tag((#N)[0] != '$');
#include "kernel/constids.inc"
#undef X

	static constexpr IdString lookup(std::string_view name)
	{
#define X(N) \
		if (name == #N) return N;
#include "kernel/constids.inc"
#undef X

		throw "unknown twine id";
	}

	static constexpr const char* static_names[] = {
#define X(N) #N,
#include "kernel/constids.inc"
#undef X
	};

	static constexpr bool is_static(IdString ref) {
		return ref.untag().value < STATIC_TWINE_END;
	}

	static std::string str(IdString ref) {
		IdString idx = ref.untag();
		log_assert(idx.value < STATIC_TWINE_END);
		std::string result = ref.isPublic() ? "\\" : "";
		result += static_names[idx.value];
		return result;
	}

	static std::string unescaped_str(IdString ref) {
		IdString idx = ref.untag();
		log_assert(idx.value < STATIC_TWINE_END);
		return static_names[idx.value];
	}
};

#define ID(id) (ID::id)

struct Twine {
	struct Suffix {
		IdString prefix;
		std::string tail;
		auto operator<=>(const Suffix&) const = default;
	};


	struct AutoSuffix {
		std::string_view prefix;
		std::string tail;
		auto operator<=>(const AutoSuffix&) const = default;
	};

	struct Leaf {
		std::string s;
		auto operator<=>(const Leaf&) const = default;
	};

	std::variant<
				// "leaf", regular deduplicated string
				Leaf,
				// "suffix", deduplicates shared prefixes
				Suffix,
				// transient suffix constructed with NEW_ID and NEW_ID_SUFFIX
				// turned into a regular Suffix when added to a TwinePool
				AutoSuffix> data;

	Twine(Leaf v) : data(std::move(v)) {}
	Twine(Suffix v) : data(std::move(v)) {}
	Twine(AutoSuffix v) : data(std::move(v)) {}

	bool is_leaf() const { return std::holds_alternative<Leaf>(data); }
	bool is_suffix() const { return std::holds_alternative<Suffix>(data); }
};

struct TwineNode {
	std::variant<std::monostate, Twine::Leaf, Twine::Suffix> data;

	TwineNode() = default;
	TwineNode(Twine::Leaf v) : data(std::move(v)) {}
	TwineNode(Twine::Suffix v) : data(std::move(v)) {}

	bool is_dead() const { return std::holds_alternative<std::monostate>(data); }
	bool is_leaf() const { return std::holds_alternative<Twine::Leaf>(data); }
	bool is_suffix() const { return std::holds_alternative<Twine::Suffix>(data); }
	const std::string &leaf() const { return std::get<Twine::Leaf>(data).s; }
	const Twine::Suffix &suffix() const { return std::get<Twine::Suffix>(data); }
};

struct StaticTwines {
	static constexpr size_t count = STATIC_TWINE_END;

	static void init();
	static const TwineNode &node(size_t idx) { return nodes_[idx]; }
	static bool ready() { return nodes_.size() == count; }

private:
	static std::vector<TwineNode> nodes_;
};

void twine_prepopulate();

extern int64_t twine_gc_ns;
extern int twine_gc_count;

inline std::pair<std::string, bool> twine_unescape(std::string s) {
	bool is_public = !(s.size() > 1 && s[0] == '$');
	if (s.size() > 1 && s[0] == '\\')
		s.erase(0, 1);
	return {std::move(s), is_public};
}

template<typename P, typename Node, typename Ref>
concept HasStaticNodes = P::kStaticCount == 0 || requires (size_t idx) {
	{ P::static_node(idx) } -> std::same_as<const Node &>;
};

template<typename P, typename Node, typename Ref>
concept HashConsPolicy = HasStaticNodes<P, Node, Ref>
	&& requires (const Node &cn, Node &n, Ref ref) {
		{ std::integral_constant<size_t, P::kStaticCount>{} };
		{ P::untag(ref) } -> std::same_as<Ref>;
		{ P::canonicalize(n) } -> std::same_as<void>;
		{ P::hash_node(cn) } -> std::same_as<size_t>;
		{ P::for_each_child(cn, [](Ref) {}) } -> std::same_as<void>;
		{ P::check_ready() } -> std::same_as<void>;
		{ cn.is_dead() } -> std::same_as<bool>;
	};

template<typename Derived, typename Node, typename Ref>
struct HashConsPool {
	// Not noexcept on purpose: see __cache_default in libstdc++ bits/hashtable.h
	struct NodeHash {
		using is_transparent = void;

		const Derived* pool = nullptr;

		size_t operator()(const Node& n) const { return Derived::hash_node(n); }
		size_t operator()(Ref ref) const { return Derived::hash_node((*pool)[ref]); }
	};

	struct NodeEq {
		using is_transparent = void;

		const Derived* pool = nullptr;

		bool operator()(Ref a, Ref b) const noexcept { return (*pool)[a].data == (*pool)[b].data; }
		bool operator()(Ref a, const Node& b) const noexcept { return (*pool)[a].data == b.data; }
		bool operator()(const Node& a, Ref b) const noexcept { return a.data == (*pool)[b].data; }
	};

	using Index = std::unordered_set<Ref, NodeHash, NodeEq>;

	std::deque<Node> backing;

protected:
	Index index;
	// Indices of monostate, kept sorted
	std::vector<size_t> free_list;

public:
	Derived* self() { return static_cast<Derived*>(this); }
	const Derived* self() const { return static_cast<const Derived*>(this); }

	Index fresh_index() { return Index(0, NodeHash{self()}, NodeEq{self()}); }

	HashConsPool() : index(fresh_index()) { rebuild_index(); }
	HashConsPool(const HashConsPool& other) : backing(other.backing), index(fresh_index()) {
		rebuild_index();
	}
	HashConsPool(HashConsPool&& other) : backing(std::move(other.backing)), index(fresh_index()) {
		other.reset();
		rebuild_index();
	}
	HashConsPool& operator=(const HashConsPool& other) {
		if (this != &other) {
			backing = other.backing;
			index = fresh_index();
			rebuild_index();
		}
		return *this;
	}
	HashConsPool& operator=(HashConsPool&& other) {
		if (this != &other) {
			backing = std::move(other.backing);
			other.reset();
			index = fresh_index();
			rebuild_index();
		}
		return *this;
	}

	void reset() {
		backing.clear();
		free_list.clear();
		index = fresh_index();
		rebuild_index();
	}

	static bool is_static(Ref ref) {
		if constexpr (Derived::kStaticCount == 0)
			return false;
		else
			return ref.value < Derived::kStaticCount;
	}

	const Node& operator[] (Ref ref) const {
		Ref idx = Derived::untag(ref);
		if constexpr (Derived::kStaticCount != 0) {
			if (is_static(idx))
				return Derived::static_node(idx.value);
		}
		return backing[idx.value - Derived::kStaticCount];
	}

	static void check_ready() {}

	void rebuild_index() {
		Derived::check_ready();
		for (size_t idx = 0; idx < Derived::kStaticCount; idx++)
			index.insert(Ref(idx));
		free_list.clear();
		for (size_t idx = 0; idx < backing.size(); ++idx) {
			if (backing[idx].is_dead())
				free_list.push_back(idx);
			else
				index.insert(Ref(Derived::kStaticCount + idx));
		}
		std::sort(free_list.begin(), free_list.end(), std::greater<size_t>());
	}

	Ref find(Node t) const {
		Derived::canonicalize(t);
		if (auto it = index.find(t); it != index.end())
			return *it;
		return Ref();
	}

	Ref add_inner(Node t) {
		Derived::canonicalize(t);

		if (auto it = index.find(t); it != index.end()) {
			if (yosys_xtrace) {
				std::cout << "#X# add_inner found ";
				self()->dump(*it);
				std::cout << "\n";
				std::cout << "#X# as integer " << it->value << "\n";
			}
			return *it;
		}

		Ref ref;
		if (!free_list.empty()) {
			size_t idx = free_list.back();
			free_list.pop_back();
			backing[idx] = std::move(t);
			ref = Ref(Derived::kStaticCount + idx);
		} else {
			ref = Ref(Derived::kStaticCount + backing.size());
			backing.push_back(std::move(t));
		}
		index.insert(ref);
		if (yosys_xtrace) {
			std::cout << "#X# add_inner added ";
			self()->dump(ref);
			std::cout << "\n";
			std::cout << "#X# as integer " << ref.value << "\n";
		}
		return ref;
	}

	size_t size() const { return backing.size() - free_list.size(); }

	template<typename Roots>
	size_t gc(const Roots& roots) {
		pool<Ref> live;
		for (Ref ref : roots)
			mark_live(ref, live);
		size_t erased = 0;
		for (size_t idx = 0; idx < backing.size(); ++idx) {
			if (backing[idx].is_dead())
				continue;
			if (!live.count(Ref(Derived::kStaticCount + idx))) {
				index.erase(Ref(Derived::kStaticCount + idx));
				free_list.push_back(idx);
				backing[idx] = Node{};
				erased++;
			}
		}
		// TODO something like YOSYS_SORT_ID_FREE_LIST to make it optional?
		std::sort(free_list.begin(), free_list.end(), std::greater<size_t>());
		return erased;
	}

	void mark_live(Ref ref, pool<Ref>& live) const {
		ref = Derived::untag(ref);
		if (ref == Ref() || is_static(ref) || !live.insert(ref).second)
			return;
		Derived::for_each_child((*this)[ref], [&](Ref child) { mark_live(child, live); });
	}
};

struct TwinePool : HashConsPool<TwinePool, TwineNode, IdString> {
	static constexpr size_t kStaticCount = StaticTwines::count;

	static const TwineNode& static_node(size_t idx) { return StaticTwines::node(idx); }
	static IdString untag(IdString ref) { return ref.untag(); }

	static void check_ready() { log_assert(StaticTwines::ready()); }

	static void canonicalize(TwineNode& t) {
		if (auto *sfx = std::get_if<Twine::Suffix>(&t.data))
			sfx->prefix = sfx->prefix.untag();
	}

	static size_t hash_node(const TwineNode& t);

	template<typename F>
	static void for_each_child(const TwineNode& t, F&& f) {
		if (t.is_suffix())
			f(t.suffix().prefix);
	}

	void dump(IdString ref, std::ostream& os = std::cout) const {
		const TwineNode& twine = (*this)[ref];
		std::visit([&](const auto& val) {
			using T = std::decay_t<decltype(val)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
				os << "Dead()";
			} else if constexpr (std::is_same_v<T, Twine::Leaf>) {
				os << "Leaf(\"" << val.s << "\")";
			} else if constexpr (std::is_same_v<T, Twine::Suffix>) {
				os << "Suffix(prefix: ";
				dump(val.prefix, os);
				os << ", tail: \"" << val.tail << "\")";
			}
		}, twine.data);
		if (ref.isPublic())
			os << " pub";
	}
	void print(IdString ref, std::ostream& os = std::cout) const {
		if (ref == IdString::Null)
			return;
		if (ref.isPublic())
			os << '\\';
		std::visit([&](const auto& val) {
			using T = std::decay_t<decltype(val)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
			} else if constexpr (std::is_same_v<T, Twine::Leaf>) {
				os << val.s;
			} else if constexpr (std::is_same_v<T, Twine::Suffix>) {
				print(val.prefix, os);
				os << val.tail;
			}
		}, (*this)[ref].data);
	}
	void append_str(IdString ref, std::string& out) const {
		if (ref == IdString::Null)
			return;
		if (ref.isPublic())
			out += '\\';
		std::visit([&](const auto& val) {
			using T = std::decay_t<decltype(val)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
			} else if constexpr (std::is_same_v<T, Twine::Leaf>) {
				out += val.s;
			} else if constexpr (std::is_same_v<T, Twine::Suffix>) {
				append_str(val.prefix, out);
				out += val.tail;
			}
		}, (*this)[ref].data);
	}
	// Publicity bit provides escaping
	std::string str(IdString ref) const {
		std::string out;
		append_str(ref, out);
		return out;
	}

	// Publicity bit ignored
	std::string unescaped_str(IdString ref) const {
		return str(ref.untag());
	}

	// Avoid. Only finds leaves. Parameter is expected to be escaped.
	// For compatibility only
	IdString find(const std::string &name) const {
		bool is_public = !name.empty() && name[0] == '\\';
		return find(Twine::Leaf{is_public ? name.substr(1) : name}).tag(is_public);
	}

	IdString find(Twine t) const {
		if (auto *ap = std::get_if<Twine::AutoSuffix>(&t.data)) {
			IdString prefix = HashConsPool::find(Twine::Leaf{std::string(ap->prefix)});
			if (prefix == IdString::Null)
				return IdString::Null;
			t = Twine::Suffix{prefix, std::move(ap->tail)};
		}
		bool is_public = inherits_publicity(t);
		return HashConsPool::find(to_node(std::move(t))).tag(is_public);
	}

	IdString add(Twine t) {
		if (auto *ap = std::get_if<Twine::AutoSuffix>(&t.data)) {
			IdString prefix = add_inner(Twine::Leaf{std::string(ap->prefix)});
			t = Twine::Suffix{prefix, std::move(ap->tail)};
		}
		bool is_public = inherits_publicity(t);
		return add_inner(to_node(std::move(t))).tag(is_public);
	}

	IdString add(std::string s) {
		if (s.empty())
			return IdString::Null;
		auto [content, is_public] = twine_unescape(std::move(s));
		return add_inner(Twine::Leaf{std::move(content)}).tag(is_public);
	}

	// Raw, untagged interning for content that is not a name and must not be
	// run through the publicity rules: source locations and their prefixes.
	IdString add_src_leaf(std::string content) {
		return add_inner(Twine::Leaf{std::move(content)});
	}
	IdString add_src_suffix(IdString prefix, std::string tail) {
		return add_inner(Twine::Suffix{prefix, std::move(tail)});
	}

	IdString copy_from(const TwinePool& src, IdString ref) {
		if (ref == IdString::Null)
			return ref;

		bool is_public = ref.isPublic();
		IdString untagged = ref.untag();
		if (ID::is_static(untagged))
			return ref;
		const TwineNode& t = src[untagged];
		if (t.is_leaf())
			return (add(Twine::Leaf{t.leaf()})).tag(is_public);
		if (t.is_suffix())
			return (add(Twine::Suffix{copy_from(src, t.suffix().prefix), t.suffix().tail})).tag(is_public);
		return IdString::Null;
	}

	void dump(std::ostream& os = std::cout) const {
		os << "--- TwinePool Dump (" << backing.size() << " nodes) ---\n";
		for (size_t idx = 0; idx < backing.size(); ++idx) {
			IdString ref(kStaticCount + idx);
			os << ref.value << " -> ";
			dump(ref, os);
			os << '\n';
		}
		os << "--------------------------------\n";
	}

	bool shares_index_space_with(const TwinePool &other) const {
		return backing.size() == other.backing.size() && free_list == other.free_list;
	}

private:
	friend struct SrcPool;

	static bool inherits_publicity(const Twine &t) {
		auto *sfx = std::get_if<Twine::Suffix>(&t.data);
		return sfx != nullptr && sfx->prefix.isPublic();
	}

	static TwineNode to_node(Twine t) {
		if (auto *leaf = std::get_if<Twine::Leaf>(&t.data))
			return TwineNode{std::move(*leaf)};
		return TwineNode{std::move(std::get<Twine::Suffix>(t.data))};
	}

	using HashConsPool::add_inner;
	using HashConsPool::gc;
};

struct TwineSegments {
	static constexpr size_t kInlineDepth = 8;

	TwineSegments(const TwinePool &pool, IdString ref)
	{
		if (ref == IdString::Null)
			return;
		for (IdString cur = ref.untag(); ;) {
			const TwineNode &t = pool[cur];
			if (t.is_suffix()) {
				push(t.suffix().tail);
				cur = t.suffix().prefix.untag();
				continue;
			}
			if (t.is_leaf())
				push(t.leaf());
			break;
		}
		if (ref.isPublic())
			push("\\");
		std::reverse(segs(), segs() + count_);
	}

	std::string_view peek()
	{
		while (pos_ < count_) {
			std::string_view seg = segs()[pos_];
			if (off_ < seg.size())
				return seg.substr(off_);
			pos_++;
			off_ = 0;
		}
		return {};
	}

	void advance(size_t n) { off_ += n; }

	size_t total_size() const
	{
		size_t total = 0;
		for (size_t i = 0; i < count_; i++)
			total += segs()[i].size();
		return total;
	}

private:
	void push(std::string_view seg)
	{
		if (spill_.empty() && count_ < kInlineDepth) {
			inline_[count_++] = seg;
			return;
		}
		if (spill_.empty())
			spill_.assign(inline_, inline_ + count_);
		spill_.push_back(seg);
		count_++;
	}

	std::string_view *segs() { return spill_.empty() ? inline_ : spill_.data(); }
	const std::string_view *segs() const { return spill_.empty() ? inline_ : spill_.data(); }

	std::string_view inline_[kInlineDepth];
	std::vector<std::string_view> spill_;
	size_t count_ = 0;
	size_t pos_ = 0;
	size_t off_ = 0;
};

inline int twine_compare_views(std::string_view a, std::string_view b)
{
	size_t n = std::min(a.size(), b.size());
	if (int diff = n ? std::memcmp(a.data(), b.data(), n) : 0; diff != 0)
		return diff;
	return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}

inline size_t twine_size(const TwinePool &pool, IdString ref)
{
	return TwineSegments(pool, ref).total_size();
}

inline bool twine_begins_with(const TwinePool &pool, IdString ref, std::string_view prefix)
{
	TwineSegments segs(pool, ref);
	while (!prefix.empty()) {
		std::string_view seg = segs.peek();
		if (seg.empty())
			return false;
		size_t n = std::min(seg.size(), prefix.size());
		if (std::memcmp(seg.data(), prefix.data(), n) != 0)
			return false;
		segs.advance(n);
		prefix.remove_prefix(n);
	}
	return true;
}

inline int twine_compare_by_name(const TwinePool &pool, IdString a, IdString b)
{
	if (a == b)
		return 0;
	if (a == IdString::Null)
		return -1;
	if (b == IdString::Null)
		return 1;

	if (a.isPublic() == b.isPublic()) {
		const TwineNode &ta = pool[a.untag()];
		const TwineNode &tb = pool[b.untag()];
		if (ta.is_leaf() && tb.is_leaf())
			return twine_compare_views(ta.leaf(), tb.leaf());
		if (ta.is_suffix() && tb.is_suffix() && ta.suffix().prefix == tb.suffix().prefix)
			return twine_compare_views(ta.suffix().tail, tb.suffix().tail);
	}

	TwineSegments sa(pool, a), sb(pool, b);
	while (true) {
		std::string_view x = sa.peek(), y = sb.peek();
		if (x.empty() || y.empty())
			return x.empty() ? (y.empty() ? 0 : -1) : 1;
		size_t n = std::min(x.size(), y.size());
		if (int diff = std::memcmp(x.data(), y.data(), n); diff != 0)
			return diff;
		sa.advance(n);
		sb.advance(n);
	}
}

inline size_t TwinePool::hash_node(const TwineNode& t) {
	Hasher h;

	std::visit([&h](const auto& val) {
		using T = std::decay_t<decltype(val)>;
		if constexpr (std::is_same_v<T, Twine::Leaf>) {
			h.eat(val.s);
		} else if constexpr (std::is_same_v<T, Twine::Suffix>) {
			h.eat(val.prefix);
			h.eat(val.tail);
		}
	}, t.data);

	return h.yield();
}

struct Src {
	std::vector<IdString> data;

	bool is_dead() const { return data.empty(); }
	const std::vector<IdString> &members() const { return data; }
};

struct SrcPool : HashConsPool<SrcPool, Src, SrcRef> {
	static constexpr size_t kStaticCount = 0;

	TwinePool *twines = nullptr;

	explicit SrcPool(TwinePool *twines) : twines(twines) {}

	SrcPool(const SrcPool &) = delete;
	SrcPool(SrcPool &&) = delete;
	SrcPool &operator=(const SrcPool &) = delete;
	SrcPool &operator=(SrcPool &&) = delete;

	// Src nodes hold IdStrings into `other.twines`, so they are only meaningful
	// here once this pool's twines are a verbatim copy of the source's.
	void clone_from(const SrcPool &other) {
		log_assert(twines->shares_index_space_with(*other.twines));
		HashConsPool::operator=(static_cast<const HashConsPool &>(other));
	}

	void clear() { reset(); }

	static SrcRef untag(SrcRef ref) { return ref; }
	static void canonicalize(Src&) {}

	static size_t hash_node(const Src &n) {
		Hasher h;
		for (IdString member : n.data)
			h.eat(member);
		return h.yield();
	}

	template<typename F>
	static void for_each_child(const Src&, F&&) {}

	SrcRef add(const std::string &location) {
		return intern({twines->add_src_leaf(location)});
	}

	SrcRef add(const std::string &file, const std::string &tail) {
		IdString prefix = twines->add_src_leaf(file);
		return intern({twines->add_src_suffix(prefix, tail)});
	}

	SrcRef merge(std::span<const SrcRef> refs) {
		std::vector<IdString> members;
		for (SrcRef ref : refs)
			if (ref != SrcRef::Null)
				members.insert(members.end(), (*this)[ref].data.begin(), (*this)[ref].data.end());
		return intern(std::move(members));
	}

	SrcRef merge(SrcRef a, SrcRef b) {
		SrcRef both[] = {a, b};
		return merge(std::span<const SrcRef>{both});
	}

	SrcRef adopt(std::span<const IdString> members) {
		for (IdString member : members)
			log_assert(!member.isPublic());
		return intern(std::vector<IdString>(members.begin(), members.end()));
	}

	SrcRef copy_from(const SrcPool &other, SrcRef ref) {
		if (ref == SrcRef::Null)
			return ref;
		std::vector<IdString> members;
		members.reserve(other[ref].data.size());
		for (IdString member : other[ref].data)
			members.push_back(twines->copy_from(*other.twines, member));
		return intern(std::move(members));
	}

	void append_str(SrcRef ref, std::string &out) const {
		if (ref == SrcRef::Null)
			return;
		bool first = true;
		for (IdString member : (*this)[ref].data) {
			if (!first)
				out += '|';
			first = false;
			twines->append_str(member, out);
		}
	}

	std::string str(SrcRef ref) const {
		std::string out;
		append_str(ref, out);
		return out;
	}

	void leaves(SrcRef ref, pool<std::string> &out) const {
		if (ref == SrcRef::Null)
			return;
		for (IdString member : (*this)[ref].data)
			out.insert(twines->str(member));
	}

	void dump(SrcRef ref, std::ostream &os = std::cout) const {
		os << "Set[";
		bool first = true;
		for (IdString member : (*this)[ref].data) {
			if (!first)
				os << ", ";
			first = false;
			os << "@" << member.value;
		}
		os << "]";
	}

	size_t gc_with_twines(const pool<SrcRef> &live_srcs, pool<IdString> &live_twines) {
		for (SrcRef ref : live_srcs)
			for (IdString member : (*this)[ref].data)
				live_twines.insert(member);
		return twines->gc(live_twines) + gc(live_srcs);
	}

	void dump(std::ostream &os = std::cout) const {
		os << "--- SrcPool Dump (" << backing.size() << " nodes) ---\n";
		for (size_t idx = 0; idx < backing.size(); ++idx) {
			os << idx << " -> ";
			dump(SrcRef(idx), os);
			os << '\n';
		}
		os << "--------------------------------\n";
	}

private:
	SrcRef intern(std::vector<IdString> members) {
		std::sort(members.begin(), members.end());
		members.erase(std::unique(members.begin(), members.end()), members.end());
		if (members.empty())
			return SrcRef::Null;
		return add_inner(Src{std::move(members)});
	}

	using HashConsPool::add_inner;
	using HashConsPool::find;
	using HashConsPool::gc;
};

static_assert(HashConsPolicy<TwinePool, TwineNode, IdString>);
static_assert(HashConsPolicy<SrcPool, Src, SrcRef>);

struct DeepTwineHash {
	using is_transparent = void;

	const TwinePool* pool = nullptr;

	struct Stream {
		Hasher h;
		uint64_t buf = 0;
		int fill = 0;

		void push(std::string_view sv) {
			for (char c : sv) {
				buf |= uint64_t(static_cast<unsigned char>(c)) << (8 * fill);
				if (++fill == 8) {
					h.hash64(buf);
					buf = 0;
					fill = 0;
				}
			}
		}

		size_t finish() {
			h.hash64(buf);
			return h.yield();
		}
	};

	void combine(Stream& s, IdString t) const {
		if (t == IdString::Null)
			return;
		const TwineNode& n = (*pool)[t];
		if (n.is_dead()) return;

		if (n.is_leaf()) {
			s.push(n.leaf());
		} else if (n.is_suffix()) {
			combine(s, n.suffix().prefix);
			s.push(n.suffix().tail);
		}
	}

	size_t operator()(std::string_view sv) const {
		Stream s;
		s.push(sv);
		return s.finish();
	}

	size_t operator()(IdString t) const {
		Stream s;
		combine(s, t);
		return s.finish();
	}
};

struct DeepTwineEq {
	using is_transparent = void;

	const TwinePool* pool = nullptr;

	// Recursively consumes the string_view to check for deep equality
	bool consume(IdString t, std::string_view& sv) const noexcept {
		if (t == IdString::Null)
			return true;
		const TwineNode& n = (*pool)[t];
		if (n.is_dead()) return true;

		if (n.is_leaf()) {
			if (!sv.starts_with(n.leaf())) return false;
			sv.remove_prefix(n.leaf().size());
			return true;
		} else if (n.is_suffix()) {
			if (!consume(n.suffix().prefix, sv)) return false;
			if (!sv.starts_with(n.suffix().tail)) return false;
			sv.remove_prefix(n.suffix().tail.size());
			return true;
		}
		return false;
	}

	bool operator()(IdString t, std::string_view sv) const noexcept {
		return consume(t, sv) && sv.empty();
	}

	bool operator()(std::string_view sv, IdString t) const noexcept {
		return (*this)(t, sv);
	}

	// Required by unordered_set to handle hash collisions between two IdStrings.
	bool operator()(IdString a, IdString b) const {
		if (a == b) return true; // Index or structural equality shortcut
		std::string fb = pool->unescaped_str(b);
		return (*this)(a, std::string_view(fb));
	}

};

// A content-addressed view of a TwinePool. Building one is O(pool), so hoist it
// out of loops, but it is a snapshot: nothing interned after construction is
// visible, and a gc underneath a live TwineSearch resolves stale.
struct TwineSearch {
	const TwinePool* pool;
	std::unordered_set<IdString, DeepTwineHash, DeepTwineEq> index;

	TwineSearch(const TwinePool* pool) : pool(pool), index(0, DeepTwineHash{pool}, DeepTwineEq{pool}) {
		for (size_t idx = 0; idx < STATIC_TWINE_END; idx++)
			index.insert(IdString(idx));
		for (size_t idx = 0; idx < pool->backing.size(); ++idx) {
			if (pool->backing[idx].is_dead())
				continue;
			index.insert(IdString(STATIC_TWINE_END + idx));
		}
	}

	void insert(IdString ref) {
		index.insert(ref.untag());
	}

	// Escaped-name aware. Resolves both statics and locals by content.
	IdString find(std::string_view sv) const {
		bool is_public = !sv.empty() && sv[0] == '\\';
		if (is_public)
			sv.remove_prefix(1);
		if (auto it = index.find(sv); it != index.end()) {
			return (*it).tag(is_public);
		}
		return IdString::Null;
	}
};

YOSYS_NAMESPACE_END

#endif
