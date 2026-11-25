# Upstream consolidation plan

Baseline: nickg/nvc `master` at `75da1dd4` ("Fix Windows installer GitHub action").
Fork tip reviewed: amb5l/nvc `unisim/22-ddr-settle` at `93408868e`.
Range: `75da1dd4..93408868e` is 33 commits, 95 files, +4038 / −209.
Source only: 32 files, +2016 / −193. The largest source files are `src/elab.c` (+641), `src/vlog/vlog-lower.c` (+387), `src/vlog/vlog-parse.c` (+260), `src/vpi/vpi-systf.c` (+253), and `src/rt/model.c` (+253).

Nothing in this document should be opened against nickg/nvc until the sequence below is agreed. The fork PRs stay on amb5l/nvc.

## What Nick would actually receive

One tracking issue, plus one small PR per item below, rebased onto `75da1dd4` and squashed to a single commit. Each PR's regression is the minimal source in that item (or the same source split into the `.v` / `.vhd` pair the test harness wants). Do not send the fork's intermediate commits, and do not keep the fork's `vlog53`–`vlog85` numbers: nickg `master` already uses `vlog1`–`vlog52` with `vlog51` absent, so number each new test `vlog53` upward in the order these PRs land.

`elab.c` is touched by the library, generic, and root-loading work. Land those three in the order U10, U11, U14 so each rebase is a small conflict instead of a rewrite.

## Drop, fold, or do not send

| Item | Why |
| --- | --- |
| amb5l/nvc#7 (`cursor/mmcm-jit-crash-b1bb`) | Superseded. Its real-`||` fix and `$realtime` are U01 and U07. The other two commits only renumber tests around the fork. |
| `8c3929060` "rise delay only" as its own PR | Incomplete. Fall and turn-off were parsed and ignored. U06 is the finished behaviour. Keep `vlog54` only as an extra case inside U06. |
| `40d35746c` as its own PR | First half of the same-time order work. U15 is the stable event queue (`f158a2452`), which also fixes several instances posting `#1` in one timestep. |
| `605ab9ed8` as its own PR | Three-line assert fix, no test of its own. Fold into U12. |
| `db5f0080e` | Test harness only (`test/run_regr.c`, 3 lines): a reheated run must see the same `-L` as analysis. Fold into U10. |
| Windows installer CI | `git describe` falls back to a raw SHA and WiX rejects `Product/@Version`. Not a product change. Do not upstream a workaround. |
| macOS arm64 cancellations | The job was cancelled. The other macOS run on the same PRs passed. |
| Specify path delays such as `(C => Q1) = (100:100:100)` | Parsed after U03. Not lowered and not applied. Defer. Do not describe U03 as timing simulation. |
| ISERDESE1 memory-mode Q2 = X | Correct IEEE 1800-2017 behaviour. No PR. See the note after U18. |

## Command lines

Every reproducer below is self-contained and has no UNISIM or Xilinx source. Unless a case says otherwise:

```
nvc --std=2008 -a repro.v -e repro -r
```

Mixed cases:

```
nvc --std=2008 -a repro.v repro.vhd -e repro -r
```

"Actual" is nickg `master` at `75da1dd4`. "Expected" is what the consolidated PR must print.

---

## U01 — Real `||` / `&&` during reset

Fork: `30ee058db` (the operator half only; the variable-delay half belongs to U05).

A short-circuit `||` stored its result in a 1-bit logic temporary. A real comparison lowers to a bool, so a process of the MMCM attribute-check shape aborted in `mir_build_store` while it was being reset.

```verilog
module repro;
  real r;
  reg flag;
  initial begin
    r = 5.0;
    flag = 1'b0;
    if ((flag == 1'b1) || (r < 2.0 || r > 64.0))
      $display("FAILED");
    else
      $display("PASSED");
  end
endmodule
```

Actual: elaboration aborts in `mir_build_store` while resetting the initial process.
Expected: `PASSED`.

Regression: that module. About 80 lines in `vlog-lower.c`, plus the test. No dependency. Land this first; later Verilog tests do not reset if this crashes.

## U02 — Trailing comma in a task argument list

Fork: `b4dde0f21`. Test today: `vlog53`.

