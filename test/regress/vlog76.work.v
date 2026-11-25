`timescale 1 ns / 1 ns

module leaf76(output reg hit);
  tri0 glblGSR = glbl.GSR;
  reg saw_high;

  initial saw_high = 1'b0;

  // The shape used by the UNISIM models: a local tri0 copies glbl.GSR
  // and a level-sensitive block notices the pulse.
  always @(glblGSR)
    if (glblGSR === 1'b1)
      saw_high = 1'b1;

  initial begin
    hit = 1'b0;
    #1;
    if (glblGSR === 1'b1)
      hit = 1'b1;
    #10;
    if (glblGSR !== 1'b0 || saw_high !== 1'b1)
      hit = 1'b0;
  end
endmodule
