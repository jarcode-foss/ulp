# Note: all identifying application information is located in this build file
project(
  'ulp',
  'c',
  version: run_command('git', 'describe', '--tags', '--always').stdout().strip(),
  default_options:['buildtype=release', 'strip=true', 'optimization=2'])
formatted_app_name = 'Untitled Lighting Project'
app_manufacturer   = 'Jarcode'
app_author         = 'Levi Webb'
app_year           = '2021'
app_email          = 'unknown'
app_desc           = '2D Strategic Space Sim'

cc = meson.get_compiler('c')
is_standalone = get_option('standalone')

# Check for some programs used by scripts
find_program('find')
find_program('basename')
find_program('sed')
find_program('awk')
find_program('head')
find_program('bash')
iheaders = find_program('iheaders')
if host_machine.system() == 'windows'
  find_program('wixl')
  find_program('wixl-heat')
endif

if get_option('buildtype').startswith('debug')
  add_project_arguments('-DAPP_DEBUG', language: 'c')
  add_project_arguments('-ggdb', language: 'c')
else
  # Hide symbols in non-debug builds
  add_project_arguments('-fvisibility=hidden', language: 'c')
endif

static_libs = false
if host_machine.system() == 'windows'
  static_libs = true
elif host_machine.system() == 'osx'
  static_libs = true
endif

app_dependencies = [
  dependency('threads',   static: static_libs),
  dependency('libcurl',   static: static_libs),
  dependency('freetype2',   static: static_libs),
  dependency('glfw3',   static: static_libs),
  cc.find_library('atomic', static: static_libs)
]

if host_machine.system() != 'windows'
  app_dependencies += dependency('threads', static: static_libs)
endif

app_link_args = []

# Closure hack
if cc.get_id() == 'clang'
  add_project_arguments('-fblocks', language: 'c')
  if host_machine.system() == 'osx'
    app_dependencies += cc.find_library('BlocksRuntime', dirs: meson.get_cross_property('osx_library_path'))
  else
    app_dependencies += cc.find_library('BlocksRuntime')
  endif
endif

conf_prefix = get_option('confprefix')
if conf_prefix == 'auto'
  if get_option('prefix') == '/usr' or get_option('prefix') == '/usr/'
    conf_prefix = ''
  else
    conf_prefix = get_option('prefix')
  endif
endif
if not conf_prefix.endswith('/') and conf_prefix != '' and not get_option('sysconfdir').startswith('/')
  conf_prefix = conf_prefix + '/'
endif

sys_config_dir = conf_prefix + get_option('sysconfdir') + '/' + meson.project_name()
app_version = meson.project_version()
if app_version == ''
  app_version = 'unknown'
endif

if host_machine.system() == 'linux' or host_machine.system() == 'bsd'
  add_project_arguments('-DAPP_UNIX', language: ['c'])
endif

if host_machine.system() == 'windows'
  add_project_arguments('-DAPP_WIN', language: ['c'])
  # Fixes some unresolved symbols in some mingw libraries
  app_dependencies += cc.find_library('ssp', static: true)
  # Avoid prompt showing up
  app_link_args += '-Wl,-subsystem,windows'
endif

if host_machine.system() == 'osx'
  add_project_arguments('-DAPP_OSX', language: ['c'])
endif

# Note: the OSX install directives only exist for future platform support
if host_machine.system() == 'darwin'
  add_project_arguments('-DAPP_OSX', language: ['c'])
  error('OSX targets are not supported, see issue #86.')
  # sys_config_dir = '/Library/[]/'
endif

if is_standalone
  add_project_arguments('-DAPP_STANDALONE', language: ['c'])
endif

resource_dir = get_option('prefix') + '/' + get_option('datadir') + '/' + meson.project_name()
resource_dir_base = get_option('prefix') + '/' + get_option('datadir')

