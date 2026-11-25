// A delay that names a parameter is in the module time unit.
// `timescale 1 ps / 1 ps and #(ROC_WIDTH) with ROC_WIDTH 100000 is
// 100 ns, not 100 ps. IEEE 1800-2017 22.7.
`timescale 1 ps / 1 ps

module vlog80;
  parameter ROC_WIDTH = 100000;
  localparam MX = 60;
  integer gap;
  reg flag;

  initial begin
    flag = 1'b1;
    gap = 5;

    #(ROC_WIDTH);
    if ($realtime != 100000.0) begin
      $display("FAILED: parameter delay %f", $realtime);
      $finish;
    end

    #(MX);
    if ($realtime != 100060.0) begin
      $display("FAILED: localparam delay %f", $realtime);
      $finish;
    end

    #(gap);
    if ($realtime != 100065.0) begin
      $display("FAILED: variable delay %f", $realtime);
      $finish;
    end

    #(100);
    if ($realtime != 100165.0) begin
      $display("FAILED: parenthesized number %f", $realtime);
      $finish;
    end

    #10;
    if ($realtime != 100175.0 || flag !== 1'b1) begin
      $display("FAILED: literal delay %f", $realtime);
      $finish;
    end

    flag = 1'b0;
    $display("PASSED");
  end
endmodule
