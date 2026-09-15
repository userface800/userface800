# UFAudioDriver — the UserFace800 AudioServerPlugIn. Standalone: fetches libASPL and produces a
# UFAudioDriver.driver bundle to install in /Library/Audio/Plug-Ins/HAL.
cmake_minimum_required(VERSION 3.20)
project(UFAudioDriver CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(libASPL GIT_REPOSITORY https://github.com/gavv/libASPL.git GIT_TAG main)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libASPL)

add_library(UFAudioDriver MODULE UFAudioDriver.cpp)
target_link_libraries(UFAudioDriver PRIVATE libASPL "-framework CoreFoundation" "-framework CoreAudio")

# Assemble the loadable bundle layout by hand (a MODULE library is just the executable).
set(BUNDLE "${CMAKE_BINARY_DIR}/UFAudioDriver.driver")
add_custom_command(TARGET UFAudioDriver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE}/Contents/MacOS"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:UFAudioDriver>" "${BUNDLE}/Contents/MacOS/UFAudioDriver"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist" "${BUNDLE}/Contents/Info.plist"
    COMMAND codesign --force --sign - "${BUNDLE}"
    COMMENT "Assembling + ad-hoc signing UFAudioDriver.driver")