```verilog
module repro;
  initial begin
    $display("hello",);
    $display("PASSED");
  end
endmodule
```

Actual: parse error on the trailing comma.
Expected: `hello` then `PASSED`.

About 10 lines in `vlog-parse.c`. No dependency.

## U03 — Specify part-selects and min:typ:max syntax

Fork: `a238b1cb2`. Test today: `specify6`.

```verilog
module repro;
  reg clk = 0, d = 0;
  reg [7:0] a = 8'ha5;
  wire [7:0] y = a;
  specify
    (a[1:1] *> y[7:0]) = (0:0:0, 0:0:0);
    $setuphold(posedge clk, posedge d, 0:0:0, 0:0:0);
  endspecify
  initial begin
    #1;
    if (y !== 8'ha5) $display("FAILED");
    else $display("PASSED");
  end
endmodule
```

Actual: parse error on the part-select or the `0:0:0` limit.
Expected: `PASSED`. The path delay is not applied; `y` changes in zero time. Say that in the PR.

About 40 lines, parser only. U19 depends on this.

## U04 — Real literals are not event sensitivity

Fork: `edb4f8643`. Test today: `vlog55`.

```verilog
module repro;
  reg q = 0;
  initial begin
    q = (1.0 > 0.0);
    #1;
    if (q !== 1'b1) $display("FAILED %b", q);
    else $display("PASSED");
  end
endmodule
```

Actual: the implicit sensitivity list tries to treat `1.0` as a net and analysis fails.
Expected: `PASSED`.

About 20 lines in `vlog-lower.c`. No dependency.

## U05 — Scale delay expressions by the time unit

Fork: `abc6c0aff`, `c6d27f136`, and the `vlog_lower_time` half of `30ee058db`. Tests today: `vlog80`, `vlog81`, `vlog65`.

A bare `#N`, a parameter, and any other expression were a femtosecond count. `` `timescale 1 ps / 1 ps `` and `#(100000)` slept 100 ps. A time literal and a parsed number are already scaled; scaling them again is wrong. IEEE 1800-2017 22.7.

```verilog
`timescale 1 ps / 1 ps
module repro;
  parameter ROC = 100000;
  real tap = 200.0;
  integer n = 3;
  initial begin
    #(ROC);
    if ($realtime != 100000.0) $display("FAILED param %f", $realtime);
    #(tap * n);
    else if ($realtime != 100600.0) $display("FAILED product %f", $realtime);
    else $display("PASSED");
  end
endmodule
```

Actual: the first wait ends at `$realtime == 100` (100 ps). The product is 600 fs.
Expected: `PASSED` (100 ns, then 600 ps later).

About 150 lines across `vlog-parse.c` and `vlog-lower.c`. Depends on U01 only so the real multiply lowers. U06 depends on this so a numeric delay is not scaled twice.

## U06 — Rise, fall, and turn-off on a continuous assign

Fork: `7e6783825` (replaces `8c3929060`). Test today: `vlog82`. Also keep the single-delay cases from `vlog54`.

```verilog
`timescale 1 ps / 1 ps
module repro;
  reg rst = 1'b0;
  wire locked;
  assign #(100000, 0) locked = rst ? 1'b0 : 1'b1;
  initial begin
    #1;
    if (locked !== 1'bx) $display("FAILED initial %b", locked);
    else begin
      #100000;
      if (locked !== 1'b1) $display("FAILED rise %b", locked);
      else begin
        rst = 1'b1;
        #1;
        if (locked !== 1'b0) $display("FAILED fall %b", locked);
        else $display("PASSED");
      end
    end
  end
endmodule
```

To X the delay is `min(rise, fall)` (IEEE 1800-2017 28.16), so the wire is X at 1 ps, not still Z. Actual on master: the fall uses 100000 ps as well, so 1 ps after `rst` rises `locked` is still 1. Expected: `PASSED`.

About 240 lines in `vlog-parse.c` and `vlog-lower.c`. Depends on U05.

## U07 — `$realtime`, `$time`, and `$stime` in the module time unit

