# UserFace800 — top-level build.
#
# This CMakeLists builds ONLY the **portable protocol core** (header-only, `uf_protocol`) and its
# unit tests. That core is pure C++ (no IOKit / CoreAudio / CoreMIDI) and therefore builds
# and tests on Linux — this is what the TDD loop drives.
#
# The **macOS platform layer**: the CLI tools (`tools/`) and `uf-daemon` are built from this cmake;
# the OHCI dext and its loader app are built with xcodebuild (`platform/loader/build-and-sign.sh`).
# The daemon owns the device and bridges audio to the AudioServerPlugIn over shared memory.
cmake_minimum_required(VERSION 3.20)
project(UserFace800 LANGUAGES CXX)

# The project targets C++23 on the host (Xcode). The container toolchain (clang-14 / gcc-12)
# only fully supports C++20, so the portable core is written to the C++20 subset and built at
# 20 here; it also compiles clean at 23 on the host.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# The protocol core is header-only: an INTERFACE target that just carries the include path.
add_library(uf_protocol INTERFACE)
target_include_directories(uf_protocol INTERFACE ${CMAKE_SOURCE_DIR}/include)

enable_testing()
add_subdirectory(tests)
