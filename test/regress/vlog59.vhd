entity vlog59 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog59 is
    component gate is
        generic (
            WIDTH : integer := 1;
            MASK  : std_logic_vector(47 downto 0) := (others => '0');
            INIT  : std_ulogic_vector(255 downto 0) := (others => '0');
            BITS  : bit_vector(7 downto 0) := (others => '0');
            FLAG  : std_logic := '0'
        );
        port (
            width_ok : out std_logic;
            mask_ok  : out std_logic;
            init_ok  : out std_logic;
            bits_ok  : out std_logic;
            flag_ok  : out std_logic
        );
    end component;

    signal width_ok, mask_ok, init_ok, bits_ok, flag_ok : std_logic;
begin
    uut: component gate
        generic map (
            WIDTH => 4,
            MASK  => X"3FFFFFFFFFFF",
            INIT  => X"00000000000000000000000000000000000000000000000000000000000000A5",
            BITS  => X"5A",
            FLAG  => '1'
        )
        port map (width_ok, mask_ok, init_ok, bits_ok, flag_ok);

    check: process is
    begin
        wait for 1 ns;
        assert width_ok = '1' report "width" severity failure;
        assert mask_ok = '1' report "mask" severity failure;
        assert init_ok = '1' report "init" severity failure;
        assert bits_ok = '1' report "bits" severity failure;
        assert flag_ok = '1' report "flag" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
