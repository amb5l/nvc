package functions is
end package;

package body functions is

    function fact(n : integer) return integer is
        variable result : integer := 1;
    begin
        for i in 1 to n loop
            result := result * i;
        end loop;
        return result;
    end function;

    type int_array is array (natural range <>) of integer;

    function len(arr : int_array) return integer is
    begin
        return arr'high - arr'low + 1;
    end function;

    function get(arr : int_array; index : natural) return integer is
    begin
        return arr(index);
    end function;

    function sum(arr : int_array) return integer is
        variable result : integer := 0;
    begin
        report integer'image(arr'left);
        for i in arr'range loop
            report "loop";
            result := result + arr(i);
        end loop;
        return result;
    end function;

    procedure hello is
    begin
        report "Hello, World!";
    end procedure;

    procedure print_int(x : in integer) is
    begin
        report integer'image(x);
    end procedure;

end package body;
