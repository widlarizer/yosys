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

#ifndef RTLIL_TWINE_COMPAT_IMPL_H
#define RTLIL_TWINE_COMPAT_IMPL_H

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
	return RTLIL::render_escaped(pool(), ref());
}

template<typename Owner>
inline std::string RTLIL::ObjNameMasq<Owner>::unescape() const {
	return RTLIL::render_unescaped(pool(), ref());
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
	return RTLIL::render_escaped(pool(), ref());
}

inline std::string RTLIL::CellTypeMasq::unescape() const {
	return RTLIL::render_unescaped(pool(), ref());
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
	return RTLIL::render_escaped(pool(), ref());
}

inline std::string RTLIL::ModuleNameMasq::unescape() const {
	return RTLIL::render_unescaped(pool(), ref());
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
	return RTLIL::render_escaped(pool_, id_);
}

inline std::string RTLIL::PooledName::unescape() const {
	return RTLIL::render_unescaped(pool_, id_);
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

#endif // RTLIL_TWINE_COMPAT_IMPL_H
