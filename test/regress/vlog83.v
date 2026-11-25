// $time and $stime are the current time in the module time unit,
// not raw femtoseconds. IEEE 1800-2017 20.3.1 and 22.7.
//
// MMCME2_ADV records clkin_period[0] <= $time - clkin_edge in an
// integer (`timescale 1 ps / 1 ps`). A femtosecond $time is 1000 times
// too big, and once the simulation passes 2^31 fs (about 2.15 us) that
// integer wraps. The first MMCM locks while the count still fits. A
// second one, started after that, sees a nonsense period and never locks.
`timescale 1 ns / 1 ps

module vlog83;
  integer t0, dt, late, narrow;

  initial begin
    #1.4;
    if ($time !== 1 || $stime !== 1) begin
      $display("FAILED: 1.4 ns rounds to %0d / %0d", $time, $stime);
      $finish;
    end

    #0.2;
    if ($time !== 2) begin
      $display("FAILED: 1.6 ns rounds to %0d", $time);
      $finish;
    end

    t0 = $time;
    #10;
    dt = $time - t0;
    if (dt !== 10 || $time !== 12) begin
      $display("FAILED: 10 ns step measured %0d at %0d", dt, $time);
      $finish;
    end

    // 3 us is 3e9 fs, which does not fit in a signed 32-bit integer.
    #3000;
    late = $time;
    narrow = $stime;
    if (late !== 3012 || narrow !== 3012) begin
      $display("FAILED: time after 3 us is %0d / %0d", late, narrow);
      $finish;
    end

    $display("PASSED");
  end
endmodule
