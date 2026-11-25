// IDDR drives Q from a reg that has both a procedural assign/deassign
// (a force source) and a nonblocking assignment. The zero-delay NBA
// has to update before time advances, and the continuous copy of that
// reg has to reach the port in the same timestep.
//
// IEEE 1800-2017 4.5: the NBA region runs before time advances.
// IEEE 1800-2017 10.4.2: a later timestep observes the update.
// A #0 is the inactive region, still before that NBA, so it sees the
// previous value. One later timestep sees the new port value.
`timescale 1 ns / 1 ps

module vlog85;
  reg c = 1'b0;
  reg d = 1'b0;
  reg qi;
  reg qo = 1'b0;
  wire q = qo;

  always @(posedge c)
    qi <= d;

  always @(qi)
    qo <= qi;

  initial begin
    assign qi = 1'b0;
    #1;
    deassign qi;

    d = 1'b1;
    #5;
    c = 1'b1;
    #0;
    if (q !== 1'b0) begin
      $display("FAILED: #0 saw %b", q);
      $finish;
    end
    #5;
    if (q !== 1'b1) begin
      $display("FAILED: one timestep saw %b", q);
      $finish;
    end
    $display("PASSED");
  end
endmodule
