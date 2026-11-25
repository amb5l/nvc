module gate #(
  parameter SUL = 1'b0,
  parameter [7:0] SULV = 8'h00,
  parameter [0:0] SL = 1'b1,
  parameter [3:0] SLV = 4'h0,
  parameter real PERIOD = 0.0,
  parameter integer WIDTH = 0
) (
  output ok
);
  assign ok = (SUL == 1'b1)
           && (SULV == 8'h3C)
           && (SL == 1'b0)
           && (SLV == 4'hA)
           && (PERIOD == 10.5)
           && (WIDTH == 4);
endmodule
