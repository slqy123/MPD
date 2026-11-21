project(
  'fooyin-tak',
  'rust',
  version: '0.2.0',
  meson_version: '>= 1.3',
  default_options: ['rust_std=2021'],
  license: 'GPL-3.0-or-later',
)

fooyin_tak = static_library(
  'fooyin_tak',
  'src/lib.rs',
  rust_abi: 'c',
)

fooyin_tak_dep = declare_dependency(
  include_directories: include_directories('include'),
  link_with: fooyin_tak,
)
