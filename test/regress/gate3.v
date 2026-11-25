module gate3;
  wire [3:0] yb, yp, yif, yrep;
  reg [3:0] a;
  reg en;
  reg scalar;

  buf b_copy[3:0] (yb, a);
  pulldown pd[3:0] (yp);
  bufif1 b_en[3:0] (yif, a, en);
  buf b_rep[3:0] (yrep, scalar);

  localparam integer MSB = 3;
  wire [MSB:0] ypar, apar;
  buf b_par[MSB:0] (ypar, apar);
  assign apar = a;

  initial begin
    a = 4'b1010;
    en = 1'b0;
    scalar = 1'b1;
    #1;
    if (yb !== 4'b1010) $display("FAILED buf %b", yb);
    else if (yp !== 4'b0000) $display("FAILED pulldown %b", yp);
    else if (yif !== 4'bzzzz) $display("FAILED bufif off %b", yif);
    else if (yrep !== 4'b1111) $display("FAILED replicate %b", yrep);
    else if (ypar !== 4'b1010) $display("FAILED param %b", ypar);
    else begin
      en = 1'b1;
      #1;
      if (yif !== 4'b1010) $display("FAILED bufif on %b", yif);
      else $display("PASSED");
    end
  end
endmodule
