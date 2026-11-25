// Intra-assignment delay of a real times an integer. timescale 1 ps
// makes 200.0 * 2 equal 400 ps. The update is not immediate, and a
// real literal delay uses the same time unit. IEEE 1800-2017 22.7.
`timescale 1 ps / 1 ps

module vlog81;
  real tap;
  integer taps;
  reg q, d;

  initial begin
    tap = 200.0;
    taps = 2;
    d = 1'b0;
    q = 1'b0;

    #10;
    d = 1'b1;
    q <= #(tap * taps) d;

    #1;
    if (q !== 1'b0) begin
      $display("FAILED: updated immediately at %f", $realtime);
      $finish;
    end

    #(tap * taps);
    if (q !== 1'b1 || $realtime != 411.0) begin
      $display("FAILED: intra delay q %b at %f", q, $realtime);
      $finish;
    end

    #200.0;
    if ($realtime != 611.0) begin
      $display("FAILED: real literal %f", $realtime);
      $finish;
    end

    $display("PASSED");
  end
endmodule
