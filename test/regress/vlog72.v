// defparam overrides a child parameter. A value in the instance's
// #() list is replaced when the same parameter is named again, and
// a parameter with neither stays at its default.

module leaf72 #(
  parameter A = 0,
  parameter B = 0,
  parameter C = 7,
  parameter [31:0] SEL = "TRUE"
)(
  input [31:0] expect_a,
  input [31:0] expect_b,
  input [31:0] expect_c,
  input [31:0] expect_sel
);
  initial begin
    if (A !== expect_a || B !== expect_b || C !== expect_c
        || SEL !== expect_sel)
      $display("FAILED A=%0d B=%0d C=%0d SEL=%s", A, B, C, SEL);
  end
endmodule

module vlog72;
  leaf72 u(.expect_a(3), .expect_b(4), .expect_c(7), .expect_sel("TRUE"));
  defparam u.A = 3;
  defparam u.B = 4;

  leaf72 #(.A(1)) w(.expect_a(9), .expect_b(0), .expect_c(7),
                    .expect_sel("FALSE"));
  defparam w.A = 9;
  defparam w.SEL = "FALSE";

  leaf72 #(1, 2) x(.expect_a(1), .expect_b(5), .expect_c(7),
                   .expect_sel("TRUE"));
  defparam x.B = 5;

  leaf72 p(.expect_a(1), .expect_b(0), .expect_c(7), .expect_sel("TRUE")),
         q(.expect_a(2), .expect_b(0), .expect_c(7), .expect_sel("TRUE"));
  defparam p.A = 1, q.A = 2;

  defparam early.A = 8;
  leaf72 early(.expect_a(8), .expect_b(0), .expect_c(7),
               .expect_sel("TRUE"));

  initial begin
    #1;
    $display("PASSED");
  end
endmodule
