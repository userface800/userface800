# Command-line tools. macOS only — every one of these except uf-testsig talks to the FF800 through
# the UFFireWireOHCI dext, so they need IOKit and there is nothing to build in the Linux container.
# The top-level CMakeLists guards this directory with if(APPLE) for exactly that reason.
#
# C++23 here rather than the project-wide 20: these are host-only, so the container's older
# toolchain (which is why the portable core is pinned to 20) does not constrain them.

# Binaries land in the build root, not build/tools/ — `./build/uf-daemon` is the path in every note,
# script and habit we have, and moving it would silently run a stale copy for anyone who missed it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

function(uf_add_tool name)
    cmake_parse_arguments(T "" "" "FRAMEWORKS" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE uf_protocol)
    target_compile_features(${name} PRIVATE cxx_std_23)
    foreach(fw IN LISTS T_FRAMEWORKS)
        target_link_libraries(${name} PRIVATE "-framework ${fw}")
    endforeach()
endfunction()

# Transport tools: open the dext user client and drive the device.
foreach(t uf-capture uf-flash uf-init uf-probe uf-set uf-status uf-mix)
    uf_add_tool(${t} FRAMEWORKS IOKit CoreFoundation)
endforeach()

# Pure generator — writes a self-describing test signal to a WAV, no device involved.
uf_add_tool(uf-testsig)

# These two live under platform/ but are built here, so that one `cmake --build build` produces
# the whole macOS side.
add_executable(uf-daemon ${CMAKE_SOURCE_DIR}/platform/daemon/uf-daemon.cpp)
target_link_libraries(uf-daemon PRIVATE uf_protocol
    "-framework IOKit" "-framework CoreFoundation" "-framework CoreMIDI")
target_compile_features(uf-daemon PRIVATE cxx_std_23)

add_executable(uf-midi ${CMAKE_SOURCE_DIR}/platform/midi/uf-midi.cpp)
target_link_libraries(uf-midi PRIVATE uf_protocol
    "-framework IOKit" "-framework CoreFoundation" "-framework CoreMIDI")
target_compile_features(uf-midi PRIVATE cxx_std_23)
