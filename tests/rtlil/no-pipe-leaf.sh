set -euo pipefail

mkdir -p temp

cat > temp/pipe.v <<'EOF'
module top(input clk, input [7:0] a, b, c, d, output reg [7:0] x, y);
  always @(posedge clk) begin
    x <= a + b;
    y <= c + d;
  end
endmodule
EOF

${YOSYS} -p "read_verilog temp/pipe.v; hierarchy -top top; proc; opt; opt_merge -share_all; alumacc; opt_dff; dump_twines" \
    > temp/pipe-dump.txt 2>&1

if grep -E '^[[:space:]]*@[0-9]+ leaf ' temp/pipe-dump.txt | grep -q '|'; then
    echo "FAIL: dump_twines produced a leaf containing '|':" >&2
    grep -E '^[[:space:]]*@[0-9]+ leaf ' temp/pipe-dump.txt | grep '|' >&2
    exit 1
fi

cat > temp/mem.v <<'EOF'
module mem(input clk, input we, input [3:0] addr, input [7:0] din, output reg [7:0] dout);
  reg [7:0] m [0:15];
  always @(posedge clk) begin
    if (we) m[addr] <= din;
    dout <= m[addr];
  end
endmodule
EOF

${YOSYS} -p "read_verilog temp/mem.v; hierarchy -top mem; proc; opt; memory_map; opt_dff; dump_twines" \
    > temp/mem-dump.txt 2>&1

if grep -E '^[[:space:]]*@[0-9]+ leaf ' temp/mem-dump.txt | grep -q '|'; then
    echo "FAIL: dump_twines (memory_map path) produced a leaf containing '|':" >&2
    grep -E '^[[:space:]]*@[0-9]+ leaf ' temp/mem-dump.txt | grep '|' >&2
    exit 1
fi
