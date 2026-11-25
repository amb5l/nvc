module child(output q);
  assign q = 1'b1;
endmodule

module parent(output q);
  child u(q);
endmodule
