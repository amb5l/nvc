module gate (input t, input i, inout io, output o,
              input [3:0] bi, inout [3:0] dio, output [3:0] bo);
  assign o = io;
  assign io = t ? 1'bz : i;
  assign bo = dio;
  assign dio = t ? 4'bz : bi;
endmodule
