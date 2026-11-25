module dut75(input c, input d, output q);
  reg q_int = 0;
  reg q_out = 0;

  // Same shape as IDDR: a nonblocking update, a second nonblocking
  // update of that result, then a continuous assign to the port.
  always @(posedge c) q_int <= d;
  always @(q_int) q_out <= q_int;
  assign q = q_out;
endmodule
