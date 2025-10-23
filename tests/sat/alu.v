module alu(
	input clk,
	input [7:0] A,
	input [7:0] B,
	input [3:0] operation,
	output reg [7:0] result
);

	localparam ALU_OP_ADD /* verilator public_flat */ = 4'b0000;
	localparam ALU_OP_SUB /* verilator public_flat */ = 4'b0001;

	reg [8:0] tmp;

	always @(posedge clk)
	begin
		case (operation)
			ALU_OP_ADD :
				tmp = A + B;
			ALU_OP_SUB :
				tmp = A - B;
		endcase
		result <= tmp[7:0];
	end
endmodule

