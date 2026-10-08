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
  foreach shader : [['color', '../shaders/color.hlsl'], ['triangle', 'shaders/triangle.hlsl'],
                    ['vertex_input', '../shaders/vertex_input.hlsl'],
                    ['hello_texture', '../shaders/hello_texture.hlsl'],
                    ['const_buffers', '../shaders/const_buffers.hlsl']]
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

  executable(
    'hello_samples',
    'hello_samples.cpp', wine_shader_headers['hello_texture'], wine_shader_headers['const_buffers'],
    cpp_args: wine_test_args,
    link_args: wine_test_link_args,
  )

  # Compute shaders shared with the native tests.
  wine_compute_headers = {}
  foreach shader : ['noop', 'probe', 'fill']
    wine_compute_headers += {shader: custom_target(
      shader + '_cs',
      input: '../shaders/' + shader + '.hlsl',
      output: shader + '_cs.h',
      command: [dxc, '-T', 'cs_6_0', '-E', 'CSMain', '-Fh', '@OUTPUT@', '-Vn', 'g_' + shader + '_cs', '@INPUT@'],
    )}
  endforeach

  # The portable tests (tests/portable) as Win32 programs.
  foreach t : portable_tests
    sources = ['../portable/p_' + t[0] + '.cpp']
    foreach shader : t[1]
      sources += wine_shader_headers[shader]
    endforeach
    foreach shader : t[2]
      sources += wine_compute_headers[shader]
    endforeach
    executable(
      'p_' + t[0], sources,
      include_directories: include_directories('../portable'),
      cpp_args: wine_test_args,
      link_args: wine_test_link_args,
    )
  endforeach
endif
