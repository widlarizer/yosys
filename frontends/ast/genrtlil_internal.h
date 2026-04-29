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
 *  Internal helpers shared between genrtlil.cc and genrtlil_process.cc.
 */

#ifndef GENRTLIL_INTERNAL_H
#define GENRTLIL_INTERNAL_H

#include "ast.h"

YOSYS_NAMESPACE_BEGIN

namespace AST_INTERNAL {

void copy_const_attributes(RTLIL::AttrObject *target, AST::AstNode *that);
void check_unique_id(RTLIL::Module *module, RTLIL::IdString id,
		const AST::AstNode *node, const char *to_add_kind);

// Lower an always/initial AST block to an RTLIL::Process on the
// current_module. Returns the SigSpec listing all output signals
// driven by the resulting process (used to populate
// ignoreThisSignalsInInitial). Defined in genrtlil_process.cc.
RTLIL::SigSpec generate_process(std::unique_ptr<AST::AstNode> always,
		RTLIL::SigSpec initSyncSignals = RTLIL::SigSpec());

}

YOSYS_NAMESPACE_END

#endif
