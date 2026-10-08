# Win32 test programs, run under Wine by tools/run-wine-tests.sh.
wine_test_args = ['-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX']
wine_test_link_args = ['-ld3d12', '-ldxgi', '-luser32', '-lgdi32']

executable(
  'swapchain_test',
  'swapchain_test.cpp',
  cpp_args: wine_test_args,
  link_args: wine_test_link_args,
)

# The programs with shaders need DXC (HLSL -> DXIL header arrays g_<name>_vs and g_<name>_ps).
dxc_option = get_option('dxc')
dxc = find_program(dxc_option != '' ? dxc_option : 'dxc', required: false)
if not dxc.found()
  warning('DXC not found (put dxc on PATH or pass -Ddxc=/path/to/dxc): wine_basic and hello_triangle are not built')
else
  wine_shader_headers = {}
  foreach shader : [['color', '../shaders/color.hlsl'], ['triangle', 'shaders/triangle.hlsl']]
    headers = []
    foreach stage : [['vs', 'vs_6_0', 'VSMain'], ['ps', 'ps_6_0', 'PSMain']]
      headers += custom_target(
        shader[0] + '_' + stage[0],
        input: shader[1],
        output: shader[0] + '_' + stage[0] + '.h',
        command: [dxc, '-T', stage[1], '-E', stage[2], '-Fh', '@OUTPUT@', '-Vn', 'g_' + shader[0] + '_' + stage[0], '@INPUT@'],
      )
    endforeach
    wine_shader_headers += {shader[0]: headers}
  endforeach

  executable(
    'wine_basic',
    'wine_basic.cpp', wine_shader_headers['color'],
    cpp_args: wine_test_args,
    link_args: wine_test_link_args,
  )

  executable(
    'hello_triangle',
    'hello_triangle.cpp', wine_shader_headers['triangle'],
    cpp_args: wine_test_args,
    link_args: wine_test_link_args,
  )
endif
