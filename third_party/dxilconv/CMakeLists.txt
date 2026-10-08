# libdxilconv: Microsoft's DXBC -> DXIL converter (projects/dxilconv of DirectXShaderCompiler) as a macOS dylib with a
# C entry point (dxilconv_c.h). The converter sources are compiled unmodified from the DXC tree; the Windows headers
# they include are provided by shim/ on top of DXC's own WinAdapter. Driven by tools/build-dxilconv.sh, once per
# architecture (each needs the LLVM libraries of a DXC build of the same architecture).
#
#   cmake -S third_party/dxilconv -B <dir> -DDXC_SRC=<DXC checkout> -DDXC_BUILD=<DXC build dir> -DCMAKE_OSX_ARCHITECTURES=<arch>
cmake_minimum_required(VERSION 3.16)
project(dxilconv CXX)

set(DXC_SRC "" CACHE PATH "DirectXShaderCompiler checkout")
set(DXC_BUILD "" CACHE PATH "build directory of DXC for the target architecture (its lib/ holds libLLVM*.a)")
if(NOT EXISTS "${DXC_SRC}/projects/dxilconv" OR NOT EXISTS "${DXC_BUILD}/lib")
  message(FATAL_ERROR "set DXC_SRC and DXC_BUILD")
endif()

set(CONV ${DXC_SRC}/projects/dxilconv)
add_library(dxilconv SHARED
  dxilconv_c.cpp
  ${CONV}/lib/DxbcConverter/DxbcConverter.cpp
  ${CONV}/lib/DxbcConverter/DxbcUtil.cpp
  ${CONV}/lib/ShaderBinary/ShaderBinary.cpp
  ${CONV}/lib/DxilConvPasses/DxilCleanup.cpp
  ${CONV}/lib/DxilConvPasses/InitializePasses.cpp
  ${CONV}/lib/DxilConvPasses/NormalizeDxil.cpp
  ${CONV}/lib/DxilConvPasses/ScopeNestInfo.cpp
  ${CONV}/lib/DxilConvPasses/ScopeNestedCFG.cpp)

# The shim directory first: it replaces windows.h, atlbase.h, wrl/client.h and the ETW header.
target_include_directories(dxilconv BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/shim
  ${DXC_SRC}/include/llvm/llvm_assert
  ${DXC_BUILD}/include
  ${DXC_SRC}/include
  ${DXC_SRC}/external/DirectX-Headers/include/directx
  ${DXC_SRC}/external/DirectX-Headers/include/wsl/stubs
  ${CONV}/include
  ${CONV}/lib/DxbcConverter
  ${CONV}/lib/ShaderBinary
  ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dxilconv PRIVATE
  D3DX12_NO_STATE_OBJECT_HELPERS __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS __STDC_LIMIT_MACROS NDEBUG)
target_compile_options(dxilconv PRIVATE -std=gnu++17 -O2 -fPIC -fms-extensions -fvisibility=hidden
  -fvisibility-inlines-hidden -w)

set(llvm_libs
  DxilContainer DxilRootSignature DxilValidation DXIL MSSupport ScalarOpts BitWriter BitReader Analysis TransformUtils
  ipa ipo InstCombine Core DxcSupport DxilHash DxilCompression Support IRReader AsmParser)
set(libs)
foreach(name ${llvm_libs})
  list(APPEND libs ${DXC_BUILD}/lib/libLLVM${name}.a)
endforeach()
target_link_libraries(dxilconv PRIVATE ${libs})
target_link_options(dxilconv PRIVATE -Wl,-dead_strip)
set_target_properties(dxilconv PROPERTIES PREFIX "lib" SUFFIX ".dylib" INSTALL_NAME_DIR "@rpath")
