`timescale 1 ns / 1 ns

module glbl;
  wire GSR;
  reg GSR_int;
  assign (strong1, weak0) GSR = GSR_int;
  initial begin
    GSR_int = 1'b1;
    #10 GSR_int = 1'b0;
  end
endmodule
