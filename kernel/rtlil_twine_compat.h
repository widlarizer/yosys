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
// Derived must define ref(), escaped(), and unescape(); everything else
// is derived from those three via operator IdString().
namespace RTLIL {
template<typename Derived>
struct NameMasqBase {
	operator IdString() const { return self().ref(); }
	operator std::string() const {
		return self().escaped();
	}
	bool isPublic() const { return self().ref().isPublic(); }
	bool empty() const { return self().ref() == Twine::Null; }
	std::string str() const { return self().escaped(); }
	std::string unescape() const { return self().unescape(); }
	bool begins_with(const char *s) const { return str().starts_with(s); }
	bool ends_with(const char *s) const { return str().ends_with(s); }
	template <typename... Ts> bool in(Ts &&...args) const {
		return self().ref().in(std::forward<Ts>(args)...);
	}
	std::string substr(size_t pos = 0, size_t len = std::string::npos) const {
		return self().escaped().substr(pos, len);
	}
	size_t size() const { return self().escaped().size(); }
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
	bool operator==(NullIdString) const { return self().ref() == Twine::Null; }
	bool operator!=(NullIdString) const { return !(self().ref() == Twine::Null); }
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
	// Tagged name handle (Twine::Null when unnamed).
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
	IdString id_ = Twine::Null;
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

#if defined(RTLIL_TWINE_COMPAT_IMPL) && !defined(RTLIL_TWINE_COMPAT_IMPL_DONE)
#define RTLIL_TWINE_COMPAT_IMPL_DONE

// The masq accessors below recover their containing Wire/Cell/Module by
// subtracting offsetof from `this`. Those types are non-standard-layout (base
// classes + virtuals), so offsetof is conditionally-supported, but it is
// well-defined on GCC/Clang for these fixed field offsets.
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

// Shared by Wire/Cell/Memory/Process (see ObjNameMasq's declaration above):
// all four resolve their Design via ->module->design to render name_.
template<typename Owner>
inline const Owner *RTLIL::ObjNameMasq<Owner>::owner() const {
	return reinterpret_cast<const Owner *>(
		reinterpret_cast<const char *>(this) - offsetof(Owner, name));
}

template<typename Owner>
inline Owner *RTLIL::ObjNameMasq<Owner>::owner() {
	return reinterpret_cast<Owner *>(
		reinterpret_cast<char *>(this) - offsetof(Owner, name));
}

template<typename Owner>
inline IdString RTLIL::ObjNameMasq<Owner>::ref() const {
	return owner()->name_;
}

template<typename Owner>
inline const TwinePool *RTLIL::ObjNameMasq<Owner>::pool() const {
	const Owner *o = owner();
	return o->module && o->module->design ? &o->module->design->twines : nullptr;
}

template<typename Owner>
inline std::string RTLIL::ObjNameMasq<Owner>::escaped() const {
	const Owner *o = owner();
	IdString id = ref();
	if (id == Twine::Null)
		return std::string();
	return o->module->design->twines.str(id);
}

template<typename Owner>
inline std::string RTLIL::ObjNameMasq<Owner>::unescape() const {
	const Owner *o = owner();
	IdString id = ref();
	if (id == Twine::Null)
		return std::string();
	return o->module->design->twines.unescaped_str(id);
}

template<typename Owner>
inline RTLIL::ObjNameMasq<Owner> &RTLIL::ObjNameMasq<Owner>::operator=(IdString id) {
	owner()->name_ = id;
	return *this;
}

inline const RTLIL::Cell *RTLIL::CellTypeMasq::owner() const {
	return reinterpret_cast<const RTLIL::Cell *>(
		reinterpret_cast<const char *>(this) - offsetof(RTLIL::Cell, type));
}

inline RTLIL::Cell *RTLIL::CellTypeMasq::owner() {
	return reinterpret_cast<RTLIL::Cell *>(
		reinterpret_cast<char *>(this) - offsetof(RTLIL::Cell, type));
}

inline IdString RTLIL::CellTypeMasq::ref() const {
	return owner()->type_impl;
}

inline const TwinePool *RTLIL::CellTypeMasq::pool() const {
	const RTLIL::Cell *c = owner();
	return c->module && c->module->design ? &c->module->design->twines : nullptr;
}

inline std::string RTLIL::CellTypeMasq::escaped() const {
	const RTLIL::Cell *c = owner();
	IdString id = c->type_impl;
	if (id == Twine::Null)
		return std::string();
	if (c->module && c->module->design)
		return c->module->design->twines.str(id);
	// Static (ID::) refs are pool-independent and resolve from the constid table.
	return ID::str(id);
}

inline std::string RTLIL::CellTypeMasq::unescape() const {
	const RTLIL::Cell *c = owner();
	IdString id = c->type_impl;
	if (id == Twine::Null)
		return std::string();
	if (c->module && c->module->design)
		return c->module->design->twines.unescaped_str(id);
	return ID::unescaped_str(id);
}

inline RTLIL::CellTypeMasq &RTLIL::CellTypeMasq::operator=(IdString id) {
	owner()->type_impl = id;
	return *this;
}

inline const RTLIL::Module *RTLIL::ModuleNameMasq::owner() const {
	return reinterpret_cast<const RTLIL::Module *>(
		reinterpret_cast<const char *>(this) - offsetof(RTLIL::Module, name));
}

inline RTLIL::Module *RTLIL::ModuleNameMasq::owner() {
	return reinterpret_cast<RTLIL::Module *>(
		reinterpret_cast<char *>(this) - offsetof(RTLIL::Module, name));
}

inline IdString RTLIL::ModuleNameMasq::ref() const {
	return owner()->name_;
}

inline const TwinePool *RTLIL::ModuleNameMasq::pool() const {
	const RTLIL::Module *m = owner();
	return m->design ? &m->design->twines : nullptr;
}

inline std::string RTLIL::ModuleNameMasq::escaped() const {
	const RTLIL::Module *m = owner();
	IdString id = ref();
	if (id == Twine::Null)
		return std::string();
	return m->design->twines.str(id);
}

inline std::string RTLIL::ModuleNameMasq::unescape() const {
	const RTLIL::Module *m = owner();
	IdString id = ref();
	if (id == Twine::Null)
		return std::string();
	return m->design->twines.unescaped_str(id);
}

inline RTLIL::ModuleNameMasq &RTLIL::ModuleNameMasq::operator=(IdString id) {
	owner()->name_ = id;
	return *this;
}

inline RTLIL::PooledName::PooledName(const RTLIL::Design *design, IdString id)
	: pool_(design ? &design->twines : nullptr), id_(id) { }

inline RTLIL::PooledName::PooledName(const RTLIL::Module *module, IdString id)
	: PooledName(module ? module->design : nullptr, id) { }

inline std::string RTLIL::PooledName::escaped() const {
	if (id_ == Twine::Null)
		return std::string();
	return pool_ ? pool_->str(id_) : ID::str(id_);
}

inline std::string RTLIL::PooledName::unescape() const {
	if (id_ == Twine::Null)
		return std::string();
	return pool_ ? pool_->unescaped_str(id_) : ID::unescaped_str(id_);
}
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif // -Winvalid-offsetof for masq accessors

// Prefer these over the pool-free log_id(IdString) (which can only render
// static constids): a masquerade knows the Design its name lives in.
template<typename Derived>
inline const char *log_id(const RTLIL::NameMasqBase<Derived> &name) {
	return log_id_str(static_cast<const Derived &>(name).unescape());
}

#endif // RTLIL_TWINE_COMPAT_IMPL
