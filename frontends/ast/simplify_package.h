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
 *  This is the AST frontend library.
 *
 *  The AST frontend library is not a frontend on it's own but provides a
 *  generic abstract syntax tree (AST) abstraction for HDL code and can be
 *  used by HDL frontends. See "ast.h" for an overview of the API and the
 *  Verilog frontend for an usage example.
 *
 */

#ifndef AST_SIMPLIFY_PACKAGE_H
#define AST_SIMPLIFY_PACKAGE_H

// #include "kernel/log.h"
// #include "libs/sha1/sha1.h"
// #include "frontends/verilog/verilog_frontend.h"
#include "ast.h"
#include "kernel/yosys_common.h"
// #include "ast_typed.h"
// #include "kernel/io.h"

YOSYS_NAMESPACE_BEGIN

using namespace AST;
using namespace AST_INTERNAL;

namespace AST_INTERNAL {
	class PackageImporter {
		std::set<std::string> import_items;
		bool is_wildcard;
		const AstNode* node;
	public:
		PackageImporter(const AstNode* n, const AstNode* child);
		void import(std::map<std::string, AstNode*>& scope, AstNode* to_import) const;
	};
};

YOSYS_NAMESPACE_END

#endif