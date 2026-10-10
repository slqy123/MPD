project('cuecheck', 'cpp', default_options: ['cpp_std=c++23', 'warning_level=2'])

executable('cuecheck',
  'main.cxx',
  '../src/tag/Tag.cxx',
  '../src/tag/Builder.cxx',
  '../src/tag/Pool.cxx',
  '../src/tag/Settings.cxx',
  '../src/tag/ParseName.cxx',
  '../src/tag/Names.cxx',
  '../src/tag/FixString.cxx',
  '../src/util/UTF8.cxx',
  '../src/util/StringStrip.cxx',
  include_directories: include_directories('../src'),
  dependencies: dependency('threads'),
  install: false,
)

python = find_program('python3')
test('cuecheck', python, args: [files('test.py'), meson.current_build_dir() / 'cuecheck'])
