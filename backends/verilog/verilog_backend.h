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
 *  A simple and straightforward Verilog backend.
 *
 */

#ifndef VERILOG_BACKEND_H
#define VERILOG_BACKEND_H

#include <string>
#include <iostream>
#include <set>
#include <vector>
#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/mem.h"

YOSYS_NAMESPACE_BEGIN
namespace VERILOG_BACKEND {

	const pool<string> verilog_keywords();
	bool char_is_verilog_escaped(char c);
	bool id_is_verilog_escaped(const std::string &str);

	struct VerilogBackendContext {
		// Output formatting options
		bool verbose = false;
		bool norename = false;
		bool noattr = false;
		bool attr2comment = false;
		bool noexpr = false;
		bool nodec = false;
		bool nohex = false;
		bool nostr = false;
		bool extmem = false;
		bool defparam = false;
		bool decimal = false;
		bool siminit = false;
		bool systemverilog = false;
		bool simple_lhs = false;
		bool noparallelcase = false;
		bool default_params = false;
		bool wreck = false;

		// Auto naming counters and map
		int auto_name_counter = 0;
		int auto_name_offset = 0;
		int auto_name_digits = 1;
		int extmem_counter = 0;
		dict<RTLIL::IdString, int> auto_name_map;
		std::string auto_prefix = "$auto$";
		std::string extmem_prefix = "$extmem$";

		// Module-level state
		RTLIL::Module *active_module = nullptr;
		dict<RTLIL::SigBit, RTLIL::State> active_initdata;
		SigMap active_sigmap;
		RTLIL::IdString initial_id;
		std::set<RTLIL::IdString> reg_wires;
	};

	class VerilogDumper {
	private:
		std::ostream &f;
		VerilogBackendContext ctx;

	public:
		VerilogDumper(std::ostream &output) : f(output) {}

		void configure(const VerilogBackendContext &context) {
			ctx = context;
		}

		VerilogBackendContext &get_context() { return ctx; }
		std::ostream &get_ostream() { return f; }

		void dump_const(const RTLIL::Const &data, int width = -1, int offset = 0, bool no_decimal = false, bool escape_comment = false);
		void dump_reg_init(SigSpec sig);
		void dump_sigchunk(const RTLIL::SigChunk &chunk, bool no_decimal = false);
		void dump_sigspec(const RTLIL::SigSpec &sig);
		void dump_attributes(std::string indent, dict<RTLIL::IdString, RTLIL::Const> &attributes, std::string term = "\n", bool modattr = false, bool regattr = false, bool as_comment = false);
		void dump_parameter(std::string indent, RTLIL::IdString id_string, RTLIL::Const parameter);
		void dump_wire(std::string indent, RTLIL::Wire *wire);
		void dump_memory(std::string indent, Mem &mem);
		void dump_cell_expr_port(RTLIL::Cell *cell, std::string port, bool gen_signed = true);
		void dump_cell_expr_uniop(std::string indent, RTLIL::Cell *cell, std::string op);
		void dump_cell_expr_binop(std::string indent, RTLIL::Cell *cell, std::string op);
		void dump_cell_expr_print(std::string indent, const RTLIL::Cell *cell);
		void dump_cell_expr_check(std::string indent, const RTLIL::Cell *cell);
		bool dump_ff(std::string indent, RTLIL::Cell *cell);
		bool dump_cell_expr(std::string indent, RTLIL::Cell *cell);
		void dump_cell(std::string indent, RTLIL::Cell *cell);
		void dump_sync_effect(std::string indent, const RTLIL::SigSpec &trg, const RTLIL::Const &polarity, std::vector<const RTLIL::Cell*> &cells);
		void dump_conn(std::string indent, const RTLIL::SigSpec &left, const RTLIL::SigSpec &right);
		void dump_case_actions(std::string indent, RTLIL::CaseRule *cs);
		bool dump_proc_switch_ifelse(std::string indent, RTLIL::SwitchRule *sw);
		void dump_case_body(std::string indent, RTLIL::CaseRule *cs, bool omit_trailing_begin = false);
		void dump_proc_switch(std::string indent, RTLIL::SwitchRule *sw);
		void dump_process(std::string indent, RTLIL::Process *proc, bool find_regs = false);
		void dump_module(std::string indent, RTLIL::Module *module);
	};

}; /* namespace VERILOG_BACKEND */
YOSYS_NAMESPACE_END

#endif /* VERILOG_BACKEND_H */