Fork: `11e9c004f`, `9174ced09`. Tests today: `vlog66`, `vlog83`. `test/regress/gold/ivtest2.txt` changes with them (the old gold is a femtosecond count).

IEEE 1800-2017 20.3.1 and 22.7. `$time` and `$stime` returned raw femtoseconds. An integer `$time - edge` under `` `timescale 1 ps / 1 ps `` overflows signed 32 bits at about 2.15 µs, which is why a second MMCM released after the first has run never locks. The first can still lock from its edge counter.

```verilog
`timescale 1 ns / 1 ps
module repro;
  integer t0, dt, late;
  initial begin
    #1.4;
    if ($time !== 1) $display("FAILED round %0d", $time);
    else begin
      t0 = $time;
      #10;
      dt = $time - t0;
      #3000;
      late = $time;
      if (dt !== 10 || late !== 3012)
        $display("FAILED dt=%0d late=%0d", dt, late);
      else
        $display("PASSED");
    end
  end
endmodule
```

Actual: `$time` at 1.4 ns is `1400000`, `dt` is `10000000`, and `late` does not fit the integer the MMCM uses. `$realtime` is a separate missing system function (parse / unknown task). Expected: `PASSED`. Round half up: 1.6 ns is `$time == 2`.

About 160 lines in `vlog-parse.c`, `vpi-systf.c`, and the well-known-identifier table, plus the ivtest2 gold update. Independent of U05. Can land in parallel with U02–U04.

## U08 — Arrayed primitive gates

Fork: `a91c7a6f2`. Test today: `gate3`.

```verilog
module repro;
  wire [1:0] y;
  reg [1:0] a = 2'b10;
  buf bufs[1:0] (y, a);
  initial begin
    #1;
    if (y !== 2'b10) $display("FAILED %b", y);
    else $display("PASSED");
  end
endmodule
```

Actual: arrayed gate instance is rejected or the bits are not connected.
Expected: `PASSED`.

About 260 lines, mostly instantiation. Independent.

## U09 — Continuous assign to a hierarchical net

Fork: `5d71c2486`. Test today: `hier1` (`hier1.v`, two tops).

```verilog
module child(inout q);
endmodule
module repro;
  wire q;
  child u(q);
  assign u.q = 1'b1;
  initial begin
    #1;
    if (q !== 1'b1) $display("FAILED %b", q);
    else $display("PASSED");
  end
endmodule
```

```
nvc --std=2008 -a repro.v -e repro -r
```

Actual: the hierarchical assign is rejected.
Expected: `PASSED`.

About 70 lines. Independent of U10 if the target is an instance in the same design.

## U10 — Modules in another library, nested instances, qualified top

Fork: `eb90932f6`, `7a379f7ab`, `fcc3c3f77`, and the harness line in `db5f0080e`. Tests today: `vlog56`, `vlog57`, `vlog58`. Use a library name such as `cells`, not UNISIM.

```verilog
module leaf(output reg q);
  initial q = 1'b1;
endmodule
module wrap(output q);
  leaf u(q);
endmodule
```

```verilog
module repro;
  wire q;
  wrap u(q);
  initial begin
    #1;
    if (q !== 1'b1) $display("FAILED %b", q);
    else $display("PASSED");
  end
endmodule
```

```
nvc --std=2008 --work=cells -a cells.v
nvc --std=2008 -L . -a repro.v -e repro -r
```

Actual: `wrap` is not found, or the nested `leaf` is sought in `work` instead of `cells`. A second form, `nvc -e cells.wrap`, is rejected as a top name.
Expected: `PASSED`, and `-e cells.wrap` elaborates.

About 330 lines, mostly `elab.c` and `nvc.c`, plus 3 lines in `test/run_regr.c`. Land before U11, U14, U21, and U22.

## U11 — VHDL generics mapped onto Verilog parameters

Fork: `cf1ec25b5`, `754b37300`, `17b56b63f`, `07f38d0fe`. Tests today: `vlog59`, `vlog60`, `vlog62`, `vlog63`, `vlog67`, `vlog68`, `vlog69`.

This is one mapping in `elab.c`. Splitting it by type produces six PRs that conflict on the same function. Keep it as one PR. It is the largest item (about 700 lines, most of the `elab.c` growth).

