entity vlog60 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog60 is
    component gate is
        generic (
            PERIOD : real := 0.0
        );
        port (
            ok : out std_logic
        );
    end component;

    signal ok : std_logic;
begin
    uut: component gate
        generic map (PERIOD => 10.5)
        port map (ok);

    check: process is
    begin
        wait for 1 ns;
        assert ok = '1' report "period" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