if host_machine.system() == 'windows'
  # Windows uses a single installation folder
  resource_dir      = './resources'
  resource_dir_base = '.'
  sys_config_dir    = '.'
elif host_machine.system() == 'osx'
  resource_dir      = './content'
  resource_dir_base = '.'
  sys_config_dir    = '.'
elif is_standalone
  # For running directly in the build directory
  resource_dir      = '../resources'
  resource_dir_base = '.'
  sys_config_dir    = '../config'
endif

# fcontext has arch-specific asm, determine the files for the host machine
if host_machine.system() == 'osx'
  asm_arch = 'combined'
  asm_ext = 'all_macho_gas.S'
else
  if host_machine.cpu_family() == 'x86'
    asm_arch = 'i386'
  elif host_machine.cpu_family() == 'ppc'
    asm_arch = 'ppc32'
  elif host_machine.cpu_family() == 'mips'
    asm_arch = 'mips32_o32'
  elif host_machine.cpu_family() == 'mips64'
    asm_arch = 'mips64_n64'
  elif host_machine.cpu_family() == 'arm'
    asm_arch = 'arm_aapcs'
  elif host_machine.cpu_family() == 'arm64'
    asm_arch = 'arm64_aapcs'
  else
    asm_arch = host_machine.cpu_family()
  endif
  
  if host_machine.system() == 'windows'
    if meson.get_compiler('c').get_id() == 'clang'
      asm_ext = 'ms_pe_clang_gas.S'
    elif meson.get_compiler('c').get_id() == 'msvc'
      asm_ext = 'ms_pe_masm.asm'
    elif meson.get_compiler('c').get_id() == 'gcc'
      asm_ext = 'ms_pe_gas.S'
    else
      error('unsupported compiler')
    endif
  else #unixes
    asm_ext = 'sysv_elf_gas.S'
  endif
endif
asm_sources = [ 'src/asm/make_'  + asm_arch + '_' + asm_ext,
                'src/asm/jump_'  + asm_arch + '_' + asm_ext,
                'src/asm/ontop_' + asm_arch + '_' + asm_ext ]
asm_missing_arch = false
foreach s: asm_sources
  if run_command('[', '-f', s, ']').returncode() != 0
    asm_missing_arch = true
    break
  endif
endforeach
if asm_missing_arch
  message('`fcontext` does not have support on the host architecture (' + host_machine.cpu_family() + '), '
          + 'using `ucontext` instead. This can severely reduce scheduler performance.')
  add_project_arguments('-DAPP_USE_SYSTEM_UCONTEXT', language: ['c'])
  asm_sources = []
endif

# Generator and target for lua objects.
# This has been written such that ninja can detect when sources need to be rebuilt.

luac_input_ext  = 'lua'
luac_output_ext = 'lua'

lua_sources = run_command(
  'find', 'src', '-type', 'f', '-name', '*.' + luac_input_ext, '-print'
).stdout().strip().split('\n')
lua_targets = []
foreach s: run_command(
  'basename', '-s.' + luac_input_ext, lua_sources
).stdout().strip().split('\n')
  lua_targets += s + '.' + luac_output_ext
endforeach


# Since we're manually locating lua(jit), we have to manually set the include argument.
lua_inc_path = '/usr/include/'
lua_lib_path = '/usr/lib/'
if host_machine.system() == 'windows'
  lua_inc_path = meson.get_cross_property('win_include_path') + '/'
  lua_lib_path = meson.get_cross_property('win_library_path') + '/'
endif
if host_machine.system() == 'osx'
  lua_inc_path = meson.get_cross_property('osx_include_path') + '/'
  lua_lib_path = meson.get_cross_property('osx_library_path') + '/'
endif

luac_args    = ['-o', '@OUTPUT@', '@INPUT@']
lua_impl     = get_option('lua_implementation')
lua_ver      = get_option('lua_version')

