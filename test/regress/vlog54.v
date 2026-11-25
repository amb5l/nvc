`timescale 1ns/1ns

module vlog54;
  reg a = 0;
  wire y, z, w;

  assign #(2, 1) y = a;
  assign #(3, 2, 1) z = a;
  assign (strong1, strong0) #(1, 0) w = a;

  initial begin
    #1 a = 1;
    #1 if (y === 1'b1) begin
      $display("FAILED early y=%b", y);
      $finish;
    end
    #2 if (y !== 1'b1 || w !== 1'b1) begin
      $display("FAILED late y=%b w=%b", y, w);
      $finish;
    end
    #1 if (z !== 1'b1) begin
      $display("FAILED z=%b", z);
      $finish;
    end
    $display("PASSED");
  end
endmodule
