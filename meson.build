project(
  'd3d12-metal', ['c', 'cpp', 'objcpp'],
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'objcpp_std=c++20',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

if host_machine.system() != 'darwin'
  error('d3d12-metal currently builds only on macOS')
endif

cxx = meson.get_compiler('cpp')

# DirectX-Headers: use pkg-config when available, else the Homebrew layout.
directx_headers = dependency('DirectX-Headers', required: false)
if not directx_headers.found()
  dx_prefix = get_option('directx_headers_prefix')
  directx_headers = declare_dependency(
    include_directories: include_directories(
      dx_prefix / 'include',
      dx_prefix / 'include/wsl/stubs',
      dx_prefix / 'include/directx',
    ),
    link_args: ['-L' + dx_prefix / 'lib', '-lDirectX-Guids'],
  )
endif
metal_frameworks = [
  dependency('appleframeworks', modules: ['Metal', 'Foundation', 'QuartzCore']),
]
irconverter = cxx.find_library('metalirconverter', dirs: ['/usr/local/lib'])
irconverter_inc = include_directories('/usr/local/include')

bridge_sources = []
d3d12_sources = []
dxgi_sources = []
subdir('src/bridge')
subdir('src/dxgi')
subdir('src/d3d12')

# One library carrying the front-end (D3D12 + DXGI exports) and the Metal backend.
d3d12_metal = shared_library(
  'd3d12metal',
  bridge_sources, d3d12_sources, dxgi_sources,
  include_directories: [include_directories('src'), irconverter_inc],
  dependencies: [directx_headers, metal_frameworks, irconverter],
  cpp_args: ['-fvisibility=hidden'],
  objcpp_args: ['-fobjc-arc', '-fvisibility=hidden'],
  install: false,
)

subdir('tests')

# Runs the tests under Metal's API validation: meson test --setup validation
add_test_setup(
  'validation',
  env: {'MTL_DEBUG_LAYER': '1', 'METAL_DEVICE_WRAPPER_TYPE': '1'},
)
