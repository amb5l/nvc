std_logic_1164 = custom_target('std_logic_1164',
                               input : 'std_logic_1164.vhdl',
                               output : 'IEEE.STD_LOGIC_1164',
                               depends : [std_standard],
                               command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
                                          '-a', '@INPUT@'],
                               build_by_default : true)

std_logic_1164_body = custom_target('std_logic_1164_body',
                                    input : 'std_logic_1164-body.vhdl',
                                    output : 'IEEE.STD_LOGIC_1164-body',
                                    depends : [std_logic_1164],
                                    command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
                                               '-a', '@INPUT@'],
                                    build_by_default : true)

numeric_std = custom_target('numeric_std',
                            input : 'numeric_std.vhdl',
                            output : 'IEEE.NUMERIC_STD',
                            depends : [std_logic_1164],
                            command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
                                       '-a', '@INPUT@'],
                            build_by_default : true)

numeric_std_body = custom_target('numeric_std_body',
                                 input : 'numeric_std-body.vhdl',
                                 output : 'IEEE.NUMERIC_STD-body',
                                 depends : [numeric_std],
                                 command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
                                            '-a', '@INPUT@'],
                                 build_by_default : true)

numeric_bit = custom_target('numeric_bit',
                            input : 'numeric_bit.vhdl',
                            output : 'IEEE.NUMERIC_BIT',
                            depends : [std_standard],
                            command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
                                       '-a', '@INPUT@'],
                            build_by_default : true)

numeric_bit_body = custom_target('numeric_bit_body',
                                 input : 'numeric_bit-body.vhdl',
                                 output : 'IEEE.NUMERIC_BIT-body',
                                 depends : [numeric_bit],
                                 command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
                                            '-a', '@INPUT@'],
                                 build_by_default : true)

math_real = custom_target('math_real',
                          input : 'math_real.vhdl',
                          output : 'IEEE.MATH_REAL',
                          depends : [std_standard],
                          command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
                                     '-a', '@INPUT@'],
                          build_by_default : true)

math_real_body = custom_target('math_real_body',
                               input : 'math_real-body.vhdl',
                               output : 'IEEE.MATH_REAL-body',
                               depends : [math_real],
                               command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
                                          '-a', '@INPUT@'],
                               build_by_default : true)

# std_logic_arith = custom_target('std_logic_arith',
#                                input : '../synopsys/std_logic_arith.vhd',
#                                output : 'IEEE.STD-LOGIC_ARITH',
#                                depends : [std_logic_1164],
#                                command : [nvc, '-L', lib_staging, '--work', '@OUTDIR@',
#                                           '-a', '@INPUT@'],
#                                build_by_default : true)
