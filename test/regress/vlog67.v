module gate #(
  parameter [0:0] BSIZED = 1'b0,
  parameter BUNSIZED = 1'b1,
  parameter [0:0] BONE = 1'b0,
  parameter [7:0] BVEC = 8'h00,
  parameter [47:0] BWIDE = 48'h0,
  parameter [255:0] BHUGE = 256'h0
) (
  output ok
);
  assign ok = (BSIZED == 1'b1)
           && (BUNSIZED == 1'b0)
           && (BONE == 1'b1)
           && (BVEC == 8'h5A)
           && (BWIDE == 48'h00000000002A)
           && (BHUGE == {248'h0, 8'hA5});
endmodule