```verilog
module dut #(parameter integer W = 1,
             parameter real R = 0.0,
             parameter [7:0] B = 8'h00,
             parameter integer FLAG = 0);
  initial $display("W=%0d R=%f B=%h FLAG=%0d", W, R, B, FLAG);
endmodule
```

```vhdl
entity repro is end;
architecture test of repro is
  component dut is
    generic (W : integer; R : real; B : bit_vector(7 downto 0); FLAG : boolean);
    port (dummy : out bit);
  end component;
  signal dummy : bit;
begin
  u: dut generic map (W => 8, R => 1.5, B => "10101010", FLAG => true)
    port map (dummy);
end;
```

```
nvc --std=2008 -a dut.v repro.vhd -e repro -r
```

An open generic (`generic map (others => open)` or a missing association) must use the Verilog default rather than failing.

Actual: integer, real, vector, bit, or boolean generic is rejected, or an open generic is an error.
Expected: `W=8 R=1.500000 B=aa FLAG=1` (format may match `$display`, the values must match).

Depends on U10 only if a later test instantiates across a library. The mapping itself can land on a module in `work`.

## U12 — VHDL inout bound to Verilog inout

Fork: `04eb4a72e`, plus `605ab9ed8` (the pending-driver assert, which has no test of its own). Test today: `vlog61`.

```verilog
module dut(inout q);
  assign q = 1'b1;
endmodule
```

```vhdl
library ieee;
use ieee.std_logic_1164.all;
entity repro is end;
architecture test of repro is
  component dut is port (q : inout std_logic); end component;
  signal q : std_logic;
begin
  u: dut port map (q);
  check: process begin
    wait for 1 ns;
    assert q = '1' report "q=" & std_logic'image(q) severity failure;
    report "PASSED";
    wait;
  end process;
end;
```

```
nvc --std=2008 -a dut.v repro.vhd -e repro -r
```

Actual: the inout port is rejected, or a debug build asserts in `put_driving` because both directions of the conversion update a signal whose effective update is still queued.
Expected: `PASSED` in both release and debug builds.

About 260 lines. U13 depends on it.

## U13 — Drive strength across an inout

Fork: `0c3d350b1`. Test today: `vlog77`.

```verilog
module pull(inout q);
  assign (pull0, pull1) q = 1'b1;
endmodule
module strong(inout q);
  assign (strong0, strong1) q = 1'b0;
endmodule
```

```vhdl
library ieee;
use ieee.std_logic_1164.all;
entity repro is end;
architecture test of repro is
  component pull is port (q : inout std_logic); end component;
  component strong is port (q : inout std_logic); end component;
  signal a, b : std_logic;
begin
  p: pull port map (a);
  s: strong port map (b);
  check: process begin
    wait for 1 ns;
    assert a = 'H' and b = '0'
      report "a=" & std_logic'image(a) & " b=" & std_logic'image(b)
      severity failure;
    report "PASSED";
    wait;
  end process;
end;
```

Actual: the pull driver arrives as a strong `'1'` (or the strengths are dropped and resolution is wrong).
Expected: `PASSED` (`'H'` from the pull, `'0'` from the strong driver).

About 480 lines. This is the riskiest behavioural change: strength encoding on the VHDL/Verilog boundary and resolved nets. Depends on U12. Review the resolution cases in `vlog77` rather than shrinking the test below the pull-vs-strong pair.

## U14 — Elaborate every root, including a second top such as glbl

Fork: `51b9df412`. Test today: `vlog76`.

```verilog
module glbl;
  reg flag = 1'b0;
  initial begin
    flag = 1'b1;
  end
endmodule
module dut(output q);
  assign q = glbl.flag;
endmodule
```

```vhdl
entity repro is end;
architecture test of repro is
  component dut is port (q : out bit); end component;
  signal q : bit;
begin
  u: dut port map (q);
  check: process begin
    wait for 1 ns;
    assert q = '1' report "glbl did not run" severity failure;
    report "PASSED";
    wait;
  end process;
end;
```

```
nvc --std=2008 -a tops.v repro.vhd -e repro glbl -r
```

