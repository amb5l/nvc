// Drive strengths seen from VHDL: weak output, pull, keeper, bibuf.
module upout77 (output o);
  wire a;
  pullup (a);
  buf (weak0, weak1) (o, a);
endmodule

module downout77 (output o);
  wire a;
  pulldown (a);
  buf (weak0, weak1) (o, a);
endmodule

module ipull77 (inout pin, output seen);
  pullup (pin);
  assign seen = pin;
endmodule

module idown77 (inout pin, output seen);
  pulldown (pin);
  assign seen = pin;
endmodule

module iweak77 (inout pin, output seen);
  assign (weak1, weak0) pin = 1'b1;
  assign seen = pin;
endmodule

module ifight77 (inout pin, output seen);
  assign pin = 1'b1;
  assign seen = pin;
endmodule

module ibuf77 (input i, input t, inout io, output o);
  bufif0 (io, i, t);
  assign o = io;
endmodule

module vpull77 (inout [1:0] pin);
  pullup (pin[0]);
  pulldown (pin[1]);
endmodule

module bibuf77 (inout a, inout b);
  assign (weak1, weak0) a = b;
  assign (weak1, weak0) b = a;
endmodule

module keep77 (inout pin);
  reg held;
  initial held = 1'b0;
  always @(pin) begin
    if (pin === 1'b1)
      held <= 1'b1;
    else if (pin === 1'b0)
      held <= 1'b0;
  end
  buf (pull1, pull0) (pin, held);
endmodule
