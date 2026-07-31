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

#ifndef RTLIL_TWINE_COMPAT_H
#define RTLIL_TWINE_COMPAT_H

namespace RTLIL {
	template<typename Derived> struct NameMasqBase;
	struct ModuleNameMasq;
	template<typename Owner> struct ObjNameMasq;
	using WireNameMasq = ObjNameMasq<Wire>;
	using CellNameMasq = ObjNameMasq<Cell>;
	using MemoryNameMasq = ObjNameMasq<Memory>;
	using ProcessNameMasq = ObjNameMasq<Process>;
	struct CellTypeMasq;
	struct PooledName;
}

// CRTP base shared by WireNameMasq, CellNameMasq, and ModuleNameMasq.
// Derived must define ref(), pool(), escaped(), and unescape(); everything
// else is derived from those via operator IdString().
namespace RTLIL {

inline std::string render_escaped(const TwinePool *pool, IdString id) {
	if (id == IdString::Null)
		return std::string();
	return pool ? pool->str(id) : ID::str(id);
}

inline std::string render_unescaped(const TwinePool *pool, IdString id) {
	if (id == IdString::Null)
		return std::string();
	return pool ? pool->unescaped_str(id) : ID::unescaped_str(id);
}

template<typename Derived>
struct NameMasqBase {
	operator IdString() const { return self().ref(); }
	operator std::string() const {
		return self().escaped();
	}
	bool isPublic() const { return self().ref().isPublic(); }
	bool empty() const { return self().ref() == IdString::Null; }
	std::string str() const { return self().escaped(); }
	std::string unescape() const { return self().unescape(); }
	bool begins_with(const char *s) const {
		const TwinePool *pool = self().pool();
		return pool ? twine_begins_with(*pool, self().ref(), s) : str().starts_with(s);
	}
	bool ends_with(const char *s) const { return str().ends_with(s); }
	template <typename... Ts> bool in(Ts &&...args) const {
		return self().ref().in(std::forward<Ts>(args)...);
	}
	std::string substr(size_t pos = 0, size_t len = std::string::npos) const {
		return self().escaped().substr(pos, len);
	}
	size_t size() const {
		const TwinePool *pool = self().pool();
		return pool ? twine_size(*pool, self().ref()) : self().escaped().size();
	}
	bool contains(const char *p) const { return self().escaped().find(p) != std::string::npos; }
	char operator[](int n) const { return self().escaped()[n]; }
	bool lt_by_name(const Derived &rhs) const {
		const TwinePool *pool = self().pool();
		if (pool == nullptr)
			return self().escaped() < rhs.escaped();
		return twine_compare_by_name(*pool, self().ref(), rhs.ref()) < 0;
	}
	bool operator==(IdString rhs) const { return self().ref() == rhs; }
	bool operator!=(IdString rhs) const { return self().ref() != rhs; }
	bool operator==(NullIdString) const { return self().ref() == IdString::Null; }
	bool operator!=(NullIdString) const { return !(self().ref() == IdString::Null); }
	bool operator==(const std::string &rhs) const { return self().escaped() == rhs; }
	bool operator!=(const std::string &rhs) const { return self().escaped() != rhs; }
	bool operator==(const Derived &rhs) const { return self().ref() == rhs.ref(); }
	bool operator!=(const Derived &rhs) const { return self().ref() != rhs.ref(); }
	template<typename Other>
	bool operator==(const NameMasqBase<Other> &rhs) const
		{ return self().ref() == static_cast<const Other &>(rhs).ref(); }
	template<typename Other>
	bool operator!=(const NameMasqBase<Other> &rhs) const
		{ return self().ref() != static_cast<const Other &>(rhs).ref(); }
	bool operator<(const Derived &rhs) const { return self().ref() < rhs.ref(); }
	[[nodiscard]] Hasher hash_into(Hasher h) const { return self().ref().hash_into(h); }
private:
	const Derived &self() const { return *static_cast<const Derived *>(this); }
};
} // namespace RTLIL
template<typename Derived>
inline bool operator==(IdString lhs, const RTLIL::NameMasqBase<Derived> &rhs) {
	return lhs == static_cast<const Derived &>(rhs).ref();
}
template<typename Derived>
inline bool operator!=(IdString lhs, const RTLIL::NameMasqBase<Derived> &rhs) {
	return lhs != static_cast<const Derived &>(rhs).ref();
}

