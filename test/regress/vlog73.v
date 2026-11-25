module vlog73;
  parameter [3:0] DIV = 4'd4;
  reg [31:0] div_reg = DIV;
  reg [3:0] bin;
  integer first;
  reg trig;

  initial begin
    first = 1;
    trig = 1'b0;
    #1 trig = ~trig;
  end

  // BUFGCE_DIV writes the divide code in an earlier always block.
  always @(trig) begin
    #1 bin = div_reg[3:0];
  end

  // A later always reads it at the same time. IEEE 1800 leaves the
  // order unspecified; NVC runs the earlier block first.
  always @(trig) begin
    #1 if (bin == 4) first = 4;
  end

  initial begin
    #3;
    if (bin === 4'b0100 && first == 4)
      $display("PASSED");
    else
      $display("FAILED bin=%b first=%0d", bin, first);
  end
endmodule
