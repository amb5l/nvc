entity vlog67 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog67 is
    component gate is
        generic (
            BSIZED   : bit := '0';
            BUNSIZED : bit := '1';
            BONE     : bit_vector(0 downto 0) := "0";
            BVEC     : bit_vector(7 downto 0) := X"00";
            BWIDE    : bit_vector(47 downto 0) := (others => '0');
            BHUGE    : bit_vector(255 downto 0) := (others => '0')
        );
        port (
            ok : out std_logic
        );
    end component;

    signal ok : std_logic;
begin
    uut: component gate
        generic map (
            BSIZED   => '1',
            BUNSIZED => '0',
            BONE     => "1",
            BVEC     => X"5A",
            BWIDE    => X"00000000002A",
            BHUGE    => X"00000000000000000000000000000000000000000000000000000000000000A5"
        )
        port map (ok);

    check: process is
    begin
        wait for 1 ns;
        assert ok = '1' report "bit generic mismatch" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
