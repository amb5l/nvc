module vlog54;
  integer d;
  real r;
  reg q;

  initial begin
    d = 4;
    r = 3.0;
    q = 1'b0;

    #d q = 1'b1;
    if ($time !== 4) begin
      $display("FAILED: integer delay time %0t q %b", $time, q);
      $finish;
    end

    #r q = 1'b0;
    if ($time !== 7 || q !== 1'b0) begin
      $display("FAILED: real delay time %0t q %b", $time, q);
      $finish;
    end

    $display("PASSED");
  end
endmodule