Actual: `glbl` is not elaborated, so `q` stays `'0'`. Passing `-e repro glbl` is either rejected or the second root's initial process never runs.
Expected: `PASSED`.

About 150 lines in `nvc.c` / `elab.c`. Depends on U10 (a hierarchical reference to another root).

## U15 — Same-time events run in the order they were scheduled

Fork: `40d35746c` and `f158a2452` together. Tests today: `vlog73`, `vlog74`. Also updates `test/regress/gold/vhpi1.txt`.

IEEE 1800 does not define the order of distinct processes that resume at the same time. The event queue ordered by time only, so heap order moved between runs and between instances. Several cells that post `#1` from an earlier block and read it from a later block then see X. This PR makes equal times keep insertion order. Call that out as a quality-of-implementation choice, not an LRM requirement. A VHPI callback registered first at a given time now runs first; that is the gold-file change.

```verilog
module repro;
  reg trig = 0;
  reg [3:0] bin = 4'bx;
  integer seen = 0;
  initial #1 trig = 1;
  always @(trig) #1 bin = 4'd4;
  always @(trig) #1 seen = (bin === 4'd4) ? 1 : 0;
  initial begin
    #3;
    if (seen !== 1) $display("FAILED bin=%b", bin);
    else $display("PASSED");
  end
endmodule
```

Actual: `FAILED` (the reader runs first, `bin` is still X). With three copies of the module the posts interleave and the result depends on heap order.
Expected: `PASSED`, including three instances elaborated together.

About 120 lines, mostly `src/rt/heap.c` and `src/rt/model.c`. Independent, but it changes event order for every language, so run the VHDL suite and `vhpi1` on the rebased branch. This is the PR most likely to need a discussion with Nick.

## U16 — A Verilog port update is visible to VHDL after one timestep

Fork: `e8b9d04c1`. Test today: `vlog75`.

IEEE 1800-2017 4.5 and 10.4.2. The NBA region is after the active region. A VHDL `wait for 0 ns` resumes in the active region and must still see the old value. The next non-zero timestep must see the port.

```verilog
module dut(input c, input d, output q);
  reg qi = 0, qo = 0;
  always @(posedge c) qi <= d;
  always @(qi) qo <= qi;
  assign q = qo;
endmodule
```

```vhdl
library ieee;
use ieee.std_logic_1164.all;
entity repro is end;
architecture test of repro is
  component dut is port (c, d : in std_logic; q : out std_logic); end component;
  signal c, d, q : std_logic := '0';
begin
  u: dut port map (c, d, q);
  check: process begin
    d <= '1';
    wait for 1 ns;
    c <= '1';
    wait for 0 ns;
    assert q = '0' report "visible during a zero-delay resume" severity failure;
    wait for 5 ns;
    assert q = '1' report "not visible after one timestep" severity failure;
    report "PASSED";
    wait;
  end process;
end;
```

Actual on master, before this stack: one 5 ns step still reads `'0'`.
Expected: `PASSED`.

About 50 lines in `src/rt/model.c`. Independent of U17. Land U16 first; U17 is the case where the reg also has a force source.

## U17 — Zero-delay NBA onto a net that also has a force source

Fork: `93408868e`. Test today: `vlog85`.

IDDR holds `q1_out_int` with a procedural `assign` until GSR falls, then `deassign`s it and uses a zero-delay NBA. That net has a force source and a deposit. The deposit is applied on the driving heap, which is drained at the start of a cycle. Posted from the NBA region, it did not schedule another delta, so the value waited until the next timed event. One 5 ns step still saw the old port. A second step saw the new one.

