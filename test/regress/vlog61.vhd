entity vlog61 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog61 is
    component gate is
        port (
            t   : in std_logic;
            i   : in std_logic;
            io  : inout std_logic;
            o   : out std_logic;
            bi  : in std_logic_vector(3 downto 0);
            dio : inout std_logic_vector(3 downto 0);
            bo  : out std_logic_vector(3 downto 0)
        );
    end component;

    signal t, i, io, o : std_logic;
    signal bi, dio, bo : std_logic_vector(3 downto 0);
begin
    uut: component gate
        port map (t, i, io, o, bi, dio, bo);

    check: process is
    begin
        io <= 'Z';
        dio <= (others => 'Z');
        t <= '0';
        i <= '1';
        bi <= X"A";
        wait for 1 ns;
        assert io = '1' and o = '1' report "drive out" severity failure;
        assert dio = X"A" and bo = X"A" report "bus out" severity failure;

        t <= '1';
        io <= '0';
        dio <= X"5";
        wait for 1 ns;
        assert io = '0' and o = '0' report "drive in" severity failure;
        assert dio = X"5" and bo = X"5" report "bus in" severity failure;

        io <= '1';
        dio <= X"3";
        wait for 1 ns;
        assert io = '1' and o = '1' report "drive in 2" severity failure;
        assert dio = X"3" and bo = X"3" report "bus in 2" severity failure;

        io <= 'Z';
        dio <= (others => 'Z');
        wait for 1 ns;
        assert o = 'Z' and bo = "ZZZZ" report "release" severity failure;

        report "PASSED";
        wait;
    end process;
end architecture;
