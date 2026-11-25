module vlog53;
  real r;
  reg q;
  reg attr_test;
  reg pass;

  function real id;
    input real x;
    id = x;
  endfunction

  // Called only when short-circuit evaluation fails to skip the RHS.
  function real boom;
    input real x;
    begin
      $display("FAILED: evaluated %f", x);
      boom = x;
    end
  endfunction

  initial begin
    pass = 1'b1;
    r = 5.0;
    attr_test = 1'b0;

    // Same shape as the MMCME2 attribute check: a vector test or-ed
    // with a real range test.
    if ((attr_test == 1'b1) || (r < 2.0 || r > 64.0)) begin
      $display("FAILED: in-range real or");
      pass = 1'b0;
    end

    if (!((attr_test == 1'b1) || (r < 2.0 || r > 1.0))) begin
      $display("FAILED: out-of-range real or");
      pass = 1'b0;
    end

    q = (r < 2.000 || r > 64.000);
    if (q !== 1'b0) begin
      $display("FAILED: q in-range %b", q);
      pass = 1'b0;
    end

    q = (r < 2.000 || r > 1.000);
    if (q !== 1'b1) begin
      $display("FAILED: q out-of-range %b", q);
      pass = 1'b0;
    end

    if (!(r > 1.0 && r < 10.0)) begin
      $display("FAILED: in-range real and");
      pass = 1'b0;
    end

    if (r > 1.0 && r < 2.0) begin
      $display("FAILED: out-of-range real and");
      pass = 1'b0;
    end

    // A call on the RHS forces the short-circuit lowering.
    if (!(r < 10.0 || boom(r) > 0.0)) begin
      $display("FAILED: or short-circuit result");
      pass = 1'b0;
    end

    if (r > 10.0 && boom(r) > 0.0) begin
      $display("FAILED: and short-circuit result");
      pass = 1'b0;
    end

    if (r > 10.0 || id(r) > 64.0) begin
      $display("FAILED: or evaluated false");
      pass = 1'b0;
    end

    if (!(r > 10.0 || id(r) < 64.0)) begin
      $display("FAILED: or evaluated true");
      pass = 1'b0;
    end

    if (!(r < 10.0 && id(r) < 64.0)) begin
      $display("FAILED: and evaluated true");
      pass = 1'b0;
    end

    if (r < 10.0 && id(r) > 64.0) begin
      $display("FAILED: and evaluated false");
      pass = 1'b0;
    end

    if (pass)
      $display("PASSED");
  end
endmodule
