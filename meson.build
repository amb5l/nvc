project('nvc', ['c'],
        license: 'GPL3+',
        default_options : [ 'c_std=c11',
                            'buildtype=release',
                            'warning_level=1'
                          ])

add_project_arguments('-DHAVE_CONFIG_H', language : ['c'])

pkgdatadir = join_paths(get_option('datadir'), 'nvc')

compiler = meson.get_compiler('c')

libdl = compiler.find_library('dl', required : false)
libdw = compiler.find_library('dw', required : false)
libz = compiler.find_library('z')

flex = find_program('flex')

check = dependency('check')
llvm = dependency('llvm')

conf_data = configuration_data()

conf_data.set_quoted('VERSION', '1.5-devel')
conf_data.set_quoted('PACKAGE_VERSION', '1.5-devel')
conf_data.set_quoted('PACKAGE_STRING', 'nvc')
conf_data.set_quoted('PACKAGE_NAME', 'nvc')
conf_data.set_quoted('PACKAGE', 'nvc')
conf_data.set_quoted('LLVM_VERSION', llvm.version())
conf_data.set_quoted('DATADIR', join_paths(get_option('prefix'), pkgdatadir))
conf_data.set_quoted('TESTDIR', join_paths(meson.source_root(), 'test'))
conf_data.set('HAVE_SYS_PTRACE_H', compiler.has_header('sys/ptrace.h'))
conf_data.set('HAVE_SYS_SYSCTL_H', compiler.has_header('sys/sysctl.h'))
conf_data.set('HAVE_INTTYPES_H', compiler.has_header('inttypes.h'))
conf_data.set('HAVE_EXECINFO_H', compiler.has_header('execinfo.h'))
conf_data.set('HAVE_LIBDW', libdw.found())
conf_data.set('PC_FROM_UCONTEXT', 'uc_mcontext.gregs[REG_RIP]')

if host_machine.system() == 'windows'
  conf_data.set_quoted('EXEEXT', '.exe')
  conf_data.set_quoted('DLL_EXT', '.dll')
else
  conf_data.set_quoted('EXEEXT', '')
  conf_data.set_quoted('DLL_EXT', '.so')
endif  

configure_file(input : 'config.h.meson',
               output : 'config.h',
               configuration : conf_data)
config_inc = include_directories('.')

subdir('thirdparty')
subdir('src')
subdir('test')

libnvc = shared_library('nvc', [lib_src, lexer],
                        install : true,
                        dependencies : [llvm, libdl, libz, libdw],
                        link_with : [fst, fastlz, lxt, json],
                        include_directories : [src_inc, thirdparty_inc, config_inc])

nvc = executable('nvc', driver_src,
                 install : true,
                 link_with : libnvc,
                 include_directories : [src_inc, config_inc])

lib_staging = meson.build_root() / 'lib'

subdir('lib/std')
subdir('lib/ieee')

test_env = ['MESON_LIB_DIR=' + lib_staging]

unit_test = executable('unit_test', test_src,
                       dependencies : [check],
                       link_with : libnvc,
                       include_directories : [src_inc, config_inc, thirdparty_inc])
test('unit_test', unit_test, is_parallel : false, priority : 50, env : test_env)

run_regr = executable('run_regr', run_regr_src,
                      include_directories : [config_inc])
test('run_regr', run_regr, is_parallel : false, env : test_env)
