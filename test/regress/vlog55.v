module vlog55;
  reg a;
  real r;

  always @(*)
    r = a + 1.0;

  initial begin
    a = 1'b1;
    #1;
    if (r != 2.0)
      $display("FAILED r=%f", r);
    else
      $display("PASSED");
  end
endmodule
