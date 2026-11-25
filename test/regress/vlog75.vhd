entity vlog75 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog75 is
    component dut75 is
        port (
            c : in std_logic;
            d : in std_logic;
            q : out std_logic
        );
    end component;

    signal c, d, q : std_logic := '0';
begin
    uut: component dut75
        port map (c, d, q);

    check: process is
    begin
        d <= '1';
        wait for 1 ns;
        c <= '1';

        -- Zero-delay resumes are still in the active region, before
        -- the nonblocking updates. One later timestep sees the port.
        wait for 0 ns;
        wait for 0 ns;
        assert q = '0' report "visible during a zero-delay resume"
            severity failure;

        wait for 5 ns;
        assert q = '1' report "not visible after one timestep"
            severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
