// A wire driven at elaboration changes during initialisation, before
// any always block is waiting. That change is a time-0 event and the
// always block must run (IEEE 1800-2017 4.5 and 9.2.1).
//
// ISERDESE1 then copies that value into a reg that is already 0
// (`o_delay_pre_fb = 0`) and samples a second reg that was never
// written (`data_in`, default X). Storing the value a variable already
// holds does not generate an event (IEEE 1800-2017 10.4.2), and an
// event control waits for a change (9.4.2). The sampler therefore
// still reads X. That is why memory-mode Q2 on the second word is X
// in Verilog and 0 in the VHDL model, which initialises the signal.
`timescale 1 ns / 1 ps

module vlog84;
  reg d = 1'b0;
  wire din = d;
  reg mid = 1'bx;
  reg copied = 1'bx;

  // Value changes, so both copies must update at time 0.
  always @(din) mid <= din;
  always @(mid) copied <= mid;

  // Same shape as the ISERDESE1 input path: the middle reg is already
  // 0, so the second always never wakes and the sampled reg stays X.
  wire same = 1'b0;
  reg held = 1'b0;
  reg sampled = 1'bx;
  always @(same) held <= same;
  always @(held) sampled <= held;

  initial begin
    #1;
    if (mid !== 1'b0 || copied !== 1'b0) begin
      $display("FAILED: time-0 copy mid=%b copied=%b", mid, copied);
      $finish;
    end
    if (sampled !== 1'bx) begin
      $display("FAILED: same-value update woke the sampler, got %b", sampled);
      $finish;
    end

    d = 1'b1;
    #1;
    if (mid !== 1'b1 || copied !== 1'b1) begin
      $display("FAILED: later copy mid=%b copied=%b", mid, copied);
      $finish;
    end

    $display("PASSED");
  end
endmodule
