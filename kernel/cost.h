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
 */

#ifndef COST_H
#define COST_H

#include "kernel/yosys.h"

YOSYS_NAMESPACE_BEGIN

struct CellCosts
{

	enum CostKind {
		DEFAULT,
		CMOS,
	};

	private:
	dict<RTLIL::IdString, int> mod_cost_cache;
	CostKind kind;
	Design *design = nullptr;
	bool type_only;

	public:
	CellCosts(CostKind kind, Design *design);

	const dict<RTLIL::IdString, int>& gate_type_cost() {
		static const dict<RTLIL::IdString, int> default_gate_db = {
			{ ID($_BUF_),    1 },
			{ ID($_NOT_),    2 },
			{ ID($_AND_),    4 },
			{ ID($_NAND_),   4 },
			{ ID($_OR_),     4 },
			{ ID($_NOR_),    4 },
			{ ID($_ANDNOT_), 4 },
			{ ID($_ORNOT_),  4 },
			{ ID($_XOR_),    5 },
			{ ID($_XNOR_),   5 },
			{ ID($_AOI3_),   6 },
			{ ID($_OAI3_),   6 },
			{ ID($_AOI4_),   7 },
			{ ID($_OAI4_),   7 },
			{ ID($_MUX_),    4 },
			{ ID($_NMUX_),   4 },
			{ ID($_DFF_P_),  1 },
			{ ID($_DFF_N_),  1 },
		};

		static const dict<RTLIL::IdString, int> cmos_transistors_db = {
			{ ID($_BUF_),     1 },
			{ ID($_NOT_),     2 },
			{ ID($_AND_),     6 },
			{ ID($_NAND_),    4 },
			{ ID($_OR_),      6 },
			{ ID($_NOR_),     4 },
			{ ID($_ANDNOT_),  6 },
			{ ID($_ORNOT_),   6 },
			{ ID($_XOR_),    12 },
			{ ID($_XNOR_),   12 },
			{ ID($_AOI3_),    6 },
			{ ID($_OAI3_),    6 },
			{ ID($_AOI4_),    8 },
			{ ID($_OAI4_),    8 },
			{ ID($_MUX_),    12 },
			{ ID($_NMUX_),   10 },
			{ ID($_DFF_P_),  16 },
			{ ID($_DFF_N_),  16 },
		};
		switch (kind) {
			case DEFAULT:
				return default_gate_db;
			case CMOS:
				return cmos_transistors_db;
			default:
				log_assert(false && "Unreachable: Invalid cell cost kind\n");
		}
	}

	int get(RTLIL::Module *mod)
	{
		if (mod->attributes.count(ID(cost)))
			return mod->attributes.at(ID(cost)).as_int();

		if (mod_cost_cache.count(mod->name))
			return mod_cost_cache.at(mod->name);

		int module_cost = 1;
		for (auto c : mod->cells())
			module_cost += get(c);

		mod_cost_cache[mod->name] = module_cost;
		return module_cost;
	}

	int get(RTLIL::Cell *cell)
	{
		if (gate_type_cost().count(cell->type))
			return gate_type_cost().at(cell->type);

		if (design && design->module(cell->type) && cell->parameters.empty())
		{
			return get(design->module(cell->type));
		} else if (RTLIL::builtin_ff_cell_types().count(cell->type)) {
			log_assert(cell->hasPort(ID::Q) && "Weird flip flop");
			return cell->getParam(ID::WIDTH).as_int();
		}

		log_warning("Can't determine cost of %s cell (%d parameters).\n", log_id(cell->type), GetSize(cell->parameters));
		return 1;
	}
};

YOSYS_NAMESPACE_END

#endif
