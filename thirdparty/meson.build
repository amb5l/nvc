thirdparty_inc = include_directories('.')

fastlz = static_library('fastlz', 'fastlz.c')

lxt = static_library('lxt', 'lxt_write.c',
                     include_directories : config_inc)

fst = static_library('fst', ['fstapi.c', 'lz4.c'],
                     include_directories : config_inc)

json = static_library('json', 'json.c')
