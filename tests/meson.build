test_deps = [directx_headers]
# Every test gets a shader cache directory of its own (never the user's, never shared between parallel tests).
test_cache_root = meson.current_build_dir() / 'shader-cache'
test_inc = include_directories('.', '../src')

# HLSL -> DXIL header arrays, one per shader stage.
dxc_option = get_option('dxc')
dxc = find_program(dxc_option != '' ? dxc_option : 'dxc', required: false)
if not dxc.found()
  error('DXC not found: put dxc on PATH or pass -Ddxc=/path/to/dxc')
endif

# Shader name -> [vs header, ps header], from shaders/<name>.hlsl.
shader_headers = {}
foreach shader : ['color', 'vertex_input', 'hello_texture', 'const_buffers', 'instanced']
  headers = []
  foreach stage : [['vs', 'vs_6_0', 'VSMain'], ['ps', 'ps_6_0', 'PSMain']]
    headers += custom_target(
      shader + '_' + stage[0],
      input: 'shaders/' + shader + '.hlsl',
      output: shader + '_' + stage[0] + '.h',
      command: [dxc, '-T', stage[1], '-E', stage[2], '-Fh', '@OUTPUT@', '-Vn', 'g_' + shader + '_' + stage[0], '@INPUT@'],
    )
  endforeach
  shader_headers += {shader: headers}
endforeach

# Compute shaders: shaders/<name>.hlsl -> g_<name>_cs.
compute_headers = {}
foreach shader : ['noop', 'probe', 'fill', 'msaa_read']
  compute_headers += {shader: custom_target(
    shader + '_cs',
    input: 'shaders/' + shader + '.hlsl',
    output: shader + '_cs.h',
    command: [dxc, '-T', 'cs_6_0', '-E', 'CSMain', '-Fh', '@OUTPUT@', '-Vn', 'g_' + shader + '_cs', '@INPUT@'],
  )}
endforeach

# [test, shaders it includes]
tests = [
  ['test_device', []],
  ['test_copy', []],
  ['test_texture_copy', []],
  ['test_bridge_stream', []],
  ['test_fence_event', []],
  ['test_foreign_objects', []],
  ['test_queue_sync', []],
  ['test_rootsig', []],
  ['test_descriptors', []],
  ['test_hardening', []],
  ['test_backend_ranges', []],
  ['test_shader_cache', ['instanced'], ['fill']],
  ['test_triangle', ['color']],
  ['test_cbv_table', ['color']],
  ['test_render_pass', ['color']],
  ['test_vertex_input', ['vertex_input']],
]
foreach t : tests
  sources = [t[0] + '.cpp']
  foreach shader : t[1]
    sources += shader_headers[shader]
  endforeach
  foreach shader : t.length() > 2 ? t[2] : []
    sources += compute_headers[shader]
  endforeach
  exe = executable(
    t[0], sources,
    include_directories: test_inc,
    dependencies: test_deps,
    link_with: d3d12_metal,
  )
  test(t[0], exe, timeout: 60, env: {'D3D12METAL_CACHE_DIR': test_cache_root / t[0]})
  if t[0] == 'test_shader_cache'
    shader_cache_exe = exe
    foreach mode : ['disabled', 'evict']  # the same program in two more modes (see its header)
      test('test_shader_cache_' + mode, exe, args: [mode], timeout: 120, env: {'D3D12METAL_CACHE_DIR': test_cache_root / ('shader_cache_' + mode)})
    endforeach
  endif
endforeach

foreach t : portable_tests
  sources = ['portable/p_' + t[0] + '.cpp']
  foreach shader : t[1]
    sources += shader_headers[shader]
  endforeach
  foreach shader : t[2]
    sources += compute_headers[shader]
  endforeach
  exe = executable(
    'p_' + t[0], sources,
    include_directories: [test_inc, include_directories('portable')],
    dependencies: test_deps,
    link_with: d3d12_metal,
  )
  test('p_' + t[0], exe, timeout: 120, env: {'D3D12METAL_CACHE_DIR': test_cache_root / t[0]})
endforeach

# The swap chain test: its Objective-C half (a layer standing in for a window) is a separate file.
test_swapchain = executable(
  'test_swapchain', 'test_swapchain.cpp', 'swapchain_support.mm',
  include_directories: test_inc,
  dependencies: [test_deps, dependency('appleframeworks', modules: ['QuartzCore', 'ImageIO', 'CoreGraphics', 'Foundation', 'Metal'])],
  objcpp_args: ['-fobjc-arc'],
  link_with: d3d12_metal,
)
test('test_swapchain', test_swapchain, timeout: 60, env: {'D3D12METAL_CACHE_DIR': test_cache_root / 'swapchain'})
