entity vlog57 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

library unisim;

architecture test of vlog57 is
    component parent is
        port (
            q : out std_logic
        );
    end component;

    signal q : std_logic;
begin
    uut: component parent
        port map (q);

    check: process is
    begin
        wait for 1 ns;
        assert q = '1' report "q mismatch" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
