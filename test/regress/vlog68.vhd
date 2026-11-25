entity vlog68 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog68 is
    component gate is
        generic (
            STR_T : boolean := FALSE;
            STR_F : boolean := TRUE;
            INT_T : boolean := FALSE;
            INT_F : boolean := TRUE;
            VEC_T : boolean := FALSE;
            VEC_F : boolean := TRUE;
            BIT_T : boolean := FALSE
        );
        port (
            ok : out std_logic
        );
    end component;

    signal ok : std_logic;
begin
    uut: component gate
        generic map (
            STR_T => TRUE,
            STR_F => FALSE,
            INT_T => TRUE,
            INT_F => FALSE,
            VEC_T => TRUE,
            VEC_F => FALSE,
            BIT_T => TRUE
        )
        port map (ok);

    check: process is
    begin
        wait for 1 ns;
        assert ok = '1' report "boolean generic mismatch" severity failure;
        report "PASSED";
        wait;
    end process;
end architecture;
