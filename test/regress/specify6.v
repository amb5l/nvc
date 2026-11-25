module specify6;
  reg clk = 0;
  reg d = 0;
  reg notifier;
  reg [7:0] a = 8'ha5;
  wire [7:0] y;

  assign y = a;

  specify
    (a[1:1] *> y[7:0]) = (0:0:0, 0:0:0);
    specparam PATHPULSE$ = 0;
    $setuphold(posedge clk, posedge d, 0:0:0, 0:0:0);
    $width(posedge clk, 0:0:0, 0, notifier);
  endspecify

  initial begin
    #1 clk = 1;
    #1 d = 1;
    #1;
    if (y !== 8'ha5)
      $display("FAILED y=%b", y);
    else
      $display("PASSED");
  end
endmodule
