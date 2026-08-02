cmake_minimum_required(VERSION 3.24)
project(coax_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR
        "coax_native targets Windows. Configure with the mingw toolchain:\n"
        "  cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake")
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Pinned libmpv (fetched by scripts/fetch-libmpv.sh)
# ---------------------------------------------------------------------------
set(MPV_DIR "${CMAKE_SOURCE_DIR}/third_party/mpv")
if(NOT EXISTS "${MPV_DIR}/include/mpv/client.h")
    message(FATAL_ERROR "libmpv not found. Run: ./scripts/fetch-libmpv.sh")
endif()

add_library(libmpv SHARED IMPORTED)
set_target_properties(libmpv PROPERTIES
    IMPORTED_LOCATION "${MPV_DIR}/libmpv-2.dll"
    IMPORTED_IMPLIB   "${MPV_DIR}/libmpv.dll.a"
    INTERFACE_INCLUDE_DIRECTORIES "${MPV_DIR}/include")

# ---------------------------------------------------------------------------
# Third-party sources
# ---------------------------------------------------------------------------
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    URL_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
    DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(json)

# The release ships a bare json.hpp; stage it under nlohmann/ so sources can
# use the canonical include path.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/staged-include/nlohmann")
file(COPY "${json_SOURCE_DIR}/json.hpp"
     DESTINATION "${CMAKE_BINARY_DIR}/staged-include/nlohmann")

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_BINARY_DIR}/staged-include")

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(imgui PUBLIC
    "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui PUBLIC d3d11 dxgi dxguid d3dcompiler dwmapi)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(coax WIN32
    src/main.cpp
    src/app/app.cpp
    src/core/channel_index.cpp
    src/xtream/xtream_client.cpp
    src/player/live_sync.cpp
    src/player/mpv_player.cpp
    src/win/app_window.cpp
    src/win/composition.cpp
    src/win/credential_store.cpp
    src/win/ui_layer.cpp
    src/util/log.cpp)

target_include_directories(coax PRIVATE src)
target_link_libraries(coax PRIVATE
    imgui
    nlohmann_json
    libmpv
    d3d11
    dxgi
    dxguid
    dcomp
    winhttp
    crypt32
    shell32
    ole32
    uuid)

target_compile_definitions(coax PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

target_compile_options(coax PRIVATE -Wall -Wextra -Wno-unused-parameter)

# -municode: the entry point is wWinMain, so mingw needs the Unicode startup.
# -static: fold libstdc++, libgcc and the mingw threading runtime into the
# executable. Without it the binary needs mingw DLLs that no Windows machine
# has, and the loader fails before main() runs.
target_link_options(coax PRIVATE -municode -static)

# Place libmpv-2.dll beside the executable so the build output is runnable.
add_custom_command(TARGET coax POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MPV_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:coax>/libmpv-2.dll")
