entity vlog69 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog69 is
    component gate is
        generic (
            SUL    : std_ulogic := '0';
            SULV   : std_ulogic_vector(7 downto 0) := (others => '0');
            SL     : std_logic := '1';
            SLV    : std_logic_vector(3 downto 0) := (others => '0');
            PERIOD : real := 0.0;
            WIDTH  : integer := 0
        );
        port (
            ok : out std_logic
        );
    end component;

    signal ok : std_logic;
begin
    uut: component gate
        generic map (
            SUL    => '1',
            SULV   => X"3C",
            SL     => '0',
            SLV    => X"A",
            PERIOD => 10.5,
            WIDTH  => 4
        )
        port map (ok);

    check: process is
    begin
        wait for 1 ns;
        assert ok = '1' report "generic type mismatch" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
