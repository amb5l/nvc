// Continuous assign delays are rise, fall, and turn-off.
// assign #(100000, 0) drives 1 after 100 ns and 0 immediately.
// IEEE 1800-2017 28.16.
`timescale 1 ps / 1 ps

module vlog82;
  reg rst;
  wire locked;
  reg [1:0] src;
  wire [1:0] bus;
  reg plain_d;
  wire plain;
  reg hz;
  wire zed;

  assign #(100000, 0) locked = rst ? 1'b0 : 1'b1;
  assign #(1000, 0) bus = src;
  assign #(1000) plain = plain_d;
  assign #(1000, 0, 250) zed = hz ? 1'bz : 1'b0;

  initial begin
    // An X input uses the minimum delay (IEEE 1800-2017 28.16), so the
    // first 1 is the rise that follows rst becoming 0.
    rst = 1'b0;
    src = 2'b00;
    plain_d = 1'b0;

    #1;
    if (locked !== 1'bx) begin
      $display("FAILED: rise not pending %b at %f", locked, $realtime);
      $finish;
    end

    #100000;
    if (locked !== 1'b1) begin
      $display("FAILED: rise %b at %f", locked, $realtime);
      $finish;
    end

    rst = 1'b1;
    #1;
    if (locked !== 1'b0) begin
      $display("FAILED: fall took the rise delay %b at %f", locked, $realtime);
      $finish;
    end

    rst = 1'b0;
    #1;
    if (locked !== 1'b0) begin
      $display("FAILED: second rise early %b at %f", locked, $realtime);
      $finish;
    end
    #100000;
    if (locked !== 1'b1) begin
      $display("FAILED: second rise %b at %f", locked, $realtime);
      $finish;
    end

    src = 2'b10;
    #1;
    if (bus !== 2'b00) begin
      $display("FAILED: vector rise early %b at %f", bus, $realtime);
      $finish;
    end
    #1000;
    if (bus !== 2'b10) begin
      $display("FAILED: vector rise/fall %b at %f", bus, $realtime);
      $finish;
    end

    src = 2'b00;
    #1;
    if (bus !== 2'b00) begin
      $display("FAILED: vector fall %b at %f", bus, $realtime);
      $finish;
    end

    plain_d = 1'b1;
    #1;
    if (plain !== 1'b0) begin
      $display("FAILED: single delay rose early %b", plain);
      $finish;
    end
    #1000;
    if (plain !== 1'b1) begin
      $display("FAILED: single delay rise %b at %f", plain, $realtime);
      $finish;
    end
    plain_d = 1'b0;
    #1;
    if (plain !== 1'b1) begin
      $display("FAILED: single delay fall was immediate %b", plain);
      $finish;
    end
    #1000;
    if (plain !== 1'b0) begin
      $display("FAILED: single delay fall %b at %f", plain, $realtime);
      $finish;
    end

    hz = 1'b0;
    #1;
    if (zed !== 1'b0) begin
      $display("FAILED: turn-off setup %b", zed);
      $finish;
    end
    hz = 1'b1;
    #1;
    if (zed !== 1'b0) begin
      $display("FAILED: turn-off was immediate %b", zed);
      $finish;
    end
    #249;
    if (zed !== 1'bz) begin
      $display("FAILED: turn-off %b at %f", zed, $realtime);
      $finish;
    end

    $display("PASSED");
  end
endmodule
