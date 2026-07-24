/* -*- c++ -*-
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
 */

#ifndef RTLIL_H
#define RTLIL_H

#include "kernel/yosys_common.h"
#include "kernel/yosys.h"
#include "kernel/twine.h"

#include <deque>
#include <string_view>
#include <unordered_map>

YOSYS_NAMESPACE_BEGIN

namespace RTLIL
{
	enum State : unsigned char {
		S0 = 0,
		S1 = 1,
		Sx = 2, // undefined value or conflict
		Sz = 3, // high-impedance / not-connected
		Sa = 4, // don't care (used only in cases)
		Sm = 5  // marker (used internally by some passes)
	};

	enum SyncType : unsigned char {
		ST0 = 0, // level sensitive: 0
		ST1 = 1, // level sensitive: 1
		STp = 2, // edge sensitive: posedge
		STn = 3, // edge sensitive: negedge
		STe = 4, // edge sensitive: both edges
		STa = 5, // always active
		STg = 6, // global clock
		STi = 7  // init
	};

	// Semantic metadata - how can this constant be interpreted?
	// Values may be generally non-exclusive
	enum ConstFlags : unsigned char {
		CONST_FLAG_NONE    = 0,
		CONST_FLAG_STRING  = 1,
		CONST_FLAG_SIGNED  = 2,  // only used for parameters
		CONST_FLAG_REAL    = 4,  // only used for parameters
		CONST_FLAG_UNSIZED = 8,  // only used for parameters
	};

	enum SelectPartials : unsigned char {
		SELECT_ALL = 0,          // include partial modules
		SELECT_WHOLE_ONLY = 1,   // ignore partial modules
		SELECT_WHOLE_WARN = 2,   // call log_warning on partial module
		SELECT_WHOLE_ERR = 3,    // call log_error on partial module
		SELECT_WHOLE_CMDERR = 4  // call log_cmd_error on partial module
	};

	enum SelectBoxes : unsigned char {
		SB_ALL = 0,            // include boxed modules
		SB_WARN = 1,           // helper for log_warning (not for direct use)
		SB_ERR = 2,            // helper for log_error (not for direct use)
		SB_CMDERR = 3,         // helper for log_cmd_error (not for direct use)
		SB_UNBOXED_ONLY = 4,   // ignore boxed modules
		SB_UNBOXED_WARN = 5,   // call log_warning on boxed module
		SB_UNBOXED_ERR = 6,    // call log_error on boxed module
		SB_UNBOXED_CMDERR = 7, // call log_cmd_error on boxed module
		SB_INCL_WB = 8,        // helper for white boxes (not for direct use)
		SB_EXCL_BB_ONLY = 12,  // ignore black boxes, but not white boxes
		SB_EXCL_BB_WARN = 13,  // call log_warning on black boxed module
		SB_EXCL_BB_ERR = 14,   // call log_error on black boxed module
		SB_EXCL_BB_CMDERR = 15 // call log_cmd_error on black boxed module
	};

	enum PortDir : unsigned char  {
		PD_UNKNOWN = 0,
		PD_INPUT = 1,
		PD_OUTPUT = 2,
		PD_INOUT = 3
	};

	// Maximum width in bits of RTLIL::Wire or RTLIL::Const
	constexpr int WIDTH_LIMIT = 1 << 30;

	struct Const;
	struct AttrObject;
	struct NamedObject;
	struct Selection;
	struct Monitor;
	struct Design;
	struct Module;
	struct Wire;
	struct Memory;
	struct Cell;
	struct SigChunk;
	struct SigBit;
	struct SigSpecIterator;
	struct SigSpecConstIterator;
	struct SigSpec;
	struct CaseRule;
	struct SwitchRule;
	struct MemWriteAction;
	struct SyncAction;
	struct SyncRule;
	struct Process;
	struct Binding;
	struct ObjMeta;

	typedef std::pair<SigSpec, SigSpec> SigSig;
};

struct SigMap;


// TODO clean up?
extern int64_t twine_gc_ns;
extern int twine_gc_count;

namespace RTLIL { using YOSYS_NAMESPACE_PREFIX ID; }

namespace RTLIL {
	// Attribute and parameter names are IdStrings into the owning Design's
	// pool, so a verbatim dict copy across designs would leave dangling
	// handles. Rebuilds the keys through the destination pool when the
	// designs differ; a plain copy otherwise.
	void copy_attr_dict(dict<IdString, RTLIL::Const> &dst,
			const dict<IdString, RTLIL::Const> &src,
			const RTLIL::Design *src_design, RTLIL::Design *dst_design);

	extern dict<std::string, std::string> constpad;

	[[deprecated("use StaticCellTypes::categories.is_ff() instead")]]
	const pool<IdString> &builtin_ff_cell_types();

	static inline std::string escape_id(const std::string &str) {
		if (str.size() > 0 && str[0] != '\\' && str[0] != '$')
			return "\\" + str;
		return str;
	}

	static inline std::string unescape_id(const std::string &str) {
		if (str.size() < 2)
			return str;
		if (str[0] != '\\')
			return str;
		if (str[1] == '$' || str[1] == '\\')
			return str;
		if (str[1] >= '0' && str[1] <= '9')
			return str;
		return str.substr(1);
	}

	template <typename T> struct sort_by_name {
		bool operator()(T *a, T *b) const {
			return a->name < b->name;
		}
	};

	// Key comparator for containers of RTLIL object pointers whose iteration
	// order must not depend on allocation addresses.
	template<typename T> struct compare_ptr_by_name {
		bool operator()(const T *a, const T *b) const {
			return (a == nullptr || b == nullptr) ? (a < b) : (a->name.ref() < b->name.ref());
		}
	};

	template <typename T> struct sort_by_name_str {
		bool operator()(T *a, T *b) const {
			return a->name.lt_by_name(b->name);
		}
	};

	struct sort_by_twine_str_expensive {
		const TwinePool& pool;
		explicit sort_by_twine_str_expensive(const TwinePool& pool)
			: pool(pool) {}
		bool operator()(IdString a, IdString b) const {
			bool a_public = a.isPublic();
			bool b_public = b.isPublic();
			std::string a_str = pool.str(a);
			std::string b_str = pool.str(b);
			return std::tie(a_str, a_public) < std::tie(b_str, b_public);
		}
	};

	static inline std::string encode_filename(const std::string &filename)
	{
		std::stringstream val;
		if (!std::any_of(filename.begin(), filename.end(), [](char c) {
			return static_cast<unsigned char>(c) < 33 || static_cast<unsigned char>(c) > 126;
		})) return filename;
		for (unsigned char const c : filename) {
			if (c < 33 || c > 126)
				val << stringf("$%02x", c);
			else
				val << c;
		}
		return val.str();
	}

