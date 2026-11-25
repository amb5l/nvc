// $setuphold / $recrem delayed nets are zero-delay copies when
// no timing annotation is present (IEEE 1800-2017 31.9).

module vlog70;
  reg clk;
  reg d;
  reg notifier;
  wire clk_d;
  wire d_d;
  wire clk_r;
  wire d_r;
  wire clk_b;
  wire d_b;
  wire cond;
  reg q;
  reg pass;

  assign cond = 1'b1;

  specify
    // Xilinx form: notifier, timestamp and timecheck conditions, then
    // the delayed nets. A second check names the same nets.
    $setuphold(posedge clk, posedge d, 0:0:0, 0:0:0, notifier, cond, cond, clk_d, d_d);
    $setuphold(negedge clk, negedge d, 0:0:0, 0:0:0, notifier, cond, cond, clk_d, d_d);
    $recrem(posedge clk, posedge d, 0:0:0, 0:0:0,,,, clk_r, d_r);
    // Inline delay annotation is accepted and not applied.
    $setuphold(posedge clk, posedge d, 0:0:0, 0:0:0,,,, clk_b[1:1:1], d_b);
  endspecify

  always @(posedge clk_d)
    q <= d_d;

  initial begin
    pass = 1'b1;
    clk = 1'b0;
    d = 1'b0;
    notifier = 1'b0;
    q = 1'b0;

    #1;
    if (clk_d !== 1'b0 || d_d !== 1'b0 || clk_b !== 1'b0 || d_b !== 1'b0) begin
      $display("FAILED: initial copy clk_d=%b d_d=%b clk_b=%b d_b=%b",
               clk_d, d_d, clk_b, d_b);
      pass = 1'b0;
    end

    d = 1'b1;
    #1;
    if (d_d !== 1'b1 || d_r !== 1'b1 || d_b !== 1'b1) begin
      $display("FAILED: data copy d_d=%b d_r=%b d_b=%b", d_d, d_r, d_b);
      pass = 1'b0;
    end

    clk = 1'b1;
    #1;
    if (clk_d !== 1'b1 || clk_r !== 1'b1 || clk_b !== 1'b1 || q !== 1'b1) begin
      $display("FAILED: clock copy clk_d=%b clk_r=%b q=%b", clk_d, clk_r, q);
      pass = 1'b0;
    end

    if (pass)
      $display("PASSED");
  end
endmodule
