#!/bin/bash
# Builds libdxilconv.dylib (Microsoft's DXBC -> DXIL converter, from DirectXShaderCompiler's projects/dxilconv,
# University of Illinois/NCSA licence) for arm64 (the native tests) and x86_64 (the unix side under Rosetta):
#   build-dxilconv/arm64/libdxilconv.dylib      build-dxilconv/x86_64/libdxilconv.dylib
# DXBC (Shader Model 4/5) shaders are converted with it before Metal Shader Converter sees them.
#
# dxilconv is Windows-only upstream. third_party/dxilconv compiles its sources unmodified against DXC's non-Windows
# WinAdapter, with a small shim directory for the Windows headers, plus a C entry point. It links the LLVM libraries of
# a DXC build per architecture; this script configures and builds those (only the libraries it needs) when missing.
#
# Usage: tools/build-dxilconv.sh [arm64] [x86_64]       (default: both)
# Environment:
#   DXC_SRC          DirectXShaderCompiler checkout [/Users/jsp/code/deps/dxc-src]
#   DXC_BUILD_ARM64  its arm64 build tree [/Users/jsp/code/deps/dxc-build]
#   DXC_BUILD_X86_64 its x86_64 build tree [/Users/jsp/code/deps/dxc-build-x86_64]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dxc_src="${DXC_SRC:-/Users/jsp/code/deps/dxc-src}"
archs=("$@")
[ ${#archs[@]} -eq 0 ] && archs=(arm64 x86_64)
[ -d "$dxc_src/projects/dxilconv" ] || { echo "no DirectXShaderCompiler at $dxc_src (set DXC_SRC)" >&2; exit 2; }

# The LLVM libraries libdxilconv links (third_party/dxilconv/CMakeLists.txt).
llvm_targets=(LLVMDxilContainer LLVMDxilRootSignature LLVMDxilValidation LLVMDXIL LLVMMSSupport LLVMScalarOpts
              LLVMBitWriter LLVMBitReader LLVMAnalysis LLVMTransformUtils LLVMipa LLVMipo LLVMInstCombine LLVMCore
              LLVMDxcSupport LLVMDxilHash LLVMDxilCompression LLVMSupport LLVMIRReader LLVMAsmParser)

for arch in "${archs[@]}"; do
    case "$arch" in
        arm64)  dxc_build="${DXC_BUILD_ARM64:-/Users/jsp/code/deps/dxc-build}" ;;
        x86_64) dxc_build="${DXC_BUILD_X86_64:-/Users/jsp/code/deps/dxc-build-x86_64}" ;;
        *) echo "unknown architecture $arch" >&2; exit 2 ;;
    esac

    if [ ! -f "$dxc_build/build.ninja" ]; then
        echo "configuring DXC for $arch in $dxc_build"
        mkdir -p "$dxc_build"
        extra=()
        if [ "$arch" = x86_64 ]; then
            # An x86_64 llvm-tblgen would have to run under Rosetta, which hangs on freshly built command-line tools
            # here; the arm64 one produces the same output.
            arm_tblgen="${DXC_BUILD_ARM64:-/Users/jsp/code/deps/dxc-build}/bin/llvm-tblgen"
            [ -x "$arm_tblgen" ] || { echo "build the arm64 DXC first ($arm_tblgen is missing)" >&2; exit 2; }
            extra=(-DCMAKE_OSX_ARCHITECTURES=x86_64 "-DLLVM_TABLEGEN=$arm_tblgen")
        fi
        cmake -G Ninja -S "$dxc_src" -B "$dxc_build" -C "$dxc_src/cmake/caches/PredefinedParams.cmake" \
            -DCMAKE_BUILD_TYPE=Release -DENABLE_SPIRV_CODEGEN=OFF -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF \
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ${extra[@]+"${extra[@]}"}
    fi
    ninja -C "$dxc_build" "${llvm_targets[@]}"

    out="$root/build-dxilconv/$arch"
    cmake -S "$root/third_party/dxilconv" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        "-DDXC_SRC=$dxc_src" "-DDXC_BUILD=$dxc_build" "-DCMAKE_OSX_ARCHITECTURES=$arch"
    ninja -C "$out"
    echo "built $out/libdxilconv.dylib"
done