// Masquerade for Wire::name/Cell::name/Memory::name/Process::name.
// The handle itself lives inline in NamedObject::name_; only str()/unescape()
// need the owning Design's twines pool, reached via Owner::module->design.
// Defined before Wire/Cell/Memory/Process so it can be used as a
// [[no_unique_address]] member of each.
template<typename Owner>
struct RTLIL::ObjNameMasq : RTLIL::NameMasqBase<RTLIL::ObjNameMasq<Owner>> {
	// Copying/moving is forbidden: an ObjNameMasq derives its identity from
	// `this` via offsetof(Owner, name), so any instance not embedded in an
	// Owner would resolve to garbage.
	ObjNameMasq() = default;
	ObjNameMasq(const ObjNameMasq &) = delete;
	ObjNameMasq(ObjNameMasq &&) = delete;
	// Tagged name handle (IdString::Null when unnamed).
	IdString ref() const;
	const TwinePool *pool() const;
	// Escaped form ('\'-prefixed when public) / bare content.
	std::string escaped() const;
	std::string unescape() const;
	// Raw write of the backing name_, for code that used to assign
	// `obj->name` directly. This does *not* reindex the owning Module's
	// wires_/cells_ dicts: use Module::rename() to rename an object that is
	// already indexed. `id` must belong to the pool of the Design owning
	// this object.
	ObjNameMasq &operator=(IdString id);
	// Without this, `wire->name = other->name` would invoke the implicitly
	// deleted copy-assign instead of operator=(IdString).
	ObjNameMasq &operator=(const ObjNameMasq &other) { return *this = other.ref(); }
	ObjNameMasq &operator=(ObjNameMasq &&other) { return *this = other.ref(); }
private:
	const Owner *owner() const;
	Owner *owner();
};

// Masquerade for Cell::type. Backed by Cell::type_impl.
struct RTLIL::CellTypeMasq : RTLIL::NameMasqBase<RTLIL::CellTypeMasq> {
	// Copying/moving is forbidden, see ObjNameMasq.
	CellTypeMasq() = default;
	CellTypeMasq(const CellTypeMasq &) = delete;
	CellTypeMasq(CellTypeMasq &&) = delete;
	IdString ref() const;
	const TwinePool *pool() const;
	std::string escaped() const;
	std::string unescape() const;
	// Write path, equivalent to assigning cell->type_impl directly. `id` must
	// be a static (ID::) ref or belong to the owning Design's pool.
	CellTypeMasq &operator=(IdString id);
	CellTypeMasq &operator=(const CellTypeMasq &other) { return *this = other.ref(); }
	CellTypeMasq &operator=(CellTypeMasq &&other) { return *this = other.ref(); }
private:
	const RTLIL::Cell *owner() const;
	RTLIL::Cell *owner();
};

// Zero-size masquerade for Module::name. Same contract as WireNameMasq:
// the handle lives inline in NamedObject::name_, rendered through
// module->design->twines.
struct RTLIL::ModuleNameMasq : RTLIL::NameMasqBase<RTLIL::ModuleNameMasq> {
	// Copying/moving is forbidden: a ModuleNameMasq derives its identity from
	// `this` via offsetof(Module, name), so any instance not embedded in a
	// Module would resolve to garbage. All conversions go through
	// operator IdString() at the embedded location.
	ModuleNameMasq() = default;
	ModuleNameMasq(const ModuleNameMasq&) = delete;
	ModuleNameMasq(ModuleNameMasq&&) = delete;
	// Raw write of the backing name_; does not reindex design->modules_
	// (use Design::rename() for a module already added).
	ModuleNameMasq& operator=(IdString id);
	// Without this, `new_mod->name = src_mod->name` invokes the implicit
	// copy-assign (no-op) instead of operator=(IdString), so the meta
	// never gets written.
	ModuleNameMasq& operator=(const ModuleNameMasq& other) { return *this = other.ref(); }
	ModuleNameMasq& operator=(ModuleNameMasq&& other) { return *this = other.ref(); }
	IdString ref() const;
	const TwinePool *pool() const;
	std::string escaped() const;
	std::string unescape() const;
private:
	const RTLIL::Module *owner() const;
	RTLIL::Module *owner();
};

struct RTLIL::PooledName : RTLIL::NameMasqBase<RTLIL::PooledName> {
	PooledName() = default;
	explicit PooledName(IdString id) : id_(id) {}
	PooledName(const TwinePool *pool, IdString id) : pool_(pool), id_(id) {}
	PooledName(const RTLIL::Design *design, IdString id);
	PooledName(const RTLIL::Module *module, IdString id);
	template<typename D> PooledName(const RTLIL::NameMasqBase<D> &masq)
		: pool_(static_cast<const D &>(masq).pool()),
		  id_(static_cast<const D &>(masq).ref()) {}
	IdString ref() const { return id_; }
	std::string escaped() const;
	std::string unescape() const;
	const TwinePool *pool() const { return pool_; }
	PooledName &operator=(IdString id) { id_ = id; return *this; }
private:
	const TwinePool *pool_ = nullptr;
	IdString id_ = IdString::Null;
};

namespace RTLIL {
template<typename T>
concept IsNameMasq = std::is_base_of_v<NameMasqBase<std::decay_t<T>>, std::decay_t<T>>;

template<IsNameMasq A, typename B>
auto make_pair(A &&a, B &&b) { return std::make_pair(a.ref(), std::forward<B>(b)); }
template<typename A, IsNameMasq B>
auto make_pair(A &&a, B &&b) { return std::make_pair(std::forward<A>(a), b.ref()); }
template<IsNameMasq A, IsNameMasq B>
auto make_pair(A &&a, B &&b) { return std::make_pair(a.ref(), b.ref()); }
} // namespace RTLIL

#endif // RTLIL_TWINE_COMPAT_H
