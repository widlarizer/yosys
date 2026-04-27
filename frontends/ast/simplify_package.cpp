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

#include "kernel/log.h"
#include "libs/sha1/sha1.h"
#include "frontends/verilog/verilog_frontend.h"
#include "frontends/ast/simplify_package.h"
#include "ast.h"
#include "ast_typed.h"


YOSYS_NAMESPACE_BEGIN

using namespace AST;
using namespace AST_INTERNAL;

AST_INTERNAL::PackageImporter::PackageImporter(const AstNode* n, const AstNode* child) : node(n) {
	is_wildcard = child->children.empty();
	// For specific imports, collect the list of items to import
	if (!is_wildcard) {
		for (auto& item : child->children) {
			import_items.insert(item->str);
		}
	}
}

void AST_INTERNAL::PackageImporter::import(std::map<std::string, AstNode*>& scope, AstNode* to_import) const {
	// Check if this is a specific import and if this item should be imported
	if (!is_wildcard && import_items.count(to_import->str) == 0)
		return;

	if (to_import->type == AST_PARAMETER || to_import->type == AST_LOCALPARAM ||
		to_import->type == AST_TYPEDEF || to_import->type == AST_FUNCTION ||
		to_import->type == AST_TASK || to_import->type == AST_ENUM) {
		// For wildcard imports, check if item already exists (from specific import)
		if (is_wildcard && scope.count(to_import->str) > 0)
			return;
		scope[to_import->str] = to_import;
	}
	if (to_import->type == AST_ENUM) {
		for (auto& enode : to_import->children) {
			log_assert(enode->type==AST_ENUM_ITEM);
			// Check if this enum item should be imported
			if (!is_wildcard && import_items.count(enode->str) == 0)
				continue;
			// For wildcard imports, check if item already exists (from specific import)
			if (is_wildcard && scope.count(enode->str) > 0)
				continue;
			if (scope.count(enode->str) == 0)
				scope[enode->str] = enode.get();
			else
				node->input_error("enum item %s already exists in current scope\n", enode->str);
		}
	}
}


YOSYS_NAMESPACE_END
