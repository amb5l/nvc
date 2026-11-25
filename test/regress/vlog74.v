module leaf74(output reg ok);
  parameter integer DIV = 1;
  reg [31:0] div_reg = DIV;
  reg [3:0] bin;
  reg trig;

  initial begin
    ok = 0;
    trig = 0;
    #1 trig = 1;
  end

  // Earlier block writes the parameter. A later block reads it after
  // the same delay. With two instances those events share a timestamp.
  always @(trig) begin
    #1 bin = div_reg[3:0];
  end

  always @(trig) begin
    #1 if (bin == DIV) ok = 1;
  end
endmodule

module vlog74;
  wire aok, bok;
  leaf74 #(.DIV(2)) a(aok);
  leaf74 #(.DIV(4)) b(bok);

  initial begin
    #3;
    if (aok === 1'b1 && bok === 1'b1)
      $display("PASSED");
    else
      $display("FAILED a=%b b=%b", aok, bok);
  end
endmodule
