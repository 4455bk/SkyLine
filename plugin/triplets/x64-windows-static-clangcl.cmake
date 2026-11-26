# Static x64 Windows libraries built with clang-cl, as required by SkyLine's
# musttail hooks and CommonLibSSE-NG build configuration.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PLATFORM_TOOLSET ClangCL)
