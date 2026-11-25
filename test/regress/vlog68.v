module gate #(
  parameter STR_T = "FALSE",
  parameter STR_F = "TRUE",
  parameter integer INT_T = 0,
  parameter integer INT_F = 1,
  parameter [3:0] VEC_T = 4'h0,
  parameter [7:0] VEC_F = 8'hff,
  parameter BIT_T = 1'b0
) (
  output ok
);
  assign ok = (STR_T == "TRUE")
           && (STR_F == "FALSE")
           && (INT_T == 1)
           && (INT_F == 0)
           && (VEC_T == 4'h1)
           && (VEC_F == 8'h00)
           && (BIT_T == 1'b1);
endmodule
