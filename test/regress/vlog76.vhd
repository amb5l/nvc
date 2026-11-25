entity vlog76 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog76 is
    component leaf76 is
        port (hit : out std_logic);
    end component;

    signal hit : std_logic;
begin
    uut: component leaf76
        port map (hit);

    check: process is
    begin
        -- glbl holds GSR for 10 ns. The leaf records that pulse.
        wait for 12 ns;
        assert hit = '1' report "glbl.GSR was not visible" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