if lua_ver != 'auto'
  # Search for the latest lua version that satisfies the version specified
  nver_parts = lua_ver.split('.')
  collected = []
  idx = 0
  foreach part: nver_parts
    if idx == nver_parts.length() - 1
      collected += (part.to_int() + 1).to_string()
    else
      collected += (part.to_int()).to_string()
    endif
    idx = idx + 1
  endforeach
  lua_ver_next = '.'.join(collected)
  message('Search criteria: [>=' + lua_ver + ', <' + lua_ver_next + ']')
  mcheck = dependency(lua_impl, static: static_libs, required: false,
                      version: ['>=' + lua_ver, '<' + lua_ver_next])
  if mcheck.found()
    lua_ver = mcheck.version()
    message('Located packaged lua version: ' + lua_ver)
  endif
else
  # Automatically grab latest lua version from the system, even if pkg-config cannot find it.
  # This provides a hassle-free interface for building with multiple versions of lua, even
  # detecting mingw lua installations.
  
  # Try letting meson find it
  mcheck = dependency(lua_impl, static: static_libs, required: false)
  if mcheck.found()
    lua_ver = mcheck.version()
    message('Located packaged lua version: ' + lua_ver)
  else
    if host_machine.system() == 'osx'
      # OSXCross packlets have pkg-config entries, so we can just rely on that.
      # Error if that somehow failed.
      error('Cannot find Lua library')
    endif
    lua_ichecks = []
    if lua_impl == 'luajit'
      if host_machine.system() == 'windows'
        lua_ichecks = ['luajit-*.*.dll', 'luajit-*.*.dll.a', 'libluajit-*.*.dll.a']
      else
        lua_ichecks = ['libluajit-*.*.so.*']
      endif
    elif lua_impl == 'lua'
      if host_machine.system() == 'windows'
        lua_ichecks = ['lua*.dll', 'lua*.dll.a', 'liblua*.dll', 'liblua*.dll.a']
      else
        lua_ichecks = ['liblua.so.*', 'liblua*.*.so.*']
      endif
    else
      error('Failed to automatically find lua version for impl: "' + lua_impl + '". Please either ' +
            'install the corresponding package(s) or manually specify the version and/or implementation.')
    endif
    latest = '0.0.0'
    icheck_collected = []
    foreach i: lua_ichecks
      icheck_collected += run_command(
        'find', lua_lib_path, '-maxdepth', '1', '-type', 'f', '-name', i, '-print'
      ).stdout().strip().split('\n')
    endforeach
    foreach s: run_command('basename', '-a', icheck_collected).stdout().strip().split('\n')
      if s != ''
        v = '?'
        if host_machine.system() == 'windows'
          v = s.split(lua_impl)[1].split('.dll')[0]
          # `-2.0` -> `2.0`
          if v.startswith('-')
            v = v.split('-')[1]
          endif
          # `51` -> `5.1`
          if not v.contains('.')
            v = v.to_int()
            v = (v / 10).to_string() + '.' + (v % 10).to_string()
          endif
        else
          vi = s.split('.so.')
          # use version after split
          if vi.length() > 1
            v = s.split('.so.')[1]
          else
            v = '0.0.0'
          endif
        endif
        if lua_impl != 'luajit' and s.contains('luajit')
          v = '0.0.0'
        endif
        if v.version_compare('>' + latest)
          latest = v
        endif
      endif
    endforeach
    if latest == '0.0.0'
      error('Could not automatically determine lua version. Is it installed for the host?')
    else
      message('Located lua version: ' + latest)
      lua_ver = latest
    endif
  endif
