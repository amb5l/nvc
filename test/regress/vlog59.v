module gate #(
  parameter integer WIDTH = 1,
  parameter [47:0] MASK = 48'h0,
  parameter [255:0] INIT = 256'h0,
  parameter [7:0] BITS = 8'h00,
  parameter FLAG = 1'b0
) (
  output width_ok,
  output mask_ok,
  output init_ok,
  output bits_ok,
  output flag_ok
);
  assign width_ok = (WIDTH == 4);
  assign mask_ok = (MASK == 48'h3FFFFFFFFFFF);
  assign init_ok = (INIT == {248'h0, 8'hA5});
  assign bits_ok = (BITS == 8'h5A);
  assign flag_ok = (FLAG == 1'b1);
endmodule
