# uf-daemon — owns the FF800 through the OHCI dext and bridges its audio to the AudioServerPlugIn.
cmake_minimum_required(VERSION 3.20)
project(uf_daemon CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(uf-daemon uf-daemon.cpp)
target_include_directories(uf-daemon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_link_libraries(uf-daemon PRIVATE "-framework IOKit" "-framework CoreFoundation")
