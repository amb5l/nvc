module gate #(
  parameter integer WIDTH = 4,
  parameter [47:0] MASK = 48'h0,
  parameter [7:0] BITS = 8'hA5,
  parameter [0:0] FLAG = 1'b0,
  parameter MODE = "RAM"
) (
  output ok
);
  assign ok = (WIDTH == 4)
           && (MASK == 48'h0)
           && (BITS == 8'hA5)
           && (FLAG == 1'b0)
           && (MODE == "RAM");
endmodule
