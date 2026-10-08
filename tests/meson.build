test_deps = [directx_headers]
test_inc = include_directories('.', '../src')

foreach name : ['test_device', 'test_copy', 'test_texture_copy', 'test_bridge_stream', 'test_fence_event', 'test_queue_sync', 'test_rootsig']
  exe = executable(
    name, name + '.cpp',
    include_directories: test_inc,
    dependencies: test_deps,
    link_with: d3d12_metal,
  )
  test(name, exe, timeout: 60)
endforeach

# HLSL -> DXIL header arrays, one per shader stage.
dxc_option = get_option('dxc')
dxc = find_program(dxc_option != '' ? dxc_option : 'dxc', required: false)
if not dxc.found()
  error('DXC not found: put dxc on PATH or pass -Ddxc=/path/to/dxc')
endif
shader_headers = []
foreach stage : [['vs', 'vs_6_0', 'VSMain'], ['ps', 'ps_6_0', 'PSMain']]
  shader_headers += custom_target(
    'color_' + stage[0],
    input: 'shaders/color.hlsl',
    output: 'color_' + stage[0] + '.h',
    command: [dxc, '-T', stage[1], '-E', stage[2], '-Fh', '@OUTPUT@', '-Vn', 'g_color_' + stage[0], '@INPUT@'],
  )
endforeach

foreach stage : [['vs', 'vs_6_0', 'VSMain'], ['ps', 'ps_6_0', 'PSMain']]
  shader_headers += custom_target(
    'vertex_input_' + stage[0],
    input: 'shaders/vertex_input.hlsl',
    output: 'vertex_input_' + stage[0] + '.h',
    command: [dxc, '-T', stage[1], '-E', stage[2], '-Fh', '@OUTPUT@', '-Vn', 'g_vertex_input_' + stage[0], '@INPUT@'],
  )
endforeach

foreach name : ['test_triangle', 'test_cbv_table', 'test_vertex_input', 'test_render_pass']
  exe = executable(
    name, [name + '.cpp', shader_headers],
    include_directories: test_inc,
    dependencies: test_deps,
    link_with: d3d12_metal,
  )
  test(name, exe, timeout: 60)
endforeach
