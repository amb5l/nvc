entity vlog63 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog63 is
    component gate is
        generic (
            INIT   : integer := 0;
            WIDE   : integer := 0;
            HUGE   : integer := 0;
            NARROW : integer := 0
        );
        port (
            init_ok   : out std_logic;
            wide_ok   : out std_logic;
            huge_ok   : out std_logic;
            narrow_ok : out std_logic
        );
    end component;

    signal init_ok, wide_ok, huge_ok, narrow_ok : std_logic;
begin
    uut: component gate
        generic map (
            INIT   => 42,
            WIDE   => 42,
            HUGE   => 42,
            NARROW => 1
        )
        port map (init_ok, wide_ok, huge_ok, narrow_ok);

    check: process is
    begin
        wait for 1 ns;
        assert init_ok = '1' report "init" severity failure;
        assert wide_ok = '1' report "wide" severity failure;
        assert huge_ok = '1' report "huge" severity failure;
        assert narrow_ok = '1' report "narrow" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