```verilog
`timescale 1 ns / 1 ps
module repro;
  reg c = 0, d = 0, qi, qo = 0;
  wire q = qo;
  always @(posedge c) qi <= d;
  always @(qi) qo <= qi;
  initial begin
    assign qi = 1'b0;
    #1 deassign qi;
    d = 1'b1;
    #5 c = 1'b1;
    #0;
    if (q !== 1'b0) $display("FAILED #0 saw %b", q);
    else begin
      #5;
      if (q !== 1'b1) $display("FAILED one step saw %b", q);
      else $display("PASSED");
    end
  end
endmodule
```

Actual: `FAILED one step saw 0`.
Expected: `PASSED`. `#0` is the inactive region and must still read 0.

About 60 lines in `src/rt/model.c`. Depends on U16 only in the sense that both touch the simulation cycle; the diffs are adjacent but small. Rebase U17 onto U16.

## U18 — An always block sees a wire's initial drive

Fork: `ffd071031`. Test today: `vlog84`.

The initial drive is applied while processes are still being reset, and the event was stamped with the init iteration. An `always @(wire)` that starts waiting at time 0 never ran. IEEE 1800-2017 4.5 and 9.2.1.

```verilog
`timescale 1 ns / 1 ps
module repro;
  reg d = 1'b0;
  wire din = d;
  reg mid = 1'bx, copied = 1'bx;
  always @(din) mid <= din;
  always @(mid) copied <= mid;
  initial begin
    #1;
    if (mid !== 1'b0 || copied !== 1'b0)
      $display("FAILED %b %b", mid, copied);
    else
      $display("PASSED");
  end
endmodule
```

Actual: `FAILED x x`.
Expected: `PASSED`.

About 100 lines in `src/rt/model.c` (`x_enable_trigger` only; VHDL waits do not use this path). Independent.

### Not a bug: ISERDESE1 Q2 stays X

Do not open an issue for this. The model declares `o_delay_pre_fb = 0` and leaves `data_in` uninitialised. Storing 0 into a variable that already holds 0 is not an event (IEEE 1800-2017 10.4.2). `always @(o_delay_pre_fb)` keeps waiting (9.4.2). An uninitialised `reg` is X (6.8). The sampler therefore reads X until `o_delay_pre_fb` later changes. The VHDL model initialises that signal to 0.

The regression for U18 should also lock this, so a later change does not "fix" it:

```verilog
wire same = 1'b0;
reg held = 1'b0, sampled = 1'bx;
always @(same) held <= same;
always @(held) sampled <= held;
// after #1, sampled must still be 1'bx
```

Actual and expected are the same: `sampled` is X. No code change beyond U18.

## U19 — Timing-check delayed nets are zero-delay copies

Fork: `3aed15d7e`. Test today: `vlog70`. IEEE 1800-2017 31.9. Depends on U03.

```verilog
module repro;
  reg clk = 0, d = 0;
  wire clk_d, d_d;
  specify
    $setuphold(posedge clk, posedge d, 0:0:0, 0:0:0, , , , clk_d, d_d);
  endspecify
  initial begin
    #1 d = 1;
    #1;
    if (d_d !== 1'b1) $display("FAILED %b", d_d);
    else $display("PASSED");
  end
endmodule
```

Actual: the delayed nets are undriven, or the `$setuphold` form with them is rejected.
Expected: `PASSED`. This still does not apply a path delay. It only copies the data onto the delayed net.

About 160 lines.

## U20 — `$sformat` and `$sformatf`

Fork: `c153e3041`. Test today: `vlog71`.

```verilog
module repro;
  reg [8*16-1:0] buf;
  initial begin
    $sformat(buf, "n=%0d", 7);
    if (buf != "n=7") $display("FAILED");
    else if ($sformatf("n=%0d", 7) != "n=7") $display("FAILED f");
    else $display("PASSED");
  end
endmodule
```

Actual: unknown system task.
Expected: `PASSED`.

About 270 lines in `vpi-systf.c`. Independent. The string comparison in the regression can be a `$display` of the result if the packed-string compare is awkward; the PR must show the formatted text.

## U21 — `defparam`

Fork: `a6d29f952`. Test today: `vlog72`. Depends on U10 if the target is hierarchical across a library; the minimal case is one module.

```verilog
module child;
  parameter P = 0;
  initial $display("P=%0d", P);
endmodule
module repro;
  child u();
  defparam u.P = 4;
endmodule
```

Actual: `defparam` is ignored or rejected, and the output is `P=0`.
Expected: `P=4`.

About 140 lines. Land after U10 and U11 so parameter override and generic mapping do not fight over the same field.

## U22 — `%m` prints the instance path

Fork: `7347fc8a9`. Tests today: `vlog78`, `vlog79`.

```verilog
module child;
  initial $display("%m");
endmodule
module repro;
  child u();
endmodule
```

Actual: `%m` prints nothing useful, or `%m` is not implemented.
Expected: `repro.u`.

About 100 lines. Depends on U10 for the instance path. A mixed case (VHDL parent, Verilog child) is worth a second check in the same PR; the pure case above is the regression.

---

## Order, size, and what to watch

| Order | PR | Fork commits folded in | Depends on | Src lines, rough | Risk |
| --- | --- | --- | --- | --- | --- |
| 1 | U01 real `\|\|` | part of `30ee058db` | — | ~80 | Low. Blocks later Verilog tests. |
| 2 | U02 trailing comma | `b4dde0f21` | — | ~10 | Low. |
| 3 | U03 specify syntax | `a238b1cb2` | — | ~40 | Low if the PR says path delays are not applied. |
| 4 | U04 real sensitivity | `edb4f8643` | — | ~20 | Low. |
| 5 | U05 delay scale | `abc6c0aff`, `c6d27f136`, delay half of `30ee058db` | U01 | ~150 | Medium. Easy to double-scale a number. |
| 6 | U06 rise/fall/turn-off | `7e6783825` (not `8c3929060` alone) | U05 | ~240 | Medium. 28.16 X/Z cases. |
| 7 | U07 `$time` / `$realtime` | `11e9c004f`, `9174ced09` | — | ~160 | Medium. ivtest2 gold changes. |
| 8 | U08 arrayed gates | `a91c7a6f2` | — | ~260 | Low. |
| 9 | U09 hierarchical assign | `5d71c2486` | — | ~70 | Low. |
| 10 | U10 libraries | `eb90932f6`, `7a379f7ab`, `fcc3c3f77`, `db5f0080e` | — | ~330 | Medium. `elab.c`. |
| 11 | U11 generics | `cf1ec25b5`, `754b37300`, `17b56b63f`, `07f38d0fe` | U10 | ~700 | Highest line count. One function, do not split. |
| 12 | U12 inout | `04eb4a72e`, `605ab9ed8` | — | ~260 | Medium. Debug assert. |
| 13 | U13 strengths | `0c3d350b1` | U12 | ~480 | Highest behavioural risk. |
| 14 | U14 second root | `51b9df412` | U10 | ~150 | Low once U10 exists. |
| 15 | U15 event order | `40d35746c`, `f158a2452` | — | ~120 | Changes every language. QoI, not LRM. |
| 16 | U16 port NBA | `e8b9d04c1` | — | ~50 | Low. |
| 17 | U17 force-source NBA | `93408868e` | U16 | ~60 | Low. Extra delta only when the heap defers. |
| 18 | U18 time-0 always | `ffd071031` | — | ~100 | Low. Verilog level triggers only. |
| 19 | U19 delayed nets | `3aed15d7e` | U03 | ~160 | Low. Not path delays. |
| 20 | U20 `$sformat` | `c153e3041` | — | ~270 | Low. |
| 21 | U21 `defparam` | `a6d29f952` | U10, U11 | ~140 | Medium. Override vs generic. |
| 22 | U22 `%m` | `7347fc8a9` | U10 | ~100 | Low. |

U02, U03, U04, U07, U08, U09, U15, U16, U18, and U20 do not depend on each other and can be reviewed in parallel. Do not merge them out of the `elab.c` order U10 → U11 → U14, and do not merge U06 before U05, U13 before U12, U17 before U16, U19 before U03, or U21 before U11.

## Fork CI, for the record

On amb5l PRs #26, #27, and #28 every job passed except the Windows installer (raw SHA) and, on #26 and #28, a cancelled macOS arm64 job. A second macOS run on those branches passed. PRs #29–#31 (`$time`, time-0 always, zero-delay deposit) were still running when this file was written. The full `run_regr` suite passed locally on `93408868e`.

## Prototypes

No `upstream/01-...` branch was cut. Each item above already passes on the fork tip as the named `vlogNN` test. The consolidated branch should cherry-pick the listed commits onto `75da1dd4`, drop the folded and superseded ones, and replace the regression with the source in this file.
