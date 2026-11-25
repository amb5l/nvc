module gate #(
  parameter integer WIDTH = 1,
  parameter MODE = "NONE"
) (
  input [3:0] d,
  output [3:0] q
);
  assign q = d;

  initial begin
    if (WIDTH != 4)
      $display("FAILED width %0d", WIDTH);
    if (MODE != "PASS")
      $display("FAILED mode");
  end
endmodule