	// see calc.cc for the implementation of this functions
	RTLIL::Const const_not         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_and         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_or          (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_xor         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_xnor        (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

	RTLIL::Const const_reduce_and  (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_reduce_or   (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_reduce_xor  (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_reduce_xnor (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_reduce_bool (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

	RTLIL::Const const_logic_not   (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_logic_and   (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_logic_or    (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

	RTLIL::Const const_shl         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_shr         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_sshl        (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_sshr        (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_shift       (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_shiftx      (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

	RTLIL::Const const_lt          (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_le          (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_eq          (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_ne          (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_eqx         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_nex         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_ge          (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_gt          (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

	RTLIL::Const const_add         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_sub         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_mul         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_div         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_divfloor    (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_modfloor    (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_mod         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_pow         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

	RTLIL::Const const_pos         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_buf         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);
	RTLIL::Const const_neg         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, bool signed1, bool signed2, int result_len);

	RTLIL::Const const_mux         (const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3);
	RTLIL::Const const_pmux        (const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3);
	RTLIL::Const const_bmux        (const RTLIL::Const &arg1, const RTLIL::Const &arg2);
	RTLIL::Const const_demux       (const RTLIL::Const &arg1, const RTLIL::Const &arg2);

	RTLIL::Const const_bweqx       (const RTLIL::Const &arg1, const RTLIL::Const &arg2);
	RTLIL::Const const_bwmux       (const RTLIL::Const &arg1, const RTLIL::Const &arg2, const RTLIL::Const &arg3);


	// This iterator-range-pair is used for Design::modules(), Module::wires() and Module::cells().
	// It maintains a reference counter that is used to make sure that the container is not modified while being iterated over.

	template<typename T, typename Key = IdString>
	struct ObjIterator {
		using iterator_category = std::forward_iterator_tag;
		using value_type = T;
		using difference_type = ptrdiff_t;
		using pointer = T*;
		using reference = T&;
		typename dict<Key, T>::iterator it;
		dict<Key, T> *list_p;
		int *refcount_p;

		ObjIterator() : list_p(nullptr), refcount_p(nullptr) {
		}

		ObjIterator(decltype(list_p) list_p, int *refcount_p) : list_p(list_p), refcount_p(refcount_p) {
			if (list_p->empty()) {
				this->list_p = nullptr;
				this->refcount_p = nullptr;
			} else {
				it = list_p->begin();
				(*refcount_p)++;
			}
		}

		ObjIterator(const RTLIL::ObjIterator<T, Key> &other) {
			it = other.it;
			list_p = other.list_p;
			refcount_p = other.refcount_p;
			if (refcount_p)
				(*refcount_p)++;
		}

		ObjIterator &operator=(const RTLIL::ObjIterator<T, Key> &other) {
			if (refcount_p)
				(*refcount_p)--;
			it = other.it;
			list_p = other.list_p;
			refcount_p = other.refcount_p;
			if (refcount_p)
				(*refcount_p)++;
			return *this;
		}

		~ObjIterator() {
			if (refcount_p)
				(*refcount_p)--;
		}

		inline T operator*() const {
			log_assert(list_p != nullptr);
			return it->second;
		}

		inline bool operator!=(const RTLIL::ObjIterator<T, Key> &other) const {
			if (list_p == nullptr || other.list_p == nullptr)
				return list_p != other.list_p;
			return it != other.it;
		}


		inline bool operator==(const RTLIL::ObjIterator<T, Key> &other) const {
			return !(*this != other);
		}

		inline ObjIterator<T, Key>& operator++() {
			log_assert(list_p != nullptr);
			if (++it == list_p->end()) {
				(*refcount_p)--;
				list_p = nullptr;
				refcount_p = nullptr;
			}
			return *this;
		}

		inline ObjIterator<T, Key>& operator+=(int amt) {
			log_assert(list_p != nullptr);
			it += amt;
			if (it == list_p->end()) {
				(*refcount_p)--;
				list_p = nullptr;
				refcount_p = nullptr;
			}
			return *this;
		}

		inline ObjIterator<T, Key> operator+(int amt) {
			log_assert(list_p != nullptr);
			ObjIterator<T, Key> new_obj(*this);
			new_obj.it += amt;
			if (new_obj.it == list_p->end()) {
				(*(new_obj.refcount_p))--;
				new_obj.list_p = nullptr;
				new_obj.refcount_p = nullptr;
			}
			return new_obj;
		}

		inline const ObjIterator<T, Key> operator++(int) {
			ObjIterator<T, Key> result(*this);
			++(*this);
			return result;
		}
	};

	template<typename T, typename Key = IdString>
	struct ObjRange
	{
		dict<Key, T> *list_p;
		int *refcount_p;

		ObjRange(decltype(list_p) list_p, int *refcount_p) : list_p(list_p), refcount_p(refcount_p) { }
		RTLIL::ObjIterator<T, Key> begin() { return RTLIL::ObjIterator<T, Key>(list_p, refcount_p); }
		RTLIL::ObjIterator<T, Key> end() { return RTLIL::ObjIterator<T, Key>(); }

		size_t size() const {
			return list_p->size();
		}

		operator pool<T>() const {
			pool<T> result;
			for (auto &it : *list_p)
				result.insert(it.second);
			return result;
		}

		operator std::vector<T>() const {
			std::vector<T> result;
			result.reserve(list_p->size());
			for (auto &it : *list_p)
				result.push_back(it.second);
			return result;
		}

		pool<T> to_pool() const { return *this; }
		std::vector<T> to_vector() const { return *this; }
	};
};

struct RTLIL::Const
{
	short int flags;
private:
	friend class KernelRtlilTest;
	FRIEND_TEST(KernelRtlilTest, ConstStr);
	using bitvectype = std::vector<RTLIL::State>;
	enum class backing_tag: bool { bits, string };
	// Do not access the union or tag even in Const methods unless necessary
	backing_tag tag;
	union {
		bitvectype bits_;
		std::string str_;
	};

	// Use these private utilities instead
	bool is_bits() const { return tag == backing_tag::bits; }
	bool is_str() const { return tag == backing_tag::string; }

	bitvectype* get_if_bits() { return is_bits() ? &bits_ : NULL; }
	std::string* get_if_str() { return is_str() ? &str_ : NULL; }
	const bitvectype* get_if_bits() const { return is_bits() ? &bits_ : NULL; }
	const std::string* get_if_str() const { return is_str() ? &str_ : NULL; }

	bitvectype& get_bits();
	std::string& get_str();
	const bitvectype& get_bits() const;
	const std::string& get_str() const;
	std::vector<RTLIL::State>& bits_internal();
	void bitvectorize_internal();

public:
	Const() : flags(RTLIL::CONST_FLAG_NONE), tag(backing_tag::bits), bits_(std::vector<RTLIL::State>()) {}
	Const(std::string str);
	Const(long long val); // default width is 32
	Const(long long val, int width);
	Const(RTLIL::State bit, int width = 1);
	Const(std::vector<RTLIL::State> bits) : flags(RTLIL::CONST_FLAG_NONE), tag(backing_tag::bits), bits_(std::move(bits)) {}
	Const(const std::vector<bool> &bits);
	Const(const RTLIL::Const &other);
	Const(RTLIL::Const &&other);
	RTLIL::Const &operator =(const RTLIL::Const &other);
	~Const();

	struct Builder
	{
		Builder() {}
		Builder(int expected_width) { bits.reserve(expected_width); }
		void push_back(RTLIL::State b) { bits.push_back(b); }
		int size() const { return static_cast<int>(bits.size()); }
		Const build() { return Const(std::move(bits)); }
	private:
		std::vector<RTLIL::State> bits;
	};

	bool operator <(const RTLIL::Const &other) const;
	bool operator ==(const RTLIL::Const &other) const;
	bool operator !=(const RTLIL::Const &other) const;

	[[deprecated("Don't use direct access to the internal std::vector<State>, that's an implementation detail.")]]
	std::vector<RTLIL::State>& bits() { return bits_internal(); }
	[[deprecated("Don't call bitvectorize() directly, it's an implementation detail.")]]
	void bitvectorize() const { const_cast<Const*>(this)->bitvectorize_internal(); }

	bool as_bool() const;

	// Convert the constant value to a C++ int.
	// NOTE: If the constant is too wide to fit in int (32 bits) this will
	// truncate any higher bits, potentially over/underflowing. Consider using
	// try_as_int, as_int_saturating, or guarding behind convertible_to_int
	// instead.
	int as_int(bool is_signed = false) const;

	// Returns true iff the constant can be converted to an int without
	// over/underflow.
	bool convertible_to_int(bool is_signed = false) const;

	// Returns the constant's value as an int if it can be represented without
	// over/underflow, or std::nullopt otherwise.
	std::optional<int> try_as_int(bool is_signed = false) const;

	// Returns the constant's value as an int if it can be represented without
	// over/underflow, otherwise the max/min value for int depending on the sign.
	int as_int_saturating(bool is_signed = false) const;

	void tag_bare_integer_const(const std::string &value);

	std::string as_string(const char* any = "-") const;
	static Const from_string(const std::string &str);
	std::vector<RTLIL::State> to_bits() const;

	std::string decode_string() const;
	int size() const;
	bool empty() const;

	void append(const RTLIL::Const &other);
	void set(int i, RTLIL::State state) {
		bits_internal()[i] = state;
	}
	void resize(int size, RTLIL::State fill) {
    log_assert(size >= 0 && size < RTLIL::WIDTH_LIMIT);
		bits_internal().resize(size, fill);
	}

	class const_iterator {
	private:
		const Const* parent;
		size_t idx;

	public:
		using iterator_category = std::bidirectional_iterator_tag;
		using value_type = State;
		using difference_type = std::ptrdiff_t;
		using pointer = const State*;
		using reference = const State&;

		const_iterator(const Const& c, size_t i) : parent(&c), idx(i) {}

		State operator*() const;

		const_iterator& operator++() { ++idx; return *this; }
		const_iterator& operator--() { --idx; return *this; }
		const_iterator operator++(int) { const_iterator result(*this); ++idx; return result; }
		const_iterator operator--(int) { const_iterator result(*this); --idx; return result; }
		const_iterator& operator+=(int i) { idx += i; return *this; }

		const_iterator operator+(int add) {
			return const_iterator(*parent, idx + add);
		}
		const_iterator operator-(int sub) {
			return const_iterator(*parent, idx - sub);
		}
		int operator-(const const_iterator& other) {
			return idx - other.idx;
		}

		bool operator==(const const_iterator& other) const {
			return idx == other.idx;
		}

		bool operator!=(const const_iterator& other) const {
			return !(*this == other);
		}
	};

	class iterator {
	private:
		Const* parent;
		size_t idx;

	public:
		class proxy {
		private:
			Const* parent;
			size_t idx;
		public:
			proxy(Const* parent, size_t idx) : parent(parent), idx(idx) {}
			operator State() const { return (*parent)[idx]; }
			proxy& operator=(State s) { parent->set(idx, s); return *this; }
			proxy& operator=(const proxy& other) { parent->set(idx, (*other.parent)[other.idx]); return *this; }
		};

		using iterator_category = std::bidirectional_iterator_tag;
		using value_type = State;
		using difference_type = std::ptrdiff_t;
		using pointer = proxy*;
		using reference = proxy;

		iterator(Const& c, size_t i) : parent(&c), idx(i) {}

		proxy operator*() const { return proxy(parent, idx); }
		iterator& operator++() { ++idx; return *this; }
		iterator& operator--() { --idx; return *this; }
		iterator operator++(int) { iterator result(*this); ++idx; return result; }
		iterator operator--(int) { iterator result(*this); --idx; return result; }
		iterator& operator+=(int i) { idx += i; return *this; }

		iterator operator+(int add) {
			return iterator(*parent, idx + add);
		}
		iterator operator-(int sub) {
			return iterator(*parent, idx - sub);
		}
		int operator-(const iterator& other) {
			return idx - other.idx;
		}

		bool operator==(const iterator& other) const {
			return idx == other.idx;
		}

		bool operator!=(const iterator& other) const {
			return !(*this == other);
		}
	};

	const_iterator begin() const {
		return const_iterator(*this, 0);
	}
	const_iterator end() const {
		return const_iterator(*this, size());
	}
	iterator begin() {
		return iterator(*this, 0);
	}
	iterator end() {
		return iterator(*this, size());
	}
	State back() const {
		return *(end() - 1);
	}
	State front() const {
		return *begin();
	}
	State at(size_t i) const {
		return *const_iterator(*this, i);
	}
	State operator[](size_t i) const {
		return *const_iterator(*this, i);
	}

	bool is_fully_zero() const;
	bool is_fully_ones() const;
	bool is_fully_def() const;
	bool is_fully_undef() const;
	bool is_fully_undef_x_only() const;
	bool is_onehot(int *pos = nullptr) const;

	RTLIL::Const extract(int offset, int len = 1, RTLIL::State padding = RTLIL::State::S0) const;

	// find the MSB without redundant leading bits
	int get_min_size(bool is_signed) const;

	// compress representation to the minimum required bits
	void compress(bool is_signed = false);

	std::optional<int> as_int_compress(bool is_signed) const;

	void extu(int width) {
		resize(width, RTLIL::State::S0);
	}

	void exts(int width) {
		resize(width, empty() ? RTLIL::State::Sx : back());
	}

	[[nodiscard]] Hasher hash_into(Hasher h) const;
};

struct RTLIL::ObjMeta
{
	IdString src = Twine::Null;
	IdString name = Twine::Null;  // used by Wire/Cell names (per-Design twines)
};

struct RTLIL::AttrObject
{
	dict<IdString, RTLIL::Const> attributes;

	// Pointer to a per-object metadata record in some pool (typically
	// the owning Design's). Nullable: cleared until first non-null write
	// of any field (src or name) and reset to null when all fields empty.
	RTLIL::ObjMeta *meta_ = nullptr;

	bool has_attribute(IdString id) const;

	void set_bool_attribute(IdString id, bool value=true);
	bool get_bool_attribute(IdString id) const;

	[[deprecated("Use Module::get_blackbox_attribute() instead.")]]
	bool get_blackbox_attribute(bool ignore_wb=false) const {
		return get_bool_attribute(ID::blackbox) || (!ignore_wb && get_bool_attribute(ID::whitebox));
	}

	void set_string_attribute(IdString id, string value);
	string get_string_attribute(IdString id) const;

	// static std::string strpool_attribute_to_str(const pool<string> &data);
	// void set_strpool_attribute(IdString id, const pool<string> &data);
	// void add_strpool_attribute(IdString id, const pool<string> &data);
	// pool<string> get_strpool_attribute(IdString id) const;
	void transfer_attribute(const AttrObject* from, IdString attr) {
		if (from->has_attribute(attr))
			attributes[attr] = from->attributes.at(attr);
	}

	void set_hdlname_attribute(const vector<string> &hierarchy);
	vector<string> get_hdlname_attribute() const;

	void set_intvec_attribute(IdString id, const vector<int> &data);
	vector<int> get_intvec_attribute(IdString id) const;
};

struct RTLIL::NamedObject : public RTLIL::AttrObject
{
	IdString name = Twine::Null;
};

#include "kernel/rtlil_twine_compat.h"

struct RTLIL::SigChunk
{
	RTLIL::Wire *wire;
	std::vector<RTLIL::State> data; // only used if wire == NULL, LSB at index 0
	int width, offset;

	SigChunk() : wire(nullptr), width(0), offset(0) {}
	SigChunk(const RTLIL::Const &value) : wire(nullptr), data(value.to_bits()), width(GetSize(data)), offset(0) {}
	SigChunk(RTLIL::Const &&value) : wire(nullptr), data(value.to_bits()), width(GetSize(data)), offset(0) {}
	SigChunk(RTLIL::Wire *wire) : wire(wire), width(GetSize(wire)), offset(0) {}
	SigChunk(RTLIL::Wire *wire, int offset, int width = 1) : wire(wire), width(width), offset(offset) {}
	SigChunk(const std::string &str) : SigChunk(RTLIL::Const(str)) {}
	SigChunk(int val) /*default width 32*/ : SigChunk(RTLIL::Const(val)) {}
	SigChunk(int val, int width) : SigChunk(RTLIL::Const(val, width)) {}
	SigChunk(RTLIL::State bit, int width = 1) : SigChunk(RTLIL::Const(bit, width)) {}
	SigChunk(const RTLIL::SigBit &bit);

	RTLIL::SigChunk extract(int offset, int length) const;
	RTLIL::SigBit operator[](int offset) const;
	inline int size() const { return width; }
	inline bool is_wire() const { return wire != NULL; }

	bool operator <(const RTLIL::SigChunk &other) const;
	bool operator ==(const RTLIL::SigChunk &other) const;
	bool operator !=(const RTLIL::SigChunk &other) const;
};

struct RTLIL::SigBit
{
	RTLIL::Wire *wire;
	union {
		RTLIL::State data; // used if wire == NULL
		int offset;        // used if wire != NULL
	};

	SigBit();
	SigBit(RTLIL::State bit);
	explicit SigBit(bool bit);
	SigBit(RTLIL::Wire *wire);
	SigBit(RTLIL::Wire *wire, int offset);
	SigBit(const RTLIL::SigChunk &chunk);
	SigBit(const RTLIL::SigChunk &chunk, int index);
	SigBit(const RTLIL::SigSpec &sig);
	SigBit(const RTLIL::SigBit &sigbit) = default;
	RTLIL::SigBit &operator =(const RTLIL::SigBit &other) = default;

	inline bool is_wire() const { return wire != NULL; }

	bool operator <(const RTLIL::SigBit &other) const;
	bool operator ==(const RTLIL::SigBit &other) const;
	bool operator !=(const RTLIL::SigBit &other) const;
	[[nodiscard]] Hasher hash_into(Hasher h) const;
	[[nodiscard]] Hasher hash_top() const;
};

namespace hashlib {
	template <>
	struct hash_ops<RTLIL::SigBit> {
		static inline bool cmp(const RTLIL::SigBit &a, const RTLIL::SigBit &b) {
			return a == b;
		}
		[[nodiscard]] static inline Hasher hash(const RTLIL::SigBit sb) {
			return sb.hash_top();
		}
		[[nodiscard]] static inline Hasher hash_into(const RTLIL::SigBit sb, Hasher h) {
			return sb.hash_into(h);
		}
	};
};

struct RTLIL::SigSpecIterator
{
	typedef std::input_iterator_tag iterator_category;
	typedef RTLIL::SigBit value_type;
	typedef ptrdiff_t difference_type;
	typedef RTLIL::SigBit* pointer;
	typedef RTLIL::SigBit& reference;

	RTLIL::SigSpec *sig_p;
	int index;

	inline RTLIL::SigBit &operator*() const;
	inline bool operator!=(const RTLIL::SigSpecIterator &other) const { return index != other.index; }
	inline bool operator==(const RTLIL::SigSpecIterator &other) const { return index == other.index; }
	inline void operator++() { index++; }
};

struct RTLIL::SigSpecConstIterator
{
	typedef std::input_iterator_tag iterator_category;
	typedef RTLIL::SigBit value_type;
	typedef ptrdiff_t difference_type;
	typedef RTLIL::SigBit* pointer;
	typedef RTLIL::SigBit& reference;

	const RTLIL::SigSpec *sig_p;
	RTLIL::SigBit bit;
	int index;

	inline const RTLIL::SigBit &operator*();
	inline bool operator!=(const RTLIL::SigSpecConstIterator &other) const { return index != other.index; }
	inline bool operator==(const RTLIL::SigSpecIterator &other) const { return index == other.index; }
	inline void operator++() { index++; }
};

struct RTLIL::SigSpec
{
private:
	friend class SigSpecRepTest;
	FRIEND_TEST(SigSpecRepTest, Extract);
	enum Representation : char {
		CHUNK,
		BITS,
	};
	// An AtomicHash is either clear or a nonzero integer.
	struct AtomicHash {
		// Create an initially clear value.
		AtomicHash() : atomic_(0) {}
		AtomicHash(const AtomicHash &rhs) : atomic_(rhs.load()) {}
		AtomicHash &operator=(const AtomicHash &rhs) { store(rhs.load()); return *this; }
		// Read the hash. Returns nullopt if the hash is clear.
		std::optional<Hasher::hash_t> read() const {
			Hasher::hash_t value = load();
			if (value == 0)
				return std::nullopt;
			return value;
		}
		// Set the hash. If the value is already set, then the new value must
		// equal the current value.
		void set(Hasher::hash_t value) const {
			log_assert(value != 0);
			Hasher::hash_t old = const_cast<std::atomic<Hasher::hash_t>&>(atomic_)
					.exchange(value, std::memory_order_relaxed);
			log_assert(old == 0 || old == value);
		}
		void clear() { store(0); }
	private:
		int load() const { return atomic_.load(std::memory_order_relaxed); }
		void store(Hasher::hash_t value) const {
			const_cast<std::atomic<Hasher::hash_t>&>(atomic_).store(value, std::memory_order_relaxed);
		}

		std::atomic<Hasher::hash_t> atomic_;
	};

	Representation rep_;
	AtomicHash hash_;
	union {
		RTLIL::SigChunk chunk_;
		std::vector<RTLIL::SigBit> bits_; // LSB at index 0
	};

	void init_empty_bits() {
		rep_ = BITS;
		new (&bits_) std::vector<RTLIL::SigBit>;
	}

	void unpack();
	inline void inline_unpack() {
		if (rep_ == CHUNK)
			unpack();
	}
	void try_repack();

	Hasher::hash_t updhash() const;
	void destroy() {
		if (rep_ == CHUNK)
			chunk_.~SigChunk();
		else
			bits_.~vector();
	}
	friend struct Chunks;

public:
	SigSpec() { init_empty_bits(); }
	SigSpec(std::initializer_list<RTLIL::SigSpec> parts);
	SigSpec(const SigSpec &value) : rep_(value.rep_), hash_(value.hash_) {
		if (value.rep_ == CHUNK)
			new (&chunk_) RTLIL::SigChunk(value.chunk_);
		else
			new (&bits_) std::vector<RTLIL::SigBit>(value.bits_);
	}
	SigSpec(SigSpec &&value) : rep_(value.rep_), hash_(value.hash_) {
		if (value.rep_ == CHUNK)
			new (&chunk_) RTLIL::SigChunk(std::move(value.chunk_));
		else
			new (&bits_) std::vector<RTLIL::SigBit>(std::move(value.bits_));
	}
	SigSpec(const RTLIL::Const &value);
	SigSpec(RTLIL::Const &&value);
	SigSpec(const RTLIL::SigChunk &chunk);
	SigSpec(RTLIL::SigChunk &&chunk);
	SigSpec(RTLIL::Wire *wire);
	SigSpec(RTLIL::Wire *wire, int offset, int width = 1);
	SigSpec(const std::string &str);
	SigSpec(int val, int width = 32);
	SigSpec(RTLIL::State bit, int width = 1);
	SigSpec(const RTLIL::SigBit &bit, int width = 1);
	SigSpec(const std::vector<RTLIL::SigChunk> &chunks);
	SigSpec(const std::vector<RTLIL::SigBit> &bits);
	SigSpec(const pool<RTLIL::SigBit> &bits);
	SigSpec(const std::set<RTLIL::SigBit> &bits);
	explicit SigSpec(bool bit);
	~SigSpec() {
		destroy();
	}

	SigSpec &operator=(const SigSpec &rhs) {
		destroy();
		rep_ = rhs.rep_;
		hash_ = rhs.hash_;
		if (rep_ == CHUNK)
			new (&chunk_) RTLIL::SigChunk(rhs.chunk_);
		else
			new (&bits_) std::vector<RTLIL::SigBit>(rhs.bits_);
		return *this;
	}
	SigSpec &operator=(SigSpec &&rhs) {
		destroy();
		rep_ = rhs.rep_;
		hash_ = rhs.hash_;
		if (rep_ == CHUNK)
			new (&chunk_) RTLIL::SigChunk(std::move(rhs.chunk_));
		else
			new (&bits_) std::vector<RTLIL::SigBit>(std::move(rhs.bits_));
		return *this;
	}

	// SigSpec::Chunks holds one reconstructed chunk at a time
	// to provide the SigSpec::chunks() read-only chunks view
	// since vector<SigChunk> SigSpec::chunks_ has been removed
	struct Chunks {
		Chunks(const SigSpec &spec) : spec(spec) {}
		struct const_iterator {
			using iterator_category = std::forward_iterator_tag;
			using value_type = const SigChunk &;
			using difference_type = std::ptrdiff_t;
			using pointer = const SigChunk *;
			using reference = const SigChunk &;

			const SigSpec &spec;
			int bit_index;
			SigChunk chunk;

			const_iterator(const SigSpec &spec) : spec(spec) {
				bit_index = 0;
				if (spec.rep_ == BITS)
					next_chunk_bits();
			}
			enum End { END };
			const_iterator(const SigSpec &spec, End) : spec(spec) {
				bit_index = spec.size();
			}
			void next_chunk_bits();

			const SigChunk &operator*() {
				if (spec.rep_ == CHUNK)
					return spec.chunk_;
				return chunk;
			};
			const SigChunk *operator->() { return &**this; }
			const_iterator &operator++() {
				bit_index += (**this).width;
				if (spec.rep_ == BITS)
					next_chunk_bits();
				return *this;
			}
			bool operator==(const const_iterator &rhs) const { return bit_index == rhs.bit_index; }
			bool operator!=(const const_iterator &rhs) const { return !(*this == rhs); }
		};
		const_iterator begin() const { return const_iterator(spec); }
		const_iterator end() const {
			const_iterator it(spec, const_iterator::END);
			return it;
		}
		// Later we should deprecate these and remove their in-tree calls,
		// so we can eventually remove chunk_vector.
		std::vector<RTLIL::SigChunk>::const_reverse_iterator rbegin() {
			ensure_chunk_vector();
			return chunk_vector.rbegin();
		}
		std::vector<RTLIL::SigChunk>::const_reverse_iterator rend() {
			ensure_chunk_vector();
			return chunk_vector.rend();
		}
		int size() {
			ensure_chunk_vector();
			return chunk_vector.size();
		}
		int size() const {
			return std::distance(begin(), end());
		}
		const SigChunk &at(int index) {
			ensure_chunk_vector();
			return chunk_vector.at(index);
		}
		operator const std::vector<RTLIL::SigChunk>&() {
			ensure_chunk_vector();
			return chunk_vector;
		}
	private:
		void ensure_chunk_vector() {
			if (spec.size() > 0 && chunk_vector.empty()) {
				for (const RTLIL::SigChunk &c : *this)
					chunk_vector.push_back(c);
			}
		}
		const SigSpec &spec;
		std::vector<RTLIL::SigChunk> chunk_vector;
	};
	friend struct Chunks::const_iterator;

	inline Chunks chunks() const { return {*this}; }
	inline const SigSpec &bits() const { return *this; }

	inline int size() const { return rep_ == CHUNK ? chunk_.width : GetSize(bits_); }
	inline bool empty() const { return size() == 0; };

	inline RTLIL::SigBit &operator[](int index) { inline_unpack(); hash_.clear(); return bits_.at(index); }
	inline RTLIL::SigBit operator[](int index) const {
		if (rep_ == CHUNK) {
			if (index < 0 || index >= chunk_.width)
				throw std::out_of_range("SigSpec::operator[]");
			if (chunk_.wire)
				return RTLIL::SigBit(chunk_.wire, chunk_.offset + index);
			return RTLIL::SigBit(chunk_.data[index]);
		}
		return bits_.at(index);
	}

	inline RTLIL::SigSpecIterator begin() { RTLIL::SigSpecIterator it; it.sig_p = this; it.index = 0; return it; }
	inline RTLIL::SigSpecIterator end() { RTLIL::SigSpecIterator it; it.sig_p = this; it.index = size(); return it; }

	inline RTLIL::SigSpecConstIterator begin() const { RTLIL::SigSpecConstIterator it; it.sig_p = this; it.index = 0; return it; }
	inline RTLIL::SigSpecConstIterator end() const { RTLIL::SigSpecConstIterator it; it.sig_p = this; it.index = size(); return it; }

	void sort();
	void sort_and_unify();

	void replace(const RTLIL::SigSpec &pattern, const RTLIL::SigSpec &with);
	void replace(const RTLIL::SigSpec &pattern, const RTLIL::SigSpec &with, RTLIL::SigSpec *other) const;

	void replace(const dict<RTLIL::SigBit, RTLIL::SigBit> &rules);
	void replace(const dict<RTLIL::SigBit, RTLIL::SigBit> &rules, RTLIL::SigSpec *other) const;

	void replace(const std::map<RTLIL::SigBit, RTLIL::SigBit> &rules);
	void replace(const std::map<RTLIL::SigBit, RTLIL::SigBit> &rules, RTLIL::SigSpec *other) const;

	void replace(int offset, const RTLIL::SigSpec &with);

	void remove(const RTLIL::SigSpec &pattern);
	void remove(const RTLIL::SigSpec &pattern, RTLIL::SigSpec *other) const;
	void remove2(const RTLIL::SigSpec &pattern, RTLIL::SigSpec *other);

	void remove(const pool<RTLIL::SigBit> &pattern);
	void remove(const pool<RTLIL::SigBit> &pattern, RTLIL::SigSpec *other) const;
	void remove2(const pool<RTLIL::SigBit> &pattern, RTLIL::SigSpec *other);
	void remove2(const std::set<RTLIL::SigBit> &pattern, RTLIL::SigSpec *other);
	void remove2(const pool<RTLIL::Wire*> &pattern, RTLIL::SigSpec *other);

	void remove(int offset, int length = 1);
	void remove_const();

	RTLIL::SigSpec extract(const RTLIL::SigSpec &pattern, const RTLIL::SigSpec *other = NULL) const;
	RTLIL::SigSpec extract(const pool<RTLIL::SigBit> &pattern, const RTLIL::SigSpec *other = NULL) const;
	RTLIL::SigSpec extract(int offset, int length = 1) const;
	RTLIL::SigSpec extract_end(int offset) const { return extract(offset, size() - offset); }

	void rewrite_wires(std::function<void(RTLIL::Wire*& wire)> rewrite);

	RTLIL::SigBit lsb() const { log_assert(size()); return (*this)[0]; };
	RTLIL::SigBit msb() const { log_assert(size()); return (*this)[size() - 1]; };
	RTLIL::SigBit front() const { return (*this)[0]; }
	RTLIL::SigBit back() const { return (*this)[size() - 1]; }

	void append(const RTLIL::SigSpec &signal);
	inline void append(Wire *wire) { append(RTLIL::SigSpec(wire)); }
	inline void append(const RTLIL::SigChunk &chunk) { append(RTLIL::SigSpec(chunk)); }
	inline void append(const RTLIL::Const &const_) { append(RTLIL::SigSpec(const_)); }

	void append(const RTLIL::SigBit &bit);
	inline void append(RTLIL::State state) { append(RTLIL::SigBit(state)); }
	inline void append(bool bool_) { append(RTLIL::SigBit(bool_)); }

	void extend_u0(int width, bool is_signed = false);

	RTLIL::SigSpec repeat(int num) const;

	void reverse() { inline_unpack(); std::reverse(bits_.begin(), bits_.end()); }

	bool operator <(const RTLIL::SigSpec &other) const;
	bool operator ==(const RTLIL::SigSpec &other) const;
	inline bool operator !=(const RTLIL::SigSpec &other) const { return !(*this == other); }

	bool is_wire() const;
	bool is_chunk() const;
	inline bool is_bit() const { return size() == 1; }

	bool known_driver() const;

	bool is_fully_const() const;
	bool is_fully_zero() const;
	bool is_fully_ones() const;
	bool is_fully_def() const;
	bool is_fully_undef() const;
	bool has_const() const;
	bool has_const(State state) const;
	bool has_marked_bits() const;
	bool is_onehot(int *pos = nullptr) const;

	bool as_bool() const;

	// Convert the SigSpec to a C++ int, assuming all bits are constant.
	// NOTE: If the value is too wide to fit in int (32 bits) this will
	// truncate any higher bits, potentially over/underflowing. Consider using
	// try_as_int, as_int_saturating, or guarding behind convertible_to_int
	// instead.
	int as_int(bool is_signed = false) const;

	// Returns true iff the SigSpec is constant and can be converted to an int
	// without over/underflow.
	bool convertible_to_int(bool is_signed = false) const;

	// Returns the SigSpec's value as an int if it is a constant and can be
	// represented without over/underflow, or std::nullopt otherwise.
	std::optional<int> try_as_int(bool is_signed = false) const;

	// Returns an all constant SigSpec's value as an int if it can be represented
	// without over/underflow, otherwise the max/min value for int depending on
	// the sign.
	int as_int_saturating(bool is_signed = false) const;

	std::string as_string() const;
	// Returns std::nullopt if there are any non-constant bits. Returns an empty
	// Const if this has zero width.
	std::optional<RTLIL::Const> try_as_const() const;
	RTLIL::Const as_const() const;
	RTLIL::Wire *as_wire() const;
	RTLIL::SigChunk as_chunk() const;
	RTLIL::SigBit as_bit() const;

	bool match(const char* pattern) const;

	std::set<RTLIL::SigBit> to_sigbit_set() const;
	pool<RTLIL::SigBit> to_sigbit_pool() const;
	std::vector<RTLIL::SigBit> to_sigbit_vector() const;
	std::map<RTLIL::SigBit, RTLIL::SigBit> to_sigbit_map(const RTLIL::SigSpec &other) const;
	dict<RTLIL::SigBit, RTLIL::SigBit> to_sigbit_dict(const RTLIL::SigSpec &other) const;

	static bool parse(RTLIL::SigSpec &sig, RTLIL::Module *module, std::string str);
	static bool parse_sel(RTLIL::SigSpec &sig, RTLIL::Design *design, RTLIL::Module *module, std::string str);
	static bool parse_rhs(const RTLIL::SigSpec &lhs, RTLIL::SigSpec &sig, RTLIL::Module *module, std::string str);

	operator std::vector<RTLIL::SigChunk>() const;
	operator std::vector<RTLIL::SigBit>() const { return to_sigbit_vector(); }
	const RTLIL::SigBit &at(int offset, const RTLIL::SigBit &defval) { return offset < size() ? (*this)[offset] : defval; }
	RTLIL::SigBit& at(int offset) { return (*this)[offset]; }
	RTLIL::SigBit at(int offset) const { return (*this)[offset]; }

	[[nodiscard]] Hasher hash_into(Hasher h) const {
		Hasher::hash_t val;
		if (std::optional<Hasher::hash_t> current = hash_.read())
			val = *current;
		else
			val = updhash();
		h.eat(val);
		return h;
	}

#ifndef NDEBUG
	void check(const Module *mod = nullptr) const;
#else
	void check(const Module *mod = nullptr) const { (void)mod; }
#endif
};

struct RTLIL::Selection
{
	// selection includes boxed modules
	bool selects_boxes;
	// selection covers full design, including boxed modules
	bool complete_selection;
	// selection covers full design, not including boxed modules
	bool full_selection;
	pool<IdString> selected_modules;
	dict<IdString, pool<IdString>> selected_members;
	RTLIL::Design *current_design;

	// create a new selection
	Selection(
		// should the selection cover the full design
		bool full = true,
		// should the selection include boxed modules
		bool boxes = false,
		// the design to select from
		RTLIL::Design *design = nullptr
	) :
		selects_boxes(boxes), complete_selection(full && boxes), full_selection(full && !boxes), current_design(design) { }

	// checks if the given module exists in the current design and is a
	// boxed module, warning the user if the current design is not set
	bool boxed_module(IdString mod_name) const;

	// checks if the given module is included in this selection
	bool selected_module(IdString mod_name) const;

	// checks if the given module is wholly included in this selection,
	// i.e. not partially selected
	bool selected_whole_module(IdString mod_name) const;

	// checks if the given member from the given module is included in this
	// selection
	bool selected_member(IdString mod_name, IdString memb_name) const;

	// optimizes this selection for the given design by:
	// - removing non-existent modules and members, any boxed modules and
	//   their members (if selection does not include boxes), and any
	//   partially selected modules with no selected members;
	// - marking partially selected modules as wholly selected if all
	//   members of that module are selected; and
	// - marking selection as a complete_selection if all modules in the
	//   given design are selected, or a full_selection if it does not
	//   include boxes.
	void optimize(RTLIL::Design *design);

	// checks if selection covers full design (may or may not include
	// boxed-modules)
	bool selects_all() const {
		return full_selection || complete_selection;
	}

	// add whole module to this selection
	template<typename T1> void select(T1 *module) {
		if (!selects_all() && selected_modules.count(module->meta_->name) == 0) {
			IdString name = module->meta_->name;
			selected_modules.insert(name);
			selected_members.erase(name);
			if (module->get_blackbox_attribute())
				selects_boxes = true;
		}
	}

	// add member of module to this selection
	template<typename T1, typename T2> void select(T1 *module, T2 *member) {
		if (!selects_all() && selected_modules.count(module->meta_->name) == 0) {
			selected_members[module->meta_->name].insert(member->meta_->name);
			if (module->get_blackbox_attribute())
				selects_boxes = true;
		}
	}

	// checks if selection is empty
	bool empty() const {
		return !selects_all() && selected_modules.empty() && selected_members.empty();
	}

	// clear this selection, leaving it empty
	void clear();

	// create a new selection which is empty
	static Selection EmptySelection(RTLIL::Design *design = nullptr) { return Selection(false, false, design); };

	// create a new selection with all non-boxed modules
	static Selection FullSelection(RTLIL::Design *design = nullptr) { return Selection(true, false, design); };

	// create a new selection with all modules, including boxes
	static Selection CompleteSelection(RTLIL::Design *design = nullptr) { return Selection(true, true, design); };
};

struct RTLIL::Monitor
{
	Hasher::hash_t hashidx_;
	[[nodiscard]] Hasher hash_into(Hasher h) const { h.eat(hashidx_); return h; }

	Monitor() {
		static unsigned int hashidx_count = 123456789;
		hashidx_count = mkhash_xorshift(hashidx_count);
		hashidx_ = hashidx_count;
	}

	virtual ~Monitor() { }
	virtual void notify_module_add(RTLIL::Module*) { }
	virtual void notify_module_del(RTLIL::Module*) { }
	virtual void notify_connect(RTLIL::Cell*, IdString, const RTLIL::SigSpec&, const RTLIL::SigSpec&) { }
	virtual void notify_connect(RTLIL::Module*, const RTLIL::SigSig&) { }
	virtual void notify_connect(RTLIL::Module*, const std::vector<RTLIL::SigSig>&) { }
	virtual void notify_blackout(RTLIL::Module*) { }
};

// Forward declaration; defined in preproc.h.
struct define_map_t;

struct RTLIL::Design
{
	Hasher::hash_t hashidx_;
	[[nodiscard]] Hasher hash_into(Hasher h) const { h.eat(hashidx_); return h; }

	pool<RTLIL::Monitor*> monitors;
	dict<std::string, std::string> scratchpad;

	bool flagBufferedNormalized = false;
	void bufNormalize(bool enable=true);

	int refcount_modules_;
	dict<IdString, RTLIL::Module*> modules_;
	std::vector<RTLIL::Binding*> bindings_;

	TwinePool twines;

	// Per-Design ObjMeta pool: stable storage (deque) + LIFO freelist of
	// returned slots. AttrObject::meta_ points into obj_meta_storage_.
	std::deque<RTLIL::ObjMeta> obj_meta_storage_;
	std::vector<RTLIL::ObjMeta*> obj_meta_free_;

	RTLIL::ObjMeta *alloc_obj_meta();
	void free_obj_meta(RTLIL::ObjMeta *m);

	IdString obj_src_id(const RTLIL::AttrObject *obj) const {
		return (obj->meta_ ? obj->meta_->src : Twine::Null);
	}
	void obj_set_src_id(RTLIL::AttrObject *obj, IdString id);
	void obj_release_src(RTLIL::AttrObject *obj);

	std::string obj_name(const RTLIL::AttrObject *obj) const {
		return (obj->meta_ ? twines.flat_string(obj->meta_->name) : std::string());
	}
	// void obj_set_name(RTLIL::AttrObject *obj, IdString name);
	// void obj_release_name(RTLIL::AttrObject *obj);

	// Wire/Cell names: stored as IdString in twines.
	// IdString obj_name(const RTLIL::AttrObject *obj) const {
	// 	return (obj->meta_ ? obj->meta_->name : Twine::Null);
	// }
	// void obj_set_name(RTLIL::AttrObject *obj, IdString id);
	// void obj_release_name(RTLIL::AttrObject *obj);

	// Replacements for the methods that used to live on AttrObject and
	// took an explicit TwinePool*. Same semantics; the pool resolves
	// to this->twines internally.
	void set_src_attribute(RTLIL::AttrObject *obj, IdString src);
	std::string get_src_attribute(const RTLIL::AttrObject *obj) const;
	void adopt_src_from(RTLIL::AttrObject *obj, const RTLIL::AttrObject *source);
	void adopt_src_from(RTLIL::AttrObject *obj, const RTLIL::AttrObject *source,
			const TwinePool *src_pool);
	void absorb_attrs(RTLIL::AttrObject *obj, dict<IdString, RTLIL::Const> &&buf);

	// Merge `source`'s src attribute into `target`'s src attribute via the
	// twine pool. After the call `target` carries the combined "@N" ref.
	// Handles every case: source has a "@N" ref → reuse that Id; source
	// has a legacy pipe-joined literal → split and intern each leaf;
	// target had pre-existing src → its leaves are folded in too. Use
	// this instead of `target->add_strpool_attribute(ID::src, source->get_strpool_attribute(ID::src))`,
	// which round-trips through a flat string and corrupts "@N" refs.
	void merge_src(RTLIL::AttrObject *target, const RTLIL::AttrObject *source);

	// Same as merge_src but consumes a raw set of leaf strings (each of
	// which may itself be either a "@N" ref or a literal path).
	void merge_src(RTLIL::AttrObject *target, const pool<std::string> &leaves);

	// Returns the resolved leaf-string set backing obj's src attribute.
	// "@N" refs are expanded through the pool; legacy pipe-joined
	// literals are split. Use this instead of get_strpool_attribute(ID::src)
	// when you actually need to iterate the path:line.col entries.
	pool<std::string> src_leaves(const RTLIL::AttrObject *obj) const;

	// Walk the design, collect the set of "@N" ids actually referenced by
	// any AttrObject's src, then compact twines to contain only those
	// nodes plus their transitive leaf children, and rewrite every cell
	// src attribute through the resulting old-id -> new-id remap.
	// Intermediate concats produced by successive merges become unreferenced
	// once a fresh concat takes their place on the surviving cell, so this
	// is what reaps them. Returns the number of nodes freed.
	size_t gc_twines();

	std::vector<std::unique_ptr<AST::AstNode>> verilog_packages, verilog_globals;
	std::unique_ptr<define_map_t> verilog_defines;

	std::vector<RTLIL::Selection> selection_stack;
	dict<IdString, RTLIL::Selection> selection_vars;
	IdString selected_active_module;

	Design();
	~Design();

	RTLIL::ObjRange<RTLIL::Module*, IdString> modules();
	// RTLIL::Module *module(IdString name);
	// const RTLIL::Module *module(IdString name) const;
	RTLIL::Module *module(IdString name);
	const RTLIL::Module *module(IdString name) const;
	RTLIL::Module *top_module() const;

	bool has(IdString id) const {
		return modules_.count(id) != 0;
	}

	void add(RTLIL::Module *module);
	void add(RTLIL::Binding *binding);

	RTLIL::Module *addModule(IdString name);
	void remove(RTLIL::Module *module);
	void rename(RTLIL::Module *module, IdString new_name);

	void scratchpad_unset(const std::string &varname);

	void scratchpad_set_int(const std::string &varname, int value);
	void scratchpad_set_bool(const std::string &varname, bool value);
	void scratchpad_set_string(const std::string &varname, std::string value);

	int scratchpad_get_int(const std::string &varname, int default_value = 0) const;
	bool scratchpad_get_bool(const std::string &varname, bool default_value = false) const;
	std::string scratchpad_get_string(const std::string &varname, const std::string &default_value = std::string()) const;

	void sort();
	void sort_modules();
	void check();
	void optimize();

	// Wholesale-copy this design into `dst`. `dst` must be empty (no
	// modules). Copies twines verbatim and clones each module
	// copy_from pool rebuild and yields byte-identical RTLIL output
	// across design -push/-pop, -save/-load, etc.
	void clone_into(RTLIL::Design *dst) const;

	// checks if the given module is included in the current selection
	bool selected_module(IdString mod_name) const;

	// checks if the given module is wholly included in the current
	// selection, i.e. not partially selected
	bool selected_whole_module(IdString mod_name) const;

	// checks if the given member from the given module is included in the
	// current selection
	bool selected_member(IdString mod_name, IdString memb_name) const;

	// checks if the given module is included in the current selection
	bool selected_module(RTLIL::Module *mod) const;

	// checks if the given module is wholly included in the current
	// selection, i.e. not partially selected
	bool selected_whole_module(RTLIL::Module *mod) const;

	// push the given selection to the selection stack
	void push_selection(RTLIL::Selection sel);
	// push a new selection to the selection stack, with nothing selected
	void push_empty_selection();
	// push a new selection to the selection stack, with all non-boxed
	// modules selected
	void push_full_selection();
	// push a new selection to the selection stack, with all modules
	// selected including boxes
	void push_complete_selection();
	// pop the current selection from the stack, returning to a full
	// selection (no boxes) if the stack is empty
	void pop_selection();

	// get the current selection
	RTLIL::Selection &selection() {
		return selection_stack.back();
	}

	// get the current selection
	const RTLIL::Selection &selection() const {
		return selection_stack.back();
	}

	// is the current selection a full selection (no boxes)
	bool full_selection() const {
		return selection().full_selection;
	}

	// is the given module in the current selection
	template<typename T1> bool selected(T1 *module) const {
		return selected_module(module->name);
	}

	// is the given member of the given module in the current selection
	template<typename T1, typename T2> bool selected(T1 *module, T2 *member) const {
		return selected_member(module->meta_->name, member->meta_->name);
	}

	// add whole module to the current selection
	template<typename T1> void select(T1 *module) {
		RTLIL::Selection &sel = selection();
		sel.select(module);
	}

	// add member of module to the current selection
	template<typename T1, typename T2> void select(T1 *module, T2 *member) {
		RTLIL::Selection &sel = selection();
		sel.select(module, member);
	}


	// returns all selected modules
	std::vector<RTLIL::Module*> selected_modules(
		// controls if partially selected modules are included
		RTLIL::SelectPartials partials = SELECT_ALL,
		// controls if boxed modules are included
		RTLIL::SelectBoxes boxes = SB_UNBOXED_WARN
	) const;

	// returns all selected modules, and may include boxes
	std::vector<RTLIL::Module*> all_selected_modules() const { return selected_modules(SELECT_ALL, SB_ALL); }
	// returns all selected unboxed modules, silently ignoring any boxed
	// modules in the selection
	std::vector<RTLIL::Module*> selected_unboxed_modules() const { return selected_modules(SELECT_ALL, SB_UNBOXED_ONLY); }
	// returns all selected unboxed modules, warning the user if any boxed
	// modules have been ignored
	std::vector<RTLIL::Module*> selected_unboxed_modules_warn() const { return selected_modules(SELECT_ALL, SB_UNBOXED_WARN); }

	[[deprecated("Use select_unboxed_whole_modules() to maintain prior behaviour, or consider one of the other selected whole module helpers.")]]
	std::vector<RTLIL::Module*> selected_whole_modules() const { return selected_modules(SELECT_WHOLE_ONLY, SB_UNBOXED_WARN); }
	// returns all selected whole modules, silently ignoring partially
	// selected modules, and may include boxes
	std::vector<RTLIL::Module*> all_selected_whole_modules() const { return selected_modules(SELECT_WHOLE_ONLY, SB_ALL); }
	// returns all selected whole modules, warning the user if any partially
	// selected or boxed modules have been ignored; optionally includes
	// selected whole modules with the 'whitebox' attribute
	std::vector<RTLIL::Module*> selected_whole_modules_warn(
		// should whole modules with the 'whitebox' attribute be
		// included
		bool include_wb = false
	) const { return selected_modules(SELECT_WHOLE_WARN, include_wb ? SB_EXCL_BB_WARN : SB_UNBOXED_WARN); }
	// returns all selected unboxed whole modules, silently ignoring
	// partially selected or boxed modules
	std::vector<RTLIL::Module*> selected_unboxed_whole_modules() const { return selected_modules(SELECT_WHOLE_ONLY, SB_UNBOXED_ONLY); }
	// returns all selected unboxed whole modules, warning the user if any
	// partially selected or boxed modules have been ignored
	std::vector<RTLIL::Module*> selected_unboxed_whole_modules_warn() const { return selected_modules(SELECT_WHOLE_WARN, SB_UNBOXED_WARN); }

	static std::map<unsigned int, RTLIL::Design*> *get_all_designs(void);

	std::string to_rtlil_str(bool only_selected = true) const;
};

namespace RTLIL_BACKEND {
void dump_wire(std::ostream &f, std::string indent, const RTLIL::Wire *wire, const RTLIL::Design *design, bool resolve_src);
}

struct RTLIL::Wire : public RTLIL::AttrObject
{
private:
	struct ConstructToken { explicit ConstructToken() = default; };
	friend struct RTLIL::Design;
	friend struct RTLIL::Cell;
	friend struct RTLIL::Module;
public:
	// Shadows NamedObject::name. Reads materialise via twines; writes
	[[no_unique_address]] RTLIL::WireNameMasq name;

	Hasher::hash_t hashidx_;
	[[nodiscard]] Hasher hash_into(Hasher h) const { h.eat(hashidx_); return h; }
	// use module->addWire() and module->remove() to create or destroy wires
	Wire(ConstructToken);
	~Wire();

	friend void RTLIL_BACKEND::dump_wire(std::ostream &f, std::string indent, const RTLIL::Wire *wire, const RTLIL::Design *design, bool resolve_src);
	RTLIL::Cell *driverCell_ = nullptr;
	IdString driverPort_ = Twine::Null;

	// do not simply copy wires
	Wire(ConstructToken, RTLIL::Wire &other);
	void operator=(RTLIL::Wire &other) = delete;

	RTLIL::Module *module;
	int width, start_offset, port_id;
	bool port_input, port_output, upto, is_signed;

	// Context-aware src helpers. Resolve Design via module->design and
	// route to the per-Design meta vector; assert the wire is attached.
	IdString src_id() const;
	IdString src_ref() const { return src_id(); }
	void set_src_id(IdString id);
	void set_src_attribute(IdString src);
	std::string get_src_attribute() const;
	// Transfer src from `source` verbatim (same pool). Asserts attached
	// to a design.
	void adopt_src_from(const RTLIL::AttrObject *source);
	void transfer_src_attribute(const RTLIL::AttrObject *source) { adopt_src_from(source); }
	void absorb_attrs(dict<IdString, RTLIL::Const> &&buf);

	bool known_driver() const { return driverCell_ != nullptr; }

	RTLIL::Cell *driverCell() const    { log_assert(driverCell_); return driverCell_; };
	IdString driverPort() const { log_assert(driverCell_); return driverPort_; };

	int from_hdl_index(int hdl_index) {
		int zero_index = hdl_index - start_offset;
		int rtlil_index = upto ? width - 1 - zero_index : zero_index;
		return rtlil_index >= 0 && rtlil_index < width ? rtlil_index : INT_MIN;
	}

	int to_hdl_index(int rtlil_index) {
		if (rtlil_index < 0 || rtlil_index >= width)
			return INT_MIN;
		int zero_index = upto ? width - 1 - rtlil_index : rtlil_index;
		return zero_index + start_offset;
	}

	std::string to_rtlil_str() const;

#ifdef YOSYS_ENABLE_PYTHON
	static std::map<unsigned int, RTLIL::Wire*> *get_all_wires(void);
#endif
};

inline int GetSize(RTLIL::Wire *wire) {
	return wire->width;
}

struct RTLIL::Memory : public RTLIL::AttrObject
{
	Hasher::hash_t hashidx_;
	[[nodiscard]] Hasher hash_into(Hasher h) const { h.eat(hashidx_); return h; }

	Memory();
	~Memory();

	// Wire::module. Set by Module::addMemory / the frontends that
	// construct Memory free-standing before attaching to a module.
	// Lets Memory's src access resolve uniformly via module->design.
	RTLIL::Module *module = nullptr;

	// Context-aware src helpers. Resolve Design via module->design and
	// route to the per-Design meta vector; assert the memory is attached.
	IdString src_id() const;
	IdString src_ref() const { return src_id(); }
	void set_src_id(IdString id);
	void set_src_attribute(IdString src);
	std::string get_src_attribute() const;
	void adopt_src_from(const RTLIL::AttrObject *source);
	void absorb_attrs(dict<IdString, RTLIL::Const> &&buf);

	// Shadows meta_->name via a read-only masquerade, same contract as
	// Wire::name/Cell::name (resolves the Design through Memory::module).
	[[no_unique_address]] RTLIL::MemoryNameMasq name;

	int width, start_offset, size;
#ifdef YOSYS_ENABLE_PYTHON
	static std::map<unsigned int, RTLIL::Memory*> *get_all_memorys(void);
#endif

	std::string to_rtlil_str() const;
};

struct RTLIL::Cell : public RTLIL::AttrObject
{
private:
	struct ConstructToken { explicit ConstructToken() = default; };
	friend struct RTLIL::Module;

	// Push existing port connections into the bufnorm index after module assignment.
	// Assumes signals are already in normalized form.
	void initIndex();

	bool bufnorm_handle_setPort(IdString portname, RTLIL::SigSpec &signal, dict<IdString, RTLIL::SigSpec>::iterator conn_it);
public:
	// Shadows NamedObject::name. Reads materialise via twines; writes
	[[no_unique_address]] RTLIL::CellNameMasq name;

	Hasher::hash_t hashidx_;
	[[nodiscard]] Hasher hash_into(Hasher h) const { h.eat(hashidx_); return h; }

	// use module->addCell() and module->remove() to create or destroy cells
	Cell(ConstructToken);
	~Cell();

	// do not simply copy cells
	Cell(ConstructToken, RTLIL::Cell &other);
	void operator=(RTLIL::Cell &other) = delete;

	RTLIL::Module *module;
	IdString type_impl;
	[[no_unique_address]] RTLIL::CellTypeMasq type;
	dict<IdString, RTLIL::SigSpec> connections_;
	dict<IdString, RTLIL::Const> parameters;

	// Context-aware src helpers. Resolve Design via module->design and
	// route to the per-Design meta vector; assert the cell is attached.
	IdString src_id() const;
	IdString src_ref() const { return src_id(); }
	void set_src_id(IdString id);
	void set_src_attribute(IdString src);
	std::string get_src_attribute() const;
	void adopt_src_from(const RTLIL::AttrObject *source);
	void transfer_src_attribute(const RTLIL::AttrObject *source) { adopt_src_from(source); }
	void absorb_attrs(dict<IdString, RTLIL::Const> &&buf);

	// access cell ports
	bool hasPort(IdString portname) const;
	void unsetPort(IdString portname);
	void setPort(IdString portname, RTLIL::SigSpec signal);
	const RTLIL::SigSpec &getPort(IdString portname) const;
	const dict<IdString, RTLIL::SigSpec> &connections() const;

	// information about cell ports
	bool known() const;
	bool input(IdString portname) const;
	bool output(IdString portname) const;
	PortDir port_dir(IdString portname) const;

	// access cell parameters
	bool hasParam(IdString paramname) const;
	void unsetParam(IdString paramname);
	void setParam(IdString paramname, RTLIL::Const value);
	const RTLIL::Const &getParam(IdString paramname) const;

	void sort();
	void check();
	void fixup_parameters(bool set_a_signed = false, bool set_b_signed = false);

	bool has_keep_attr() const;

	template<typename T> void rewrite_sigspecs(T &functor);
	template<typename T> void rewrite_sigspecs2(T &functor);

#ifdef YOSYS_ENABLE_PYTHON
	static std::map<unsigned int, RTLIL::Cell*> *get_all_cells(void);
#endif

	bool has_memid() const;
	bool is_mem_cell() const;
	bool is_builtin_ff() const;

	std::string to_rtlil_str() const;
};

struct RTLIL::CaseRule : public RTLIL::AttrObject
{
	// Back-pointer to the owning module. Set by the frontend / kernel
	// attach path before any src access; the per-Design src meta vector
	// is resolved as `module->design`. Frontends that construct an
	// inner-process tree must call setModuleRecursive() on the root before
	// the tree is consumed (e.g. before set_src_attribute is invoked on
	// any nested CaseRule/SwitchRule/MemWriteAction).
	RTLIL::Module *module = nullptr;

	std::vector<RTLIL::SigSpec> compare;
	std::vector<RTLIL::SyncAction> actions;
	std::vector<RTLIL::SwitchRule*> switches;
	IdString compare_src = Twine::Null;

	~CaseRule();

	bool empty() const;

	// Walk the whole CaseRule subtree (this case, every switch, every
	// nested case, every MemWriteAction inside this process's sync rules
	// each. Idempotent.
	void setModuleRecursive(RTLIL::Module *m);

	// Context-aware src helpers via module->design.
	IdString src_id() const;
	IdString src_ref() const { return src_id(); }
	void set_src_id(IdString id);
	void set_src_attribute(IdString src);
	std::string get_src_attribute() const;
	void adopt_src_from(const RTLIL::AttrObject *source);
	void absorb_attrs(dict<IdString, RTLIL::Const> &&buf);

	template<typename T> void rewrite_sigspecs(T &functor);
	template<typename T> void rewrite_sigspecs2(T &functor);
	RTLIL::CaseRule *clone() const;
};

struct RTLIL::SwitchRule : public RTLIL::AttrObject
{
	// Back-pointer to the owning module; see CaseRule::module.
	RTLIL::Module *module = nullptr;

	RTLIL::SigSpec signal;
	IdString signal_src = Twine::Null;
	std::vector<RTLIL::CaseRule*> cases;

	~SwitchRule();

	bool empty() const;

	void setModuleRecursive(RTLIL::Module *m);

	// Context-aware src helpers via module->design.
	IdString src_id() const;
	IdString src_ref() const { return src_id(); }
	void set_src_id(IdString id);
	void set_src_attribute(IdString src);
	std::string get_src_attribute() const;
	void adopt_src_from(const RTLIL::AttrObject *source);
	void absorb_attrs(dict<IdString, RTLIL::Const> &&buf);

	template<typename T> void rewrite_sigspecs(T &functor);
	template<typename T> void rewrite_sigspecs2(T &functor);
	RTLIL::SwitchRule *clone() const;
};

struct RTLIL::MemWriteAction : RTLIL::AttrObject
{
	// Back-pointer to the owning module; see CaseRule::module.
	RTLIL::Module *module = nullptr;

	IdString memid;
	RTLIL::SigSpec address;
	RTLIL::SigSpec data;
	RTLIL::SigSpec enable;
	RTLIL::Const priority_mask;

	// Context-aware src helpers via module->design.
	IdString src_id() const;
	IdString src_ref() const { return src_id(); }
	void set_src_id(IdString id);
	void set_src_attribute(IdString src);
	std::string get_src_attribute() const;
	void adopt_src_from(const RTLIL::AttrObject *source);
	void absorb_attrs(dict<IdString, RTLIL::Const> &&buf);
};

struct RTLIL::SyncAction
{
	RTLIL::SigSpec lhs;
	RTLIL::SigSpec rhs;
	IdString src = Twine::Null;
};

struct RTLIL::SyncRule
{
	RTLIL::SyncType type;
	RTLIL::SigSpec signal;
	std::vector<RTLIL::SyncAction> actions;
	std::vector<RTLIL::MemWriteAction> mem_write_actions;

	template<typename T> void rewrite_sigspecs(T &functor);
	template<typename T> void rewrite_sigspecs2(T &functor);
	RTLIL::SyncRule *clone() const;
};

struct RTLIL::Process : public RTLIL::AttrObject
{
	friend struct RTLIL::Cell;
	friend struct RTLIL::Design;

	Hasher::hash_t hashidx_;
	[[nodiscard]] Hasher hash_into(Hasher h) const { h.eat(hashidx_); return h; }

protected:
	// use module->addProcess() and module->remove() to create or destroy processes
	friend struct RTLIL::Module;
	Process();
	~Process();

public:
	RTLIL::Module *module;
	RTLIL::CaseRule root_case;
	std::vector<RTLIL::SyncRule*> syncs;

	// Context-aware src helpers. Resolve Design via module->design and
	// route to the per-Design meta vector; assert the process is attached.
	IdString src_id() const;
	IdString src_ref() const { return src_id(); }
	void set_src_id(IdString id);
	void set_src_attribute(IdString src);
	std::string get_src_attribute() const;
	void adopt_src_from(const RTLIL::AttrObject *source);
	void absorb_attrs(dict<IdString, RTLIL::Const> &&buf);

	// Shadows meta_->name via a read-only masquerade, same contract as
	// Wire::name/Cell::name (resolves the Design through Process::module).
	[[no_unique_address]] RTLIL::ProcessNameMasq name;

	template<typename T> void rewrite_sigspecs(T &functor);
	template<typename T> void rewrite_sigspecs2(T &functor);
	RTLIL::Process *clone() const;

	std::string to_rtlil_str() const;
};

inline RTLIL::SigBit::SigBit() : wire(NULL), data(RTLIL::State::S0) { }
inline RTLIL::SigBit::SigBit(RTLIL::State bit) : wire(NULL), data(bit) { }
inline RTLIL::SigBit::SigBit(bool bit) : wire(NULL), data(bit ? State::S1 : State::S0) { }
inline RTLIL::SigBit::SigBit(RTLIL::Wire *wire) : wire(wire), offset(0) { log_assert(wire && wire->width == 1); }
inline RTLIL::SigBit::SigBit(RTLIL::Wire *wire, int offset) : wire(wire), offset(offset) { log_assert(wire != nullptr); }
inline RTLIL::SigBit::SigBit(const RTLIL::SigChunk &chunk) : wire(chunk.wire) { log_assert(chunk.width == 1); if (wire) offset = chunk.offset; else data = chunk.data[0]; }
inline RTLIL::SigBit::SigBit(const RTLIL::SigChunk &chunk, int index) : wire(chunk.wire) { if (wire) offset = chunk.offset + index; else data = chunk.data[index]; }

inline bool RTLIL::SigBit::operator<(const RTLIL::SigBit &other) const {
	if (wire == other.wire)
		return wire ? (offset < other.offset) : (data < other.data);
	if (wire != nullptr && other.wire != nullptr)
		return wire->name < other.wire->name;
	return (wire != nullptr) < (other.wire != nullptr);
}

inline bool RTLIL::SigBit::operator==(const RTLIL::SigBit &other) const {
	return (wire == other.wire) && (wire ? (offset == other.offset) : (data == other.data));
}

inline bool RTLIL::SigBit::operator!=(const RTLIL::SigBit &other) const {
	return (wire != other.wire) || (wire ? (offset != other.offset) : (data != other.data));
}

inline Hasher RTLIL::SigBit::hash_into(Hasher h) const {
	if (wire) {
		h.eat(offset);
		h.eat(wire->name);
		return h;
	}
	h.eat(data);
	return h;
}


inline Hasher RTLIL::SigBit::hash_top() const {
	Hasher h;
	if (wire) {
		IdString name = wire->meta_ ? wire->meta_->name : Twine::Null;
		uint32_t n = (uint32_t)name.value ^ (uint32_t)(name.value >> 32);
		// This hashing trick is optimized for dense integers
		// where the second integer is usually only up to 32 large
		// which fits the offset of a wire. Better performance than
		// just calling h.eat on the two fields in sequence!
		// Do not mess this up again!
		h.force(hashlib::legacy::djb2_add(n, offset));
		return h;
	}
	h.force(data);
	return h;
}

inline RTLIL::SigBit &RTLIL::SigSpecIterator::operator*() const {
	return (*sig_p)[index];
}

inline const RTLIL::SigBit &RTLIL::SigSpecConstIterator::operator*() {
	bit = (*sig_p)[index];
	return bit;
}

inline RTLIL::SigBit::SigBit(const RTLIL::SigSpec &sig) {
	log_assert(sig.size() == 1);
	auto it = sig.chunks().begin();
	*this = SigBit(*it);
}

template<typename Derived>
class CellAdderMixin {
public:
	// The add* methods create a cell and return the created cell. All signals must exist in advance.

	RTLIL::Cell* addNot (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addPos (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addBuf (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addNeg (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::Cell* addAnd  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addOr   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addXor  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addXnor (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::Cell* addReduceAnd  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addReduceOr   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addReduceXor  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addReduceXnor (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addReduceBool (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::Cell* addShl    (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addShr    (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addSshl   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addSshr   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addShift  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addShiftx (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::Cell* addLt  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addLe  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addEq  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addNe  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addEqx (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addNex (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addGe  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addGt  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::Cell* addAdd (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addSub (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addMul (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	// truncating division
	RTLIL::Cell* addDiv (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	// truncating modulo
	RTLIL::Cell* addMod (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addDivFloor (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addModFloor (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addPow (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool a_signed = false, bool b_signed = false, IdString src = Twine::Null);

	RTLIL::Cell* addFa (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_c, const RTLIL::SigSpec &sig_x, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);

	RTLIL::Cell* addLogicNot (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addLogicAnd (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::Cell* addLogicOr  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::Cell* addMux  (IdString name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addMux  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addPmux (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addBmux (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addDemux (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);

	RTLIL::Cell* addBweqx  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addBwmux  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);

	RTLIL::Cell* addSlice  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, RTLIL::Const offset, IdString src = Twine::Null);
	RTLIL::Cell* addConcat (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addLut    (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_y, RTLIL::Const lut, IdString src = Twine::Null);
	RTLIL::Cell* addTribuf (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addAssert (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_en, IdString src = Twine::Null);
	RTLIL::Cell* addAssume (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_en, IdString src = Twine::Null);
	RTLIL::Cell* addLive   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_en, IdString src = Twine::Null);
	RTLIL::Cell* addFair   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_en, IdString src = Twine::Null);
	RTLIL::Cell* addCover  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_en, IdString src = Twine::Null);
	RTLIL::Cell* addEquiv  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);

	RTLIL::Cell* addSr    (IdString name, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr, const RTLIL::SigSpec &sig_q, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addSr    (Twine &&name, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr, const RTLIL::SigSpec &sig_q, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null)
		{ return addSr(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_set, sig_clr, sig_q, set_polarity, clr_polarity, src); }
	RTLIL::Cell* addFf    (IdString name, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, IdString src = Twine::Null);

	RTLIL::Cell* addFf    (Twine &&name, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, IdString src = Twine::Null)
		{ return addFf(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_d, sig_q, src); }
	RTLIL::Cell* addDff   (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_d,   const RTLIL::SigSpec &sig_q, bool clk_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDff   (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_d,   const RTLIL::SigSpec &sig_q, bool clk_polarity = true, IdString src = Twine::Null)
		{ return addDff(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_d, sig_q, clk_polarity, src); }
	RTLIL::Cell* addDffe  (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en,  const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool en_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDffe  (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en,  const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool en_polarity = true, IdString src = Twine::Null)
		{ return addDffe(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_d, sig_q, clk_polarity, en_polarity, src); }
	RTLIL::Cell* addDffsr (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr, RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDffsr (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr, RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null)
		{ return addDffsr(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_set, sig_clr, sig_d, sig_q, clk_polarity, set_polarity, clr_polarity, src); }
	RTLIL::Cell* addDffsre (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr, RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool en_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDffsre (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr, RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool en_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null)
		{ return addDffsre(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_set, sig_clr, sig_d, sig_q, clk_polarity, en_polarity, set_polarity, clr_polarity, src); }
	RTLIL::Cell* addAdff (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const arst_value, bool clk_polarity = true, bool arst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAdff (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const arst_value, bool clk_polarity = true, bool arst_polarity = true, IdString src = Twine::Null)
		{ return addAdff(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_arst, sig_d, sig_q, arst_value, clk_polarity, arst_polarity, src); }
	RTLIL::Cell* addAdffe (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_arst,  const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const arst_value, bool clk_polarity = true, bool en_polarity = true, bool arst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAdffe (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_arst,  const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const arst_value, bool clk_polarity = true, bool en_polarity = true, bool arst_polarity = true, IdString src = Twine::Null)
		{ return addAdffe(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_arst, sig_d, sig_q, arst_value, clk_polarity, en_polarity, arst_polarity, src); }
	RTLIL::Cell* addAldff (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_aload, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, const RTLIL::SigSpec &sig_ad, bool clk_polarity = true, bool aload_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAldff (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_aload, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, const RTLIL::SigSpec &sig_ad, bool clk_polarity = true, bool aload_polarity = true, IdString src = Twine::Null)
		{ return addAldff(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_aload, sig_d, sig_q, sig_ad, clk_polarity, aload_polarity, src); }
	RTLIL::Cell* addAldffe (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_aload,  const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, const RTLIL::SigSpec &sig_ad, bool clk_polarity = true, bool en_polarity = true, bool aload_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAldffe (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_aload,  const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, const RTLIL::SigSpec &sig_ad, bool clk_polarity = true, bool en_polarity = true, bool aload_polarity = true, IdString src = Twine::Null)
		{ return addAldffe(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_aload, sig_d, sig_q, sig_ad, clk_polarity, en_polarity, aload_polarity, src); }
	RTLIL::Cell* addSdff (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const srst_value, bool clk_polarity = true, bool srst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addSdff (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const srst_value, bool clk_polarity = true, bool srst_polarity = true, IdString src = Twine::Null)
		{ return addSdff(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_srst, sig_d, sig_q, srst_value, clk_polarity, srst_polarity, src); }
	RTLIL::Cell* addSdffe (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_srst,  const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const srst_value, bool clk_polarity = true, bool en_polarity = true, bool srst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addSdffe (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_srst,  const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const srst_value, bool clk_polarity = true, bool en_polarity = true, bool srst_polarity = true, IdString src = Twine::Null)
		{ return addSdffe(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_srst, sig_d, sig_q, srst_value, clk_polarity, en_polarity, srst_polarity, src); }
	RTLIL::Cell* addSdffce (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const srst_value, bool clk_polarity = true, bool en_polarity = true, bool srst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addSdffce (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const srst_value, bool clk_polarity = true, bool en_polarity = true, bool srst_polarity = true, IdString src = Twine::Null)
		{ return addSdffce(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_srst, sig_d, sig_q, srst_value, clk_polarity, en_polarity, srst_polarity, src); }
	RTLIL::Cell* addDlatch (IdString name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool en_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDlatch (Twine &&name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool en_polarity = true, IdString src = Twine::Null)
		{ return addDlatch(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_en, sig_d, sig_q, en_polarity, src); }
	RTLIL::Cell* addAdlatch (IdString name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const arst_value, bool en_polarity = true, bool arst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAdlatch (Twine &&name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, RTLIL::Const arst_value, bool en_polarity = true, bool arst_polarity = true, IdString src = Twine::Null)
		{ return addAdlatch(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_en, sig_arst, sig_d, sig_q, arst_value, en_polarity, arst_polarity, src); }
	RTLIL::Cell* addDlatchsr (IdString name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr, RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool en_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDlatchsr (Twine &&name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr, RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool en_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null)
		{ return addDlatchsr(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_en, sig_set, sig_clr, sig_d, sig_q, en_polarity, set_polarity, clr_polarity, src); }

	RTLIL::Cell* addBufGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addNotGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addAndGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addNandGate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addOrGate     (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addNorGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addXorGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addXnorGate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addAndnotGate (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addOrnotGate  (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addMuxGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_s, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addNmuxGate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_s, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addAoi3Gate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_c, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addOai3Gate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_c, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addAoi4Gate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_c, const RTLIL::SigBit &sig_d, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);
	RTLIL::Cell* addOai4Gate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_c, const RTLIL::SigBit &sig_d, const RTLIL::SigBit &sig_y, IdString src = Twine::Null);

	RTLIL::Cell* addSrGate     (IdString name, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr,
			const RTLIL::SigSpec &sig_q, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addSrGate     (Twine &&name, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr,
			const RTLIL::SigSpec &sig_q, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null)
		{ return addSrGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_set, sig_clr, sig_q, set_polarity, clr_polarity, src); }
	RTLIL::Cell* addFfGate     (IdString name, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, IdString src = Twine::Null);

	RTLIL::Cell* addFfGate     (Twine &&name, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, IdString src = Twine::Null)
		{ return addFfGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_d, sig_q, src); }
	RTLIL::Cell* addDffGate    (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDffGate    (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, IdString src = Twine::Null)
		{ return addDffGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_d, sig_q, clk_polarity, src); }
	RTLIL::Cell* addDffeGate   (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool en_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDffeGate   (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool en_polarity = true, IdString src = Twine::Null)
		{ return addDffeGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_d, sig_q, clk_polarity, en_polarity, src); }
	RTLIL::Cell* addDffsrGate  (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr,
			RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDffsrGate  (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr,
			RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null)
		{ return addDffsrGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_set, sig_clr, sig_d, sig_q, clk_polarity, set_polarity, clr_polarity, src); }
	RTLIL::Cell* addDffsreGate (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr,
			RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool en_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDffsreGate (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr,
			RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool clk_polarity = true, bool en_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null)
		{ return addDffsreGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_set, sig_clr, sig_d, sig_q, clk_polarity, en_polarity, set_polarity, clr_polarity, src); }
	RTLIL::Cell* addAdffGate   (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool arst_value = false, bool clk_polarity = true, bool arst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAdffGate   (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool arst_value = false, bool clk_polarity = true, bool arst_polarity = true, IdString src = Twine::Null)
		{ return addAdffGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_arst, sig_d, sig_q, arst_value, clk_polarity, arst_polarity, src); }
	RTLIL::Cell* addAdffeGate  (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool arst_value = false, bool clk_polarity = true, bool en_polarity = true, bool arst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAdffeGate  (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool arst_value = false, bool clk_polarity = true, bool en_polarity = true, bool arst_polarity = true, IdString src = Twine::Null)
		{ return addAdffeGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_arst, sig_d, sig_q, arst_value, clk_polarity, en_polarity, arst_polarity, src); }
	RTLIL::Cell* addAldffGate   (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_aload, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			const RTLIL::SigSpec &sig_ad, bool clk_polarity = true, bool aload_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAldffGate   (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_aload, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			const RTLIL::SigSpec &sig_ad, bool clk_polarity = true, bool aload_polarity = true, IdString src = Twine::Null)
		{ return addAldffGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_aload, sig_d, sig_q, sig_ad, clk_polarity, aload_polarity, src); }
	RTLIL::Cell* addAldffeGate  (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_aload, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			const RTLIL::SigSpec &sig_ad, bool clk_polarity = true, bool en_polarity = true, bool aload_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAldffeGate  (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_aload, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			const RTLIL::SigSpec &sig_ad, bool clk_polarity = true, bool en_polarity = true, bool aload_polarity = true, IdString src = Twine::Null)
		{ return addAldffeGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_aload, sig_d, sig_q, sig_ad, clk_polarity, en_polarity, aload_polarity, src); }
	RTLIL::Cell* addSdffGate   (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool srst_value = false, bool clk_polarity = true, bool srst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addSdffGate   (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool srst_value = false, bool clk_polarity = true, bool srst_polarity = true, IdString src = Twine::Null)
		{ return addSdffGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_srst, sig_d, sig_q, srst_value, clk_polarity, srst_polarity, src); }
	RTLIL::Cell* addSdffeGate  (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool srst_value = false, bool clk_polarity = true, bool en_polarity = true, bool srst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addSdffeGate  (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool srst_value = false, bool clk_polarity = true, bool en_polarity = true, bool srst_polarity = true, IdString src = Twine::Null)
		{ return addSdffeGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_srst, sig_d, sig_q, srst_value, clk_polarity, en_polarity, srst_polarity, src); }
	RTLIL::Cell* addSdffceGate (IdString name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool srst_value = false, bool clk_polarity = true, bool en_polarity = true, bool srst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addSdffceGate (Twine &&name, const RTLIL::SigSpec &sig_clk, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_srst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool srst_value = false, bool clk_polarity = true, bool en_polarity = true, bool srst_polarity = true, IdString src = Twine::Null)
		{ return addSdffceGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_clk, sig_en, sig_srst, sig_d, sig_q, srst_value, clk_polarity, en_polarity, srst_polarity, src); }
	RTLIL::Cell* addDlatchGate (IdString name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool en_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDlatchGate (Twine &&name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, bool en_polarity = true, IdString src = Twine::Null)
		{ return addDlatchGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_en, sig_d, sig_q, en_polarity, src); }
	RTLIL::Cell* addAdlatchGate(IdString name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool arst_value = false, bool en_polarity = true, bool arst_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addAdlatchGate(Twine &&name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_arst, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q,
			bool arst_value = false, bool en_polarity = true, bool arst_polarity = true, IdString src = Twine::Null)
		{ return addAdlatchGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_en, sig_arst, sig_d, sig_q, arst_value, en_polarity, arst_polarity, src); }
	RTLIL::Cell* addDlatchsrGate  (IdString name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr,
			RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool en_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null);

	RTLIL::Cell* addDlatchsrGate  (Twine &&name, const RTLIL::SigSpec &sig_en, const RTLIL::SigSpec &sig_set, const RTLIL::SigSpec &sig_clr,
			RTLIL::SigSpec sig_d, const RTLIL::SigSpec &sig_q, bool en_polarity = true, bool set_polarity = true, bool clr_polarity = true, IdString src = Twine::Null)
		{ return addDlatchsrGate(static_cast<Derived*>(this)->design->twines.add(std::move(name)), sig_en, sig_set, sig_clr, sig_d, sig_q, en_polarity, set_polarity, clr_polarity, src); }

	// The methods without the add* prefix create a cell and an output signal. They return the newly created output signal.

	RTLIL::SigSpec Not (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Pos (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Buf (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Neg (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::SigSpec And  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Or   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Xor  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Xnor (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::SigSpec ReduceAnd  (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec ReduceOr   (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec ReduceXor  (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec ReduceXnor (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec ReduceBool (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::SigSpec Shl    (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Shr    (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Sshl   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Sshr   (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Shift  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Shiftx (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::SigSpec Lt  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Le  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Eq  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Ne  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Eqx (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Nex (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Ge  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Gt  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::SigSpec Add (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Sub (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Mul (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	// truncating division
	RTLIL::SigSpec Div (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	// truncating modulo
	RTLIL::SigSpec Mod (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec DivFloor (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec ModFloor (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec Pow (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool a_signed = false, bool b_signed = false, IdString src = Twine::Null);

	RTLIL::SigSpec LogicNot (Twine &&name, const RTLIL::SigSpec &sig_a, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec LogicAnd (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);
	RTLIL::SigSpec LogicOr  (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, bool is_signed = false, IdString src = Twine::Null);

	RTLIL::SigSpec Mux      (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_s, IdString src = Twine::Null);
	RTLIL::SigSpec Pmux     (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_s, IdString src = Twine::Null);
	RTLIL::SigSpec Bmux     (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, IdString src = Twine::Null);
	RTLIL::SigSpec Demux     (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, IdString src = Twine::Null);

	RTLIL::SigSpec Bweqx      (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, IdString src = Twine::Null);
	RTLIL::SigSpec Bwmux      (Twine &&name, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_b, const RTLIL::SigSpec &sig_s, IdString src = Twine::Null);

	RTLIL::SigBit BufGate    (Twine &&name, const RTLIL::SigBit &sig_a, IdString src = Twine::Null);
	RTLIL::SigBit NotGate    (Twine &&name, const RTLIL::SigBit &sig_a, IdString src = Twine::Null);
	RTLIL::SigBit AndGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, IdString src = Twine::Null);
	RTLIL::SigBit NandGate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, IdString src = Twine::Null);
	RTLIL::SigBit OrGate     (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, IdString src = Twine::Null);
	RTLIL::SigBit NorGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, IdString src = Twine::Null);
	RTLIL::SigBit XorGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, IdString src = Twine::Null);
	RTLIL::SigBit XnorGate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, IdString src = Twine::Null);
	RTLIL::SigBit AndnotGate (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, IdString src = Twine::Null);
	RTLIL::SigBit OrnotGate  (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, IdString src = Twine::Null);
	RTLIL::SigBit MuxGate    (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_s, IdString src = Twine::Null);
	RTLIL::SigBit NmuxGate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_s, IdString src = Twine::Null);
	RTLIL::SigBit Aoi3Gate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_c, IdString src = Twine::Null);
	RTLIL::SigBit Oai3Gate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_c, IdString src = Twine::Null);
	RTLIL::SigBit Aoi4Gate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_c, const RTLIL::SigBit &sig_d, IdString src = Twine::Null);
	RTLIL::SigBit Oai4Gate   (Twine &&name, const RTLIL::SigBit &sig_a, const RTLIL::SigBit &sig_b, const RTLIL::SigBit &sig_c, const RTLIL::SigBit &sig_d, IdString src = Twine::Null);
};

struct RTLIL::Module : public RTLIL::AttrObject, public CellAdderMixin<RTLIL::Module>
{
	friend struct RTLIL::Cell;
	friend struct RTLIL::Design;

	[[no_unique_address]] RTLIL::ModuleNameMasq name;

	Hasher::hash_t hashidx_;
	[[nodiscard]] Hasher hash_into(Hasher h) const { h.eat(hashidx_); return h; }

protected:
	void add(RTLIL::Wire *wire);
	void add(RTLIL::Cell *cell);
	void add(RTLIL::Process *process);

public:
	RTLIL::Design *design;
	pool<RTLIL::Monitor*> monitors;

	int refcount_wires_;
	int refcount_cells_;

	dict<IdString, RTLIL::Wire*> wires_;
	dict<IdString, RTLIL::Cell*> cells_;

	std::vector<RTLIL::SigSig>   connections_;
	std::vector<RTLIL::Binding*> bindings_;

	idict<IdString> avail_parameters;
	dict<IdString, RTLIL::Const> parameter_default_values;
	dict<IdString, RTLIL::Memory*> memories;
	dict<IdString, RTLIL::Process*> processes;

	// Context-aware src helpers. Resolve Design via this->design and
	// route to the per-Design meta vector; assert the module is attached.
	IdString src_id() const;
	IdString src_ref() const { return src_id(); }
	void set_src_id(IdString id);
	void set_src_attribute(IdString src);
	std::string get_src_attribute() const;
	void adopt_src_from(const RTLIL::AttrObject *source);
	void absorb_attrs(dict<IdString, RTLIL::Const> &&buf);

	Module();
	virtual ~Module();
	virtual IdString derive(RTLIL::Design *design, const dict<IdString, RTLIL::Const> &parameters, bool mayfail = false);
	virtual IdString derive(RTLIL::Design *design, const dict<IdString, RTLIL::Const> &parameters, const dict<IdString, RTLIL::Module*> &interfaces, const dict<IdString, IdString> &modports, bool mayfail = false);
	virtual size_t count_id(IdString id);
	virtual void expand_interfaces(RTLIL::Design *design, const dict<IdString, RTLIL::Module *> &local_interfaces);
	virtual bool reprocess_if_necessary(RTLIL::Design *design);

	virtual void sort();
	virtual void check();
	virtual void optimize();
	virtual void makeblackbox();

	bool get_blackbox_attribute(bool ignore_wb=false) const {
		return get_bool_attribute(ID::blackbox) || (!ignore_wb && get_bool_attribute(ID::whitebox));
	}

	void connect(const RTLIL::SigSig &conn);
	void connect(const RTLIL::SigSpec &lhs, const RTLIL::SigSpec &rhs);
	void new_connections(const std::vector<RTLIL::SigSig> &new_conn);
	const std::vector<RTLIL::SigSig> &connections() const;

	std::vector<IdString> ports;
	void fixup_ports();

	pool<RTLIL::Cell *> buf_norm_cell_queue;
	pool<pair<RTLIL::Cell *, IdString>> buf_norm_cell_port_queue;
	pool<RTLIL::Wire *> buf_norm_wire_queue;
	pool<RTLIL::Cell *> pending_deleted_cells;
	dict<RTLIL::Wire *, pool<RTLIL::Cell *>> buf_norm_connect_index;
	void bufNormalize();

	template<typename T> void rewrite_sigspecs(T &functor);
	template<typename T> void rewrite_sigspecs2(T &functor);
	// `src_id_verbatim`: when true, the caller guarantees that
	// `new_mod->design->twines` is a verbatim copy of
	// `this->design->twines`, so src_id_ values can be transferred
	// without retain/release on the destination pool (the copied refcounts
	// already account for the new AttrObject references). Used by
	// Design::clone_into for wholesale design copies.
	void cloneInto(RTLIL::Module *new_mod, bool src_id_verbatim = false) const;
	virtual RTLIL::Module *clone() const;
	// Clone variant that attaches the new module to `dst` BEFORE cloneInto
	// runs. This is the right pattern when the destination design is known
	// the pending-literal src stashing it entails. Subtypes override to
	// preserve their type (AstModule). `src_id_verbatim` is forwarded to
	// cloneInto.
	virtual RTLIL::Module *clone(RTLIL::Design *dst, bool src_id_verbatim = false) const;
	// As above, but additionally renames the new module to `target_name` in
	// `dst`. Used when source and destination designs may contain modules
	// with the same name and the new one must take a different identity.
	virtual RTLIL::Module *clone(RTLIL::Design *dst, IdString target_name, bool src_id_verbatim = false) const;

	bool has_memories() const;
	bool has_processes() const;

	bool has_memories_warn() const;
	bool has_processes_warn() const;

	bool is_selected() const;
	bool is_selected_whole() const;

	std::vector<RTLIL::Wire*> selected_wires() const;
	std::vector<RTLIL::Cell*> selected_cells() const;
	std::vector<RTLIL::Memory*> selected_memories() const;
	std::vector<RTLIL::Process*> selected_processes() const;
	std::vector<RTLIL::AttrObject*> selected_members() const;

	template<typename T> bool selected(T *member) const {
		return design->selected_member(meta_->name, member->meta_->name);
	}

	RTLIL::Wire* wire(IdString id) {
		auto it = wires_.find(id);
		return it == wires_.end() ? nullptr : it->second;
	}
	RTLIL::Cell* cell(IdString id) {
		auto it = cells_.find(id);
		return it == cells_.end() ? nullptr : it->second;
	}
	const RTLIL::Wire* wire(IdString id) const {
		auto it = wires_.find(id);
		return it == wires_.end() ? nullptr : it->second;
	}
	const RTLIL::Cell* cell(IdString id) const {
		auto it = cells_.find(id);
		return it == cells_.end() ? nullptr : it->second;
	}

	RTLIL::ObjRange<RTLIL::Wire*, IdString> wires() { return RTLIL::ObjRange<RTLIL::Wire*, IdString>(&wires_, &refcount_wires_); }
	int wires_size() const { return wires_.size(); }
	RTLIL::Wire* wire_at(int index) const { return wires_.element(index)->second; }
	RTLIL::ObjRange<RTLIL::Cell*, IdString> cells() { return RTLIL::ObjRange<RTLIL::Cell*, IdString>(&cells_, &refcount_cells_); }
	int cells_size() const { return cells_.size(); }
	RTLIL::Cell* cell_at(int index) const { return cells_.element(index)->second; }

	void add(RTLIL::Binding *binding);

	// Removing wires is expensive. If you have to remove wires, remove them all at once.
	void remove(const pool<RTLIL::Wire*> &wires);
	void remove(RTLIL::Cell *cell);
	void remove(RTLIL::Memory *memory);
	void remove(RTLIL::Process *process);

	void rename(RTLIL::Wire *wire, IdString new_name);
	void rename(RTLIL::Cell *cell, IdString new_name);
	void rename(IdString old_name, IdString new_name);

	void swap_names(RTLIL::Wire *w1, RTLIL::Wire *w2);
	void swap_names(RTLIL::Cell *c1, RTLIL::Cell *c2);

	IdString uniquify(IdString name);
	IdString uniquify(Twine&& name);
	IdString uniquify(IdString name, int &index);
	IdString uniquify(Twine&& name, int &index);

	// Primary overloads: name already interned in design->twines.
	RTLIL::Wire *addWire(IdString name, int width = 1);
	RTLIL::Wire *addWire(IdString name, const RTLIL::Wire *other);
	// Convenience: adds name into twines, then dispatches.
	RTLIL::Wire *addWire(Twine &&name, int width = 1);
	RTLIL::Wire *addWire(Twine &&name, const RTLIL::Wire *other);

	// Primary overloads.
	RTLIL::Cell *addCell(IdString name, IdString type);
	RTLIL::Cell *addCell(IdString name, const RTLIL::Cell *other);
	// Convenience.
	RTLIL::Cell *addCell(Twine name, Twine type);
	RTLIL::Cell *addCell(Twine &&name, IdString type);
	RTLIL::Cell *addCell(IdString name, Twine &&type);
	RTLIL::Cell *addCell(Twine &&name, const RTLIL::Cell *other);

	// CellAdderMixin hook: cells added here are attached, so set src directly.
	void cell_set_src(RTLIL::Cell *cell, IdString src) { cell->set_src_attribute(src); }

	// NEW_ID analog for twine names; see NEW_ID in yosys_common.h.
	IdString new_name(const std::string *prefix) {
		IdString pref = design->twines.add(Twine{*prefix});
		return design->twines.add(Twine{Twine::Suffix{pref, std::to_string(autoidx++)}});
	}

	RTLIL::Memory *addMemory(IdString name);
	RTLIL::Memory *addMemory(Twine &&name);
	RTLIL::Memory *addMemory(IdString name, const RTLIL::Memory *other);

	RTLIL::Process *addProcess(IdString name);
	RTLIL::Process *addProcess(Twine &&name);
	RTLIL::Process *addProcess(IdString name, const RTLIL::Process *other);

	// The add* methods create a cell and return the created cell. All signals must exist in advance.

	RTLIL::Cell* addAnyinit(IdString name, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, IdString src = Twine::Null);
	RTLIL::Cell* addAnyinit(Twine &&name, const RTLIL::SigSpec &sig_d, const RTLIL::SigSpec &sig_q, IdString src = Twine::Null);

	// The methods without the add* prefix create a cell and an output signal. They return the newly created output signal.

	RTLIL::SigSpec Anyconst  (IdString name, int width = 1, IdString src = Twine::Null);
	RTLIL::SigSpec Anyseq    (IdString name, int width = 1, IdString src = Twine::Null);
	RTLIL::SigSpec Allconst  (IdString name, int width = 1, IdString src = Twine::Null);
	RTLIL::SigSpec Allseq    (IdString name, int width = 1, IdString src = Twine::Null);
	RTLIL::SigSpec Initstate (IdString name, IdString src = Twine::Null);
	RTLIL::SigSpec Anyconst  (Twine &&name, int width = 1, IdString src = Twine::Null);
	RTLIL::SigSpec Anyseq    (Twine &&name, int width = 1, IdString src = Twine::Null);
	RTLIL::SigSpec Allconst  (Twine &&name, int width = 1, IdString src = Twine::Null);
	RTLIL::SigSpec Allseq    (Twine &&name, int width = 1, IdString src = Twine::Null);
	RTLIL::SigSpec Initstate (Twine &&name, IdString src = Twine::Null);

	RTLIL::SigSpec SetTag          (IdString name, const std::string &tag, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_c, IdString src = Twine::Null);
	RTLIL::Cell*   addSetTag       (IdString name, const std::string &tag, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_c, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::SigSpec GetTag          (IdString name, const std::string &tag, const RTLIL::SigSpec &sig_a, IdString src = Twine::Null);
	RTLIL::Cell*   addOverwriteTag (IdString name, const std::string &tag, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_c, IdString src = Twine::Null);
	RTLIL::SigSpec OriginalTag     (IdString name, const std::string &tag, const RTLIL::SigSpec &sig_a, IdString src = Twine::Null);
	RTLIL::SigSpec FutureFF        (IdString name, const RTLIL::SigSpec &sig_e, IdString src = Twine::Null);
	RTLIL::SigSpec SetTag          (Twine &&name, const std::string &tag, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_c, IdString src = Twine::Null);
	RTLIL::Cell*   addSetTag       (Twine &&name, const std::string &tag, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_c, const RTLIL::SigSpec &sig_y, IdString src = Twine::Null);
	RTLIL::SigSpec GetTag          (Twine &&name, const std::string &tag, const RTLIL::SigSpec &sig_a, IdString src = Twine::Null);
	RTLIL::Cell*   addOverwriteTag (Twine &&name, const std::string &tag, const RTLIL::SigSpec &sig_a, const RTLIL::SigSpec &sig_s, const RTLIL::SigSpec &sig_c, IdString src = Twine::Null);
	RTLIL::SigSpec OriginalTag     (Twine &&name, const std::string &tag, const RTLIL::SigSpec &sig_a, IdString src = Twine::Null);
	RTLIL::SigSpec FutureFF        (Twine &&name, const RTLIL::SigSpec &sig_e, IdString src = Twine::Null);

	std::string to_rtlil_str() const;
#ifdef YOSYS_ENABLE_PYTHON
	static std::map<unsigned int, RTLIL::Module*> *get_all_modules(void);
#endif
};

template<typename T>
void RTLIL::Module::rewrite_sigspecs(T &functor)
{
	for (auto &it : cells_)
		it.second->rewrite_sigspecs(functor);
	for (auto &it : processes)
		it.second->rewrite_sigspecs(functor);
	for (auto &it : connections_) {
		functor(it.first);
		functor(it.second);
	}
}

template<typename T>
void RTLIL::Module::rewrite_sigspecs2(T &functor)
{
	for (auto &it : cells_)
		it.second->rewrite_sigspecs2(functor);
	for (auto &it : processes)
		it.second->rewrite_sigspecs2(functor);
	for (auto &it : connections_) {
		functor(it.first, it.second);
	}
}

template<typename T>
void RTLIL::Cell::rewrite_sigspecs(T &functor) {
	for (auto &it : connections_)
		functor(it.second);
}

template<typename T>
void RTLIL::Cell::rewrite_sigspecs2(T &functor) {
	for (auto &it : connections_)
		functor(it.second);
}

template<typename T>
void RTLIL::CaseRule::rewrite_sigspecs(T &functor) {
	for (auto &it : compare)
		functor(it);
	for (auto &it : actions) {
		functor(it.lhs);
		functor(it.rhs);
	}
	for (auto it : switches)
		it->rewrite_sigspecs(functor);
}

template<typename T>
void RTLIL::CaseRule::rewrite_sigspecs2(T &functor) {
	for (auto &it : compare)
		functor(it);
	for (auto &it : actions) {
		functor(it.lhs, it.rhs);
	}
	for (auto it : switches)
		it->rewrite_sigspecs2(functor);
}

template<typename T>
void RTLIL::SwitchRule::rewrite_sigspecs(T &functor)
{
	functor(signal);
	for (auto it : cases)
		it->rewrite_sigspecs(functor);
}

template<typename T>
void RTLIL::SwitchRule::rewrite_sigspecs2(T &functor)
{
	functor(signal);
	for (auto it : cases)
		it->rewrite_sigspecs2(functor);
}

template<typename T>
void RTLIL::SyncRule::rewrite_sigspecs(T &functor)
{
	functor(signal);
	for (auto &it : actions) {
		functor(it.lhs);
		functor(it.rhs);
	}
	for (auto &it : mem_write_actions) {
		functor(it.address);
		functor(it.data);
		functor(it.enable);
	}
}

template<typename T>
void RTLIL::SyncRule::rewrite_sigspecs2(T &functor)
{
	functor(signal);
	for (auto &it : actions) {
		functor(it.lhs, it.rhs);
	}
	for (auto &it : mem_write_actions) {
		functor(it.address);
		functor(it.data);
		functor(it.enable);
	}
}

template<typename T>
void RTLIL::Process::rewrite_sigspecs(T &functor)
{
	root_case.rewrite_sigspecs(functor);
	for (auto it : syncs)
		it->rewrite_sigspecs(functor);
}

template<typename T>
void RTLIL::Process::rewrite_sigspecs2(T &functor)
{
	root_case.rewrite_sigspecs2(functor);
	for (auto it : syncs)
		it->rewrite_sigspecs2(functor);
}

// Uniform way to reach the owning Design of any RTLIL object, so pool-dependent
// helpers can be written generically over Module/Wire/Cell/Memory/Process.
namespace RTLIL {
	inline RTLIL::Design *design_of(const RTLIL::Module *m) { return m ? m->design : nullptr; }
	inline RTLIL::Design *design_of(const RTLIL::Wire *w) { return w && w->module ? w->module->design : nullptr; }
	inline RTLIL::Design *design_of(const RTLIL::Cell *c) { return c && c->module ? c->module->design : nullptr; }
	inline RTLIL::Design *design_of(const RTLIL::Memory *m) { return m && m->module ? m->module->design : nullptr; }
	inline RTLIL::Design *design_of(const RTLIL::Process *p) { return p && p->module ? p->module->design : nullptr; }
}

// Part two of the masquerade compat layer: the accessor bodies, which need
// Design and Module to be complete (see kernel/rtlil_twine_compat.h).
#define RTLIL_TWINE_COMPAT_IMPL
#include "kernel/rtlil_twine_compat.h"

// Carry source locations across a rewrite: combine the src of every cell in
// `sources` into a single twine and set it on every cell in `targets`. When
// more than one source contributes, the combination is a twine *concat node*
// referencing the contributing nodes -- the locations are never rendered to a
// string and re-interned, so the graph structure of the merged location is
// preserved. Sources without a src are skipped; if none has one, targets are
// left alone.
void merge_cell_src(RTLIL::Module *module, const std::vector<RTLIL::Cell*> &sources,
		const std::vector<RTLIL::Cell*> &targets);

YOSYS_NAMESPACE_END

#endif
