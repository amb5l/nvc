std_standard = custom_target('std_standard',
                             input : 'standard.vhd',
                             output : 'STD.STANDARD',
                             command : [nvc, '--work', '@OUTDIR@', '--force-init',
                                        '-a', '--bootstrap', '@INPUT@'],
                             build_by_default : true)

std_textio = custom_target('std_textio',
                           input : 'textio.vhd',
                           output : 'STD.TEXTIO',
                           depends : [std_standard],
                           command : [nvc, '--work', '@OUTDIR@', '-a', '@INPUT@'],
                           build_by_default : true)
