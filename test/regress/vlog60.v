module gate #(parameter real PERIOD = 0.0) (output ok);
  assign ok = (PERIOD == 10.5);
endmodule
