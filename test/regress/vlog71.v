// $sformat writes a formatted string into a reg. $sformatf returns
// one. %v prints strength, which DPHY_DIFFINBUF uses to detect Strong.

module vlog71;
  reg [23:0] text;
  reg [23:0] wide;
  reg [23:0] strength;
  reg [23:0] pulled;
  reg bitv;
  wire strong_net;
  wire pull_net;
  reg pass;

  assign strong_net = 1'b1;
  pullup (pull_net);

  initial begin
    pass = 1'b1;
    bitv = 1'b0;

    $sformat(text, "v=%0d", 7);
    if (text !== "v=7") begin
      $display("FAILED: sformat text %s", text);
      pass = 1'b0;
    end

    wide = $sformatf("%s=%0d", "n", 3);
    if (wide !== "n=3") begin
      $display("FAILED: sformatf wide %s", wide);
      pass = 1'b0;
    end

    #1;

    $sformat(strength, "%v", strong_net);
    if (strength !== "St1" || strength[23:16] !== "S") begin
      $display("FAILED: strong %s", strength);
      pass = 1'b0;
    end

    $sformat(pulled, "%v", pull_net);
    if (pulled !== "Pu1" || pulled[23:16] !== "P") begin
      $display("FAILED: pull %s", pulled);
      pass = 1'b0;
    end

    bitv = 1'b1;
    $sformat(strength, "%v", bitv);
    if (strength !== "St1") begin
      $display("FAILED: reg %s", strength);
      pass = 1'b0;
    end

    if (pass)
      $display("PASSED");
  end
endmodule
