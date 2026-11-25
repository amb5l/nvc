`timescale 1 ns / 1 ps

module vlog66;
  initial begin
    if ($realtime != 0.0) begin
      $display("FAILED: time zero is %f", $realtime);
      $finish;
    end

    #1;
    if ($realtime != 1.0) begin
      $display("FAILED: 1 ns is %f", $realtime);
      $finish;
    end

    #3;
    if ($realtime != 4.0) begin
      $display("FAILED: 4 ns is %f", $realtime);
      $finish;
    end

    $display("PASSED");
  end
endmodule