endif
lua_inc      = get_option('lua_implementation') + get_option('lua_version')
vs = lua_ver.split('.')
lua_ver_s    = '.'.join([vs[0], vs[1]])
lua_sdir     = lua_impl
luac_name    = 'luac' + lua_ver_s
lua_mod_base = lua_impl + '/' + lua_ver_s
if get_option('lua_implementation') == 'luajit'
  # LuaJIT compiler produces better bytecode; use that
  luac_name = 'luajit'
  lua_impl += '-'
  luac_args = ['-b', '@INPUT@', '@OUTPUT@']
  if get_option('buildtype').startswith('debug')
    luac_args += '-g'
  endif
  # LuaJIT head compat only breaks on `x.x` changes, trim from `x.x.x`
  v = lua_ver.split('.')
  # LuaJIT include path follows `luajit-x.x` fmt instead of `lua/x.x`
  lua_inc = lua_impl + lua_ver_s
  # LuaJIT module path follows `luajit-x.x.x` fmt instead of `lua/x.x`
  lua_mod_base = lua_impl + lua_ver
  lua_sdir = lua_mod_base
  # LuaJIT is locked to 5.1 compatibility
  lua_ver_s = '5.1'
elif not get_option('buildtype').startswith('debug')
  luac_args = ['-s'] + luac_args
endif

lua_mod_path = lua_mod_base + '/' + meson.project_name()

p_prefix = get_option('prefix')
if host_machine.system() == 'windows'
  p_prefix = '[package local]'
elif is_standalone
  p_prefix = '[build root]'
endif
summary({ 'Prefix':               '"' + p_prefix          + '"',
          'Resources':            '"' + resource_dir      + '"',
          'Lua Module Path':      '"' + resource_dir_base + '/' + lua_mod_path + '"',
          'System Configuration': '"' + sys_config_dir    + '"' },
        section: '[Directories]')

add_project_arguments(
  # Pass the application name
  '-DAPP_NAME="' + meson.project_name() + '"',
  # Pass the formatted application name
  '-DAPP_FNAME="' + formatted_app_name + '"',
  # Pass the application version
  '-DAPP_VERSION="' + app_version + '"',
  # Pass the system (non user local) configuration path
  '-DAPP_SYS_CFG_PATH="' + sys_config_dir + '"',
  # Pass the resource path for other portable code (shaders) and resources
  '-DAPP_RESOURCE_PATH="' + resource_dir + '"',
  # Pass the path for loading lua modules so the stub loading code can ensure
  # that it is present in `package.path`
  '-DAPP_LUA_PATH="' + resource_dir_base + '/' + lua_mod_base + '"',
  '-DAPP_LUA_INCLUDE=' + lua_inc,
  '-DAPP_LUA_LANG_VERSION=' + lua_ver_s,
  language: ['c'])

run_command(find_program('rm'), '-rf', meson.build_root() + '/luac-out.p')
run_command(find_program('mkdir'), '-p', meson.build_root() + '/' + lua_mod_base)
depth_arr = ['..']
if lua_mod_base.contains('/')
  depth_arr = []
  foreach _: lua_mod_base.split('/')
    depth_arr += '..'
  endforeach
endif
run_command(find_program('ln'), '-sfT', '/'.join(depth_arr) + '/luac-out.p',
            meson.build_root() + '/' + lua_mod_path)

# This really hacky piece of code allows Meson to recursively track all lua
# sources and their corresponding compiled objects. Because of how generators
# work, the source tree structure has to be collapsed into a single directory;
# we resolve conflicts by preceding files with a string and building an
# association table with the old paths.
#
# This is quite useful since it allows us to avoid some platform-specific
# characteristics for handling file/directory names, and allows us to generate
# a table of files at build time.
g_prog    = find_program(luac_name)
g_output  = '@BASENAME@.' + luac_output_ext
g_args    = luac_args
objs      = []
lns       = []

