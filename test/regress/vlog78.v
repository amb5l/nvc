module leaf78;
  initial begin
    $display("leaf %m");
    if ($sformatf("%m") !== "vlog78.u")
      $display("FAILED leaf %s", $sformatf("%m"));
  end
endmodule

module vlog78;
  leaf78 u();
  if (1) begin : g
    initial begin
      if ($sformatf("%m") !== "vlog78.g")
        $display("FAILED gen %s", $sformatf("%m"));
    end
  end
  initial begin
    if ($sformatf("n=%m") !== "n=vlog78")
      $display("FAILED top %s", $sformatf("%m"));
    else
      $display("PASSED");
  end
endmodule
