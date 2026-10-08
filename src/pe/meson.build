# The PE half of the Wine build. Everything is in d3d12.dll (the front-end shares
# its objects between D3D12 and DXGI); dxgi.dll only forwards its exports to it.
pe_include = include_directories('../../src')

d3d12_dll = shared_library(
  'd3d12',
  d3d12_sources, dxgi_sources, bridge_client_sources,
  include_directories: [pe_include, mtlb_wine_include],
  name_prefix: '',
  vs_module_defs: 'd3d12.def',
  # winpthread is linked whole so that std::thread needs no libwinpthread DLL.
  link_args: ['-Wl,-Bstatic,--whole-archive', '-lwinpthread', '-Wl,--no-whole-archive'],
)

dxgi_dll = shared_library(
  'dxgi',
  'dxgi_forwarder.c',
  name_prefix: '',
  vs_module_defs: 'dxgi.def',
  link_with: d3d12_dll,
)
