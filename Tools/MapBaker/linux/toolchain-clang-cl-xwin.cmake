# Linux-hosted cross toolchain for wowdecomp/MapBaker.
#
# Uses clang-cl + lld-link with MSVC headers/libs staged by `xwin`:
#   xwin --accept-license splat --output /media/storage/toolchains/xwin
#
# Override XWIN_ROOT / the tools with -D or environment if needed.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(XWIN_ROOT "$ENV{XWIN_ROOT}" CACHE PATH "xwin splat output directory")
if (NOT XWIN_ROOT)
    set(XWIN_ROOT "/media/storage/toolchains/xwin" CACHE PATH "xwin splat output directory" FORCE)
endif ()

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_RC_COMPILER llvm-rc)

set(_xwin_crt  "${XWIN_ROOT}/crt")
set(_xwin_sdk  "${XWIN_ROOT}/sdk")
set(_xwin_includes
    "${_xwin_crt}/include"
    "${_xwin_sdk}/include/ucrt"
    "${_xwin_sdk}/include/shared"
    "${_xwin_sdk}/include/um"
    "${_xwin_sdk}/include/winrt"
    "${_xwin_sdk}/include/cppwinrt"
)
set(_xwin_libdirs
    "${_xwin_crt}/lib/x86_64"
    "${_xwin_sdk}/lib/um/x86_64"
    "${_xwin_sdk}/lib/ucrt/x86_64"
)

set(_xwin_cflags "")
foreach (_dir IN LISTS _xwin_includes)
    string(APPEND _xwin_cflags " -imsvc \"${_dir}\"")
endforeach ()

# Match the semantics of the MSVC build: no aggressive type-based aliasing, no
# FP contraction surprises, and real MSVC SEH codegen for the __try/__except sites.
# clang-cl only forwards a subset of the GCC-style flags; -Xclang is needed for the
# two cc1-only ones. -Wno-register is required because the bundled libjpeg is
# deliberately compiled as C++17 and still uses the register keyword.
set(_portable_flags "-fms-extensions -Xclang -fasync-exceptions -fno-strict-aliasing -Xclang -ffp-contract=off -fwrapv -Wno-register -Wno-c++11-narrowing")
set(CMAKE_C_FLAGS_INIT   "${_portable_flags}${_xwin_cflags}")
set(CMAKE_CXX_FLAGS_INIT "${_portable_flags}${_xwin_cflags}")

set(_xwin_linkflags "")
foreach (_dir IN LISTS _xwin_libdirs)
    string(APPEND _xwin_linkflags " /libpath:\"${_dir}\"")
endforeach ()
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_xwin_linkflags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_linkflags}")

# CMake's Windows-Clang detection: tell it we are cross compiling, and where the
# compiler/linker live. No target triple is needed; clang-cl defaults to
# x86_64-pc-windows-msvc.
set(CMAKE_CROSSCOMPILING TRUE)
