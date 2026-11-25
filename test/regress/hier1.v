module glbl;
  wire GSR;
  wire [3:0] DO_GLBL;
endmodule

module hier1;
  reg src;
  reg bit;
  wire gsr = glbl.GSR;
  wire d0 = glbl.DO_GLBL[0];

  assign glbl.GSR = src;
  assign glbl.DO_GLBL[0] = bit;

  initial begin
    src = 1'b0;
    bit = 1'b0;
    #1;
    if (gsr !== 1'b0 || d0 !== 1'b0) begin
      $display("FAILED early gsr=%b d0=%b", gsr, d0);
      $finish;
    end
    src = 1'b1;
    bit = 1'b1;
    #1;
    if (gsr !== 1'b1 || d0 !== 1'b1)
      $display("FAILED late gsr=%b d0=%b", gsr, d0);
    else
      $display("PASSED");
  end
endmodule
