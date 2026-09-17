# UserFace800 — top-level build.
#
# The **portable protocol core** (header-only, `uf_protocol`) and its unit tests are pure C++ (no IOKit /
# CoreAudio / CoreMIDI), so they build and test on any platform. On macOS the CLI tools and the
# daemon are built too.
#
# The **macOS platform layer**: the CLI tools (`tools/`) and `uf-daemon` are built from this cmake;
# the OHCI dext and its loader app are built with xcodebuild (`platform/loader/build-and-sign.sh`).
# The daemon owns the device and bridges audio to the AudioServerPlugIn over shared memory.
cmake_minimum_required(VERSION 3.20)
project(UserFace800 LANGUAGES CXX)

# The project targets C++23 on the host (Xcode). The portable core is deliberately written to the
# C++20 subset and built at 20 here, so it stays buildable with older toolchains; it also compiles
# clean at 23 on the host, which is what the tools and the daemon use.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# RelWithDebInfo, not Debug. uf-daemon is a realtime audio pump on a 1 kHz interrupt: an
# unoptimised build misses deadlines and glitches, which would be indistinguishable from a driver
# bug. Keeping -g means a stack trace is still readable.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# The protocol core is header-only: an INTERFACE target that just carries the include path.
add_library(uf_protocol INTERFACE)
target_include_directories(uf_protocol INTERFACE ${CMAKE_SOURCE_DIR}/include)

# macOS-only: the CLI tools and the daemon need IOKit/CoreMIDI. Elsewhere only the portable core and
# its tests are built. On the Mac, one `cmake --build build` produces the whole macOS side.
if(APPLE)
  add_subdirectory(tools)
endif()

enable_testing()
add_subdirectory(tests)
