module gate #(
  parameter [31:0] INIT = 32'h0,
  parameter [47:0] WIDE = 48'h0,
  parameter [255:0] HUGE = 256'h0,
  parameter NARROW = 1'b0
) (
  output init_ok,
  output wide_ok,
  output huge_ok,
  output narrow_ok
);
  assign init_ok = (INIT == 32'h0000002A);
  assign wide_ok = (WIDE == 48'h00000000002A);
  assign huge_ok = (HUGE == {248'h0, 8'h2A});
  assign narrow_ok = (NARROW == 1'b1);
endmodule
