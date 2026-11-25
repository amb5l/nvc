module sub79;
  initial begin
    if ($sformatf("%m") !== "VLOG79.UUT.sub79")
      $display("FAILED %s", $sformatf("%m"));
    else
      $display("PASSED");
  end
endmodule
