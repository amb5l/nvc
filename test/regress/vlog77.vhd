entity vlog77 is
end entity;

library ieee;
use ieee.std_logic_1164.all;

architecture test of vlog77 is
    component upout77 is
        port (o : out std_logic);
    end component;
    component downout77 is
        port (o : out std_logic);
    end component;
    component ipull77 is
        port (pin : inout std_logic; seen : out std_logic);
    end component;
    component idown77 is
        port (pin : inout std_logic; seen : out std_logic);
    end component;
    component iweak77 is
        port (pin : inout std_logic; seen : out std_logic);
    end component;
    component ifight77 is
        port (pin : inout std_logic; seen : out std_logic);
    end component;
    component ibuf77 is
        port (
            i  : in std_logic;
            t  : in std_logic;
            io : inout std_logic;
            o  : out std_logic
        );
    end component;
    component vpull77 is
        port (pin : inout std_logic_vector(1 downto 0));
    end component;
    component bibuf77 is
        port (a, b : inout std_logic);
    end component;
    component keep77 is
        port (pin : inout std_logic);
    end component;

    signal up_o, down_o : std_logic;
    signal pu, pu_s, pd, pd_s : std_logic;
    signal wk, wk_s, ft, ft_s : std_logic;
    signal bi, bt, bio, bo : std_logic;
    signal vp : std_logic_vector(1 downto 0);
    signal ba, bb : std_logic;
    signal kp : std_logic;
begin
    u_up: component upout77 port map (up_o);
    u_dn: component downout77 port map (down_o);
    u_pu: component ipull77 port map (pu, pu_s);
    u_pd: component idown77 port map (pd, pd_s);
    u_wk: component iweak77 port map (wk, wk_s);
    u_ft: component ifight77 port map (ft, ft_s);
    u_io: component ibuf77 port map (bi, bt, bio, bo);
    u_vp: component vpull77 port map (vp);
    u_bb: component bibuf77 port map (ba, bb);
    u_kp: component keep77 port map (kp);

    check: process is
    begin
        pu <= 'Z';
        pd <= 'Z';
        wk <= 'Z';
        ft <= 'Z';
        bio <= 'Z';
        vp <= "ZZ";
        ba <= 'Z';
        bb <= 'Z';
        kp <= 'Z';
        bi <= '1';
        bt <= '0';
        wait for 1 ns;
        assert up_o = 'H' report "pullup out" severity failure;
        assert down_o = 'L' report "pulldown out" severity failure;
        assert pu = 'H' and pu_s = '1' report "pull release" severity failure;
        assert pd = 'L' and pd_s = '0' report "down release" severity failure;
        assert wk = 'H' and wk_s = '1' report "weak release" severity failure;
        assert ft = '1' and ft_s = '1' report "strong release" severity failure;
        assert bio = '1' and bo = '1' report "buf drive" severity failure;
        assert vp = "LH" report "vector pull" severity failure;

        pu <= '0';
        pd <= '1';
        wk <= '0';
        bio <= '0';
        ba <= '1';
        kp <= '1';
        wait for 1 ns;
        assert pu = '0' and pu_s = '0' report "pull vs 0" severity failure;
        assert pd = '1' and pd_s = '1' report "down vs 1" severity failure;
        assert wk = '0' and wk_s = '0' report "weak vs 0" severity failure;
        assert bio = 'X' and bo = 'U' report "buf fight" severity failure;
        assert ba = '1' and bb = 'H' report "bibuf copy" severity failure;
        assert kp = '1' report "keeper drive 1" severity failure;

        pu <= 'Z';
        wk <= 'Z';
        ft <= '0';
        bt <= '1';
        bio <= 'Z';
        bb <= '0';
        kp <= 'Z';
        wait for 1 ns;
        assert pu = 'H' report "pull again" severity failure;
        assert wk = 'H' report "weak again" severity failure;
        assert ft = 'X' and ft_s = 'U' report "strong fight" severity failure;
        assert bio = 'Z' and bo = 'Z' report "buf off" severity failure;
        assert ba = '1' and bb = '0' report "bibuf hold" severity failure;
        assert kp = 'H' report "keeper holds 1" severity failure;

        bt <= 'X';
        bio <= 'Z';
        kp <= '0';
        wait for 1 ns;
        assert bio = 'Z' report "buf X enable" severity failure;
        assert kp = '0' report "keeper drive 0" severity failure;

        kp <= 'Z';
        wait for 1 ns;
        assert kp = 'L' report "keeper holds 0" severity failure;

        report "PASSED";
        wait;
    end process;
end architecture;
