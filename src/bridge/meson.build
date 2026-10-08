if host_system == 'darwin'
  bridge_sources += files(
    'metal/descriptors.mm',
    'metal/device.mm',
    'metal/formats.mm',
    'metal/notify.mm',
    'metal/pipeline.mm',
    'metal/queue.mm',
    'metal/swapchain.mm',
  )
endif

# The Wine unix-call transport, generated from mtlb.h (the one list of bridge
# functions). Only the Wine builds (the PE client, the unix module) need it.
if host_system == 'windows' or wine_unix
  mtlb_wine_generated = custom_target(
    'mtlb_wine_generated',
    input: 'mtlb.h',
    depend_files: meson.project_source_root() / 'tools/gen_mtlb_wine.py',
    output: ['mtlb_wine_params.h', 'mtlb_wine_client.inc', 'mtlb_wine_unix.inc'],
    command: [find_program('python3'), meson.project_source_root() / 'tools/gen_mtlb_wine.py', '@INPUT@', '@OUTDIR@'],
  )
  mtlb_wine_include = include_directories('.')  # the build directory holding the generated files

  if host_system == 'darwin'
    bridge_wine_unix_sources = files('wine/mtlb_unix.mm', 'wine/wine_window.mm') + [mtlb_wine_generated]
  else
    bridge_client_sources += files('wine/mtlb_client.cpp') + [mtlb_wine_generated]
  endif
endif
