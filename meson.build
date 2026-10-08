project(
  'd3d12-metal', ['c', 'cpp'],
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'objcpp_std=c++20',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

# Three kinds of build, selected by the machine file:
#   native (default)            arm64 macOS: one dylib with the whole layer, headless tests
#   cross/mingw-x86_64.txt      d3d12.dll and dxgi.dll (PE) plus the Win32 test programs
#   cross/macos-x86_64.txt      d3d12metal.so, the unix half of the PE build, run by Wine
host_system = host_machine.system()
if host_system == 'darwin'
  add_languages('objcpp', native: false)
elif host_system != 'windows'
  error('d3d12-metal builds on macOS, or cross-compiles for Windows (MinGW)')
endif
wine_unix = get_option('wine_unix')

cxx = meson.get_compiler('cpp')

# Tests that build both natively (against the dylib) and as Win32 programs (run under
# Wine): [name, vertex/pixel shaders (tests/shaders/<name>.hlsl), compute shaders].
# The source is tests/portable/p_<name>.cpp; see tests/portable/t12.h.
# Stages per shader: the default is a vertex and a pixel shader.
stages_of = {
  'geometry': [['vs', 'vs_6_0', 'VSMain'], ['gs', 'gs_6_0', 'GSMain'], ['ps', 'ps_6_0', 'PSMain']],
  'tessellation': [['vs', 'vs_6_0', 'VSMain'], ['hs', 'hs_6_0', 'HSMain'], ['ds', 'ds_6_0', 'DSMain'], ['ps', 'ps_6_0', 'PSMain']],
}

portable_tests = [
  ['interfaces', ['color'], ['noop']],
  ['textures', [], ['probe']],
  ['hello_texture', ['hello_texture', 'const_buffers'], []],
  ['compute', [], ['fill']],
  ['depth', ['color'], ['probe']],
  ['heaps', [], ['probe']],
  ['barriers', ['color'], ['probe', 'fill']],
  ['queues', [], ['fill']],
  ['large_buffers', [], ['probe']],
  ['indirect', ['color'], ['fill']],
  ['msaa', ['color'], ['msaa_read']],
  ['queries', ['color'], ['fill']],
  ['draw', ['instanced', 'color'], []],
  ['sdk', [], []],
  ['renderpass', ['color'], []],
  ['descbench', [], []],
  ['psothreads', ['instanced'], ['fill']],
  ['dxbc', [], []],
  ['rtmismatch', ['rt_mismatch'], []],
  ['geometry', ['geometry'], []],
  ['tessellation', ['tessellation'], []],
  ['embedded_rs', [], ['embedded_rs']],
]

bridge_sources = []
bridge_client_sources = []
d3d12_sources = []
dxgi_sources = []
irconverter_inc = []
subdir('src/bridge')
subdir('src/dxgi')
subdir('src/d3d12')

if host_system == 'windows'
  subdir('src/pe')
  subdir('tests/wine')
else
  metal_frameworks = [
    dependency('appleframeworks', modules: ['Metal', 'Foundation', 'QuartzCore', 'AppKit', 'ImageIO', 'CoreGraphics']),
  ]
  irconverter = cxx.find_library('metalirconverter', dirs: ['/usr/local/lib'])
  irconverter_inc = include_directories('/usr/local/include')
  # DXBC shaders go through libdxilconv (tools/build-dxilconv.sh), loaded on first use; the build tree's copy is the
  # fallback after the directory of the module itself.
  dxilconv_arch = host_machine.cpu_family() == 'x86_64' ? 'x86_64' : 'arm64'
  dxilconv_dir = meson.project_source_root() / 'build-dxilconv' / dxilconv_arch
  dxilconv_inc = include_directories('third_party/dxilconv')
  dxilconv_args = ['-DD3D12METAL_DXILCONV_DIR="' + dxilconv_dir + '"']

  if wine_unix
    # The unix half of the Wine build: the Metal backend plus the unix-call table.
    shared_library(
      'd3d12metal',
      bridge_sources, bridge_wine_unix_sources,
      include_directories: [include_directories('src'), irconverter_inc, dxilconv_inc],
      dependencies: [metal_frameworks, irconverter],
      name_prefix: '',
      name_suffix: 'so',
      cpp_args: ['-fvisibility=hidden'],
      objcpp_args: ['-fobjc-arc', '-fvisibility=hidden'] + dxilconv_args,
      link_args: ['-Wl,-install_name,@rpath/d3d12metal.so', '-Wl,-undefined,dynamic_lookup', '-Wl,-rpath,/usr/local/lib'],
    )
  else
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

    # One library carrying the front-end (D3D12 + DXGI exports) and the Metal backend.
    d3d12_metal = shared_library(
      'd3d12metal',
      bridge_sources, d3d12_sources, dxgi_sources,
      include_directories: [include_directories('src'), irconverter_inc, dxilconv_inc],
      dependencies: [directx_headers, metal_frameworks, irconverter],
      cpp_args: ['-fvisibility=hidden'],
      objcpp_args: ['-fobjc-arc', '-fvisibility=hidden'] + dxilconv_args,
      install: false,
    )

    subdir('tests')

    # Runs the tests under Metal's API validation: meson test --setup validation
    add_test_setup(
      'validation',
      env: {'MTL_DEBUG_LAYER': '1', 'METAL_DEVICE_WRAPPER_TYPE': '1'},
    )
  endif
endif
