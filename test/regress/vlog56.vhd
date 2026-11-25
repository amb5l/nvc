entity vlog56 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

library unisim;
use unisim.all;

architecture test of vlog56 is
    component gate is
        generic (
            WIDTH : integer := 1;
            MODE  : string := "NONE"
        );
        port (
            d : in std_logic_vector(3 downto 0);
            q : out std_logic_vector(3 downto 0)
        );
    end component;

    signal d, q : std_logic_vector(3 downto 0);
begin
    uut: component gate
        generic map (WIDTH => 4, MODE => "PASS")
        port map (d, q);

    check: process is
    begin
        d <= "1010";
        wait for 1 ns;
        assert q = "1010" report "q mismatch" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
