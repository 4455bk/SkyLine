# Use the LLVM clang-cl compiler while retaining the MSVC Windows SDK/linker
# environment prepared by ilammy/msvc-dev-cmd in GitHub Actions.
set(CMAKE_C_COMPILER clang-cl CACHE STRING "")
set(CMAKE_CXX_COMPILER clang-cl CACHE STRING "")