foreach source: run_command(
  'find', 'src', '-type', 'f', '-name', '*.' + luac_input_ext, '-print'
).stdout().strip().split('\n')
  parts = source.split('/')
  i = 0
  sorted = []
  basename = '?'
  foreach part: parts
    if i != 0 and i != parts.length() - 1
      sorted += part
    endif
    if i == parts.length() - 1
      basename = part
    endif
    i += 1
  endforeach
  pre = '/'.join(sorted)
  if parts.length() > 2
    # Generate file prefix format for directory structure
    fmt = run_command('./scripts/lua_fmt.sh', pre, check: true).stdout().strip() + '__'
  else
    # Root directory can just use a unique prefix
    if host_machine.system() == 'windows'
      fmt = '__' # avoid conflicts with Windows filename restrictions
    else
      fmt = '' # unix filenames are quite permissive, use no prefix for root files
    endif
  endif
  fn = basename.split('.')
  if sorted.length() > 0
    pre += '/'
  endif
  lns += '["' + pre + basename + '"]="' + fmt + fn[0] + '.' + luac_output_ext + '"'
  
  objs += generator(g_prog, output: fmt + g_output, arguments: g_args).process(source)
endforeach
lua_index = '{' + (','.join(lns)) + '}'
g_p = []
foreach arg: g_args
  if arg == '@INPUT@'
    g_p += '-'
  elif arg == '@OUTPUT@'
    g_p += meson.build_root() + '/luac-out.p/index.lua'
  else
    g_p += arg
  endif
endforeach

run_command('mkdir', '-p', meson.build_root() + '/luac-out.p', check: true)
run_command('./scripts/lua_index.sh', luac_name + ' ' + ' '.join(g_p), lua_index, check: true)

# This target does two things: first, it evaluates all of our luac generators into a
# target that we can use later. The results are stored in `[build_dir]/[output].p`,
# and we update a dummy file to make ninja build dependencies to it.
#
# Second, it calls a script that removes all but the most nessecary regenerate rules
# from its own the `build.ninja` file. Meson seems to toss various files into
# regenerate rules if it thinks the `meson.build` itself depends on them, which it
# is incredibly liberal with guessing. This fixes constant regeneration caused by
# both lua and assembly sources.
luac_target = custom_target(
  'app-luac',
  input:            objs,
  output:           'luac-out',
  command:          [meson.source_root() + '/scripts/disable_regenerate.sh', meson.build_root(), 'luac-out'],
  build_by_default: true)

# This fun little hack is to deal with systems where luajit is named
# `luajit-{lua version}` or `luajit-{luajit version}`. MinGW does the
# latter, _most_ linux distributions the former. It also handles
# different naming schemes for standard lua.
lua_found = '?'
if get_option('lua_implementation') == 'luajit'
  lj_l3_available = true
  if host_machine.system() == 'osx'
    lj_l1 = cc.find_library(lua_impl + lua_ver_s, required: false,
                            dirs: meson.get_cross_property('osx_library_path'),
                            static: static_libs)
    lj_l2 = cc.find_library(lua_inc,              required: false,
                            dirs: meson.get_cross_property('osx_library_path'),
                            static: static_libs)
    lj_l3_available = false
  else
    
    # temporary fix for AUR package
    prefix = ''
    suffix = ''
    if host_machine.system() == 'windows'
      prefix = 'lib'
      suffix = '.dll.a'
    endif
    # end temporary fix
    
    lj_l1 = cc.find_library(prefix + lua_impl + lua_ver_s + suffix, required: false, static: static_libs)
    lj_l2 = cc.find_library(prefix + lua_inc + suffix,              required: false, static: static_libs)
    if static_libs == true
      lj_l3 = cc.find_library(get_option('lua_implementation'), required: false, static: true)
    else
      lj_l3_available = false
    endif
  endif
  assert(lj_l1.found() == true or lj_l2.found() == true or lj_l3.found() == true,
         'Could not locate LuaJIT for the target platform. Is it installed?')
  app_dependencies += [lj_l1, lj_l2]
  if lj_l3_available
    app_dependencies += lj_l3
  endif
  if lj_l1.found()
    lua_found = lua_impl + lua_ver_s
  endif
  if lj_l2.found()
    lua_found = lua_inc
  endif
  if lj_l3_available and lj_l3.found()
    lua_found = get_option('lua_implementation') + ' (static, version unknown)'
  endif
