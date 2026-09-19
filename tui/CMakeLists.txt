# uf-tui — terminal front-end for the mixer and settings.
#
# A CLIENT of the daemon, like every other front-end: it reads state from the /userface800.state shm
# segment and sends commands over the control socket. It never opens a dext user client, so it
# cannot collide with the daemon inside the dext's single AT context.
#
# FTXUI is fetched, which means the first configure needs network. Behind an option so the rest of
# the build stays offline-capable — the driver itself must never depend on a download.
include(FetchContent)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v5.0.0)
FetchContent_MakeAvailable(ftxui)

add_executable(uf-tui uf-tui.cpp)
target_link_libraries(uf-tui PRIVATE uf_protocol
    ftxui::screen ftxui::dom ftxui::component)
target_compile_features(uf-tui PRIVATE cxx_std_23)
set_target_properties(uf-tui PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
