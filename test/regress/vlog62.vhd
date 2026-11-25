entity vlog62 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog62 is
    component gate is
        generic (
            WIDTH : integer := 4;
            MASK  : std_logic_vector(47 downto 0) := (others => '0');
            BITS  : std_logic_vector(7 downto 0) := X"A5";
            FLAG  : std_logic_vector(0 downto 0) := (others => '0');
            MODE  : string := "RAM"
        );
        port (
            ok : out std_logic
        );
    end component;

    signal ok : std_logic;
begin
    -- No generic map: non-literal defaults are left open by simplification.
    uut: component gate
        port map (ok);

    check: process is
    begin
        wait for 1 ns;
        assert ok = '1' report "default generic mismatch" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