else
  list = [lua_impl + lua_ver_s, lua_impl + ''.join(lua_ver_s.split('.')),
          lua_impl + '-' + lua_ver_s, lua_impl]
  foreach entry: list
    if host_machine.system() == 'osx'
      ret = cc.find_library(entry, required: false,
                            dirs: meson.get_cross_property('osx_library_path')
                                  + '/lua-' + lua_ver_s, static: static_libs)
    else
      ret = cc.find_library(entry, required: false, static: static_libs)
    endif
    if ret.found()
      app_dependencies += ret
      lua_found = entry
      break
    endif
  endforeach
endif
if lua_found == '?'
  error('Failed to locate Lua library.')
endif

pre_targets = []
pre_objects = []

if host_machine.system() == 'windows'
  # Create object for executable resources (windows fmt)
  pre_targets += custom_target(
    'app-win-icon-resource',
    input: 'resources.rc',
    output: 'resources.o',
    command: [meson.source_root() + '/scripts/generate_resources.sh',
              find_program('windres').full_path(), '@INPUT@', meson.source_root(), '@OUTDIR@',
              formatted_app_name, app_version, app_manufacturer, app_author,
              meson.project_name(), app_desc],
    build_by_default: true)
  pre_objects += meson.build_root() + '/resources.o'
endif

sources = run_command(
  'find', 'src', '-type', 'f', '-name', '*.c', '-print',
  check: true
).stdout().strip().split('\n')

# Generate headers in a custom rule.
#
# Ideally, this should be integrated into the backend, but backends scan header dependencies
# before the build, so running iheaders as a `custom_target` produces inconsistent results.
# Note that iheaders itself detects changes within the header output and preserves modification
# flags to signal to ninja when the contents actually change, saving compilation time.
#
# As a result, most recompiles should use `ninja -C build iheaders && ninja -C build`

prepended_sources = []
foreach source: sources
  prepended_sources += (meson.source_root() + '/' + source)
endforeach
run_target(
  'iheaders',
  command: ['iheaders', '-d', meson.build_root() + '/headers',
            '-r', meson.source_root() + '/src', '-G', prepended_sources])

# Always generate headers on new builds
if run_command('[', '-d', meson.build_root() + '/headers', ']').returncode() != 0
  message('Generating headers for new build')
  run_command('mkdir', '-p', meson.build_root() + '/headers', check: true)
  run_command('iheaders', '-d', meson.build_root() + '/headers',
              '-r', meson.source_root() + '/src', '-G', sources)
endif

# Strip source files with `iheaders`, and collapse the source tree paths into
# filenames separated by `@` characters (meson workaround)

stripped_sources = []
foreach source: sources
  parts = source.split('/')
  i = 0
  sorted = []
  foreach part: parts
    if i != 0 and i != parts.length() - 1
      sorted += part
    endif
    i += 1
  endforeach
  pre = '@'.join(sorted)
  stripped_sources += generator(iheaders, output: pre + '@@BASENAME@.c',
                                arguments: ['-d', '@BUILD_DIR@', '-p', '-P', pre + '@', '@INPUT@']
                               ).process(source)
endforeach

unix_install_dir = 'bin'
if host_machine.system() == 'osx'
  unix_install_dir = '.'
endif

#run_command('touch', 'build/iheaders-dummy.h')
#dummy_lib = static_library('dummy', generated_headers, build_by_default: true)
#dummy_dep = declare_dependency(link_with: dummy_lib, include_directories: dummy_lib.private_dir_include())
#app_dependencies += dummy_dep

# Compile & run unit tests
tests = run_command(
  'find', 'tests', '-type', 'f', '-name', '*.c', '-print',
  check: true
).stdout().strip().split('\n')
foreach test: tests
  binary = executable(
    test.split('/')[1].split('.')[0],
    link_depends: [pre_targets, luac_target],
    link_args:    pre_objects,
    sources: [stripped_sources, asm_sources, test],
    dependencies: app_dependencies,
    include_directories: [include_directories('build/headers'),
                          include_directories('build'),
                          include_directories('src')])
  test(test.split('/')[1].split('.')[0], binary)
endforeach


exec_target = executable(
  meson.project_name(),
  link_depends: pre_targets,
  link_args:    pre_objects,
  sources:      [stripped_sources, asm_sources, 'entry/entry_wrapper.c'],
  dependencies: app_dependencies,
  include_directories: [include_directories('build/headers'),
                        include_directories('build'),
                        include_directories('src')],
  install:      true,
  install_dir:  unix_install_dir)

if host_machine.system() == 'windows'
  # Create MSI package for executable
  msi_output = meson.project_name() + '-' + meson.project_version() + '-' + host_machine.cpu_family()
  if get_option('buildtype').startswith('debug')
    msi_output += '-debug'
  endif
  msi_output += '-installer'
  custom_target(
    'app-win-msi',
    depends: [exec_target, luac_target],
    output: msi_output + '.msi',
    command: [meson.source_root() + '/scripts/generate_msi.sh', meson.source_root(),
              meson.build_root(), lua_sdir, msi_output,
              formatted_app_name, app_version, app_manufacturer, app_author,
              meson.project_name() + '.exe', app_year, meson.get_cross_property('wixl_arch'),
              host_machine.cpu_family()],
    build_by_default: true)
elif host_machine.system() == 'linux' and not is_standalone
  custom_target(
    'app-freedesktop-entry',
    input: 'prototype-entry.desktop',
    output: meson.project_name() + '.desktop',
    command: [meson.source_root() + '/scripts/generate_entry.sh',
              '@INPUT@', '@OUTDIR@',
              meson.project_name(), formatted_app_name, app_desc,
              get_option('prefix') + '/' + get_option('bindir') + '/' + meson.project_name(),
              resource_dir + '/icon.png'],
    build_by_default: true,
    install: true,
    install_dir: resource_dir_base + '/applications')
  run_command('./scripts/generate_pkgbuild.sh', meson.build_root(), meson.project_name(),
              meson.project_version(), app_desc, check: true)
  debian_arch_map = { 'x86_64' :'amd64',
                      'i686'   :'i386', # debian uses i386 as the identifier but requires i686
                      'x86'    :'i386' }
  arch_str = host_machine.cpu_family()
  arch_str = debian_arch_map.get(arch_str, arch_str)
  run_command('./scripts/generate_deb.sh', meson.build_root(), meson.project_name(),
              meson.project_version(), app_desc, app_author, app_email, app_year,
             arch_str, check: true)
elif host_machine.system() == 'osx' and not is_standalone
  run_command('./scripts/generate_pkg.sh', meson.build_root(), meson.project_name(),
              meson.project_version(), app_desc, app_author, app_email, app_year,
              app_manufacturer, formatted_app_name, check: true)
endif

if (host_machine.system() == 'osx' or host_machine.system() == 'linux') and not is_standalone
  install_subdir('config',    install_dir: sys_config_dir, strip_directory: true)
  install_subdir('resources', install_dir: resource_dir,   strip_directory: true)
  install_subdir('build/luac-out.p', install_dir: resource_dir_base + '/' + lua_mod_path,
                 strip_directory: true)
endif

summary({ 'Standalone': is_standalone,
          'Lua Library': lua_found,
          'Lua Implementation': get_option('lua_implementation'),
          'Lua Version': lua_ver,
          'Platform': host_machine.system() + ' ' + host_machine.cpu()
                      + ' (' + host_machine.endian() + ' endian)',
          'Build Type': get_option('buildtype')},
        section: '[Configuration]')
