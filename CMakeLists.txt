cmake_minimum_required(VERSION 3.24)
project(coax_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COAX_BUILD_APP "Build the Windows application and libmpv adapter" ${WIN32})

include(CTest)
include(FetchContent)

# Portable policy and state model. This target deliberately has no link
# dependencies: configuring it natively is the mechanical platform-boundary
# check, rather than relying on include discipline alone.
add_library(coax_core
    src/core/playback_health.cpp
    src/core/supervisor.cpp
    src/core/supervisor_host.cpp)
target_include_directories(coax_core PUBLIC src)
target_compile_options(coax_core PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    # Immutable Catch2 v3 release used by both native and cross builds.
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.8.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)

    add_executable(coax_core_tests
        test/core/generation_tests.cpp
        test/core/playback_health_tests.cpp
        test/core/supervisor_tests.cpp
        test/core/supervisor_host_tests.cpp)
    target_link_libraries(coax_core_tests PRIVATE Catch2::Catch2WithMain coax_core)
    target_compile_options(coax_core_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(WIN32)
        target_link_options(coax_core_tests PRIVATE -static)
    endif()
    catch_discover_tests(coax_core_tests DISCOVERY_MODE PRE_TEST)
endif()

if(COAX_BUILD_APP)
    if(NOT WIN32)
        message(FATAL_ERROR
            "COAX_BUILD_APP targets Windows. Configure with the mingw toolchain:\n"
            "  cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw.cmake")
    endif()

    # Pinned libmpv (fetched by scripts/fetch-libmpv.sh).
    set(MPV_DIR "${CMAKE_SOURCE_DIR}/third_party/mpv")
    if(NOT EXISTS "${MPV_DIR}/include/mpv/client.h")
        message(FATAL_ERROR "libmpv not found. Run: ./scripts/fetch-libmpv.sh")
    endif()

    add_library(libmpv SHARED IMPORTED)
    set_target_properties(libmpv PROPERTIES
        IMPORTED_LOCATION "${MPV_DIR}/libmpv-2.dll"
        IMPORTED_IMPLIB   "${MPV_DIR}/libmpv.dll.a"
        INTERFACE_INCLUDE_DIRECTORIES "${MPV_DIR}/include")

    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
        URL_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
        DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_MakeAvailable(json)

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

    add_library(coax_player
        src/player/live_sync.cpp
        src/player/load_diagnostics.cpp
        src/player/player_event_adapter.cpp
        src/player/recovery_effect_executor.cpp
        src/player/transport_log_classifier.cpp
        src/player/mpv_player.cpp)
    target_include_directories(coax_player PUBLIC src)
    target_link_libraries(coax_player PUBLIC coax_core PRIVATE libmpv)
    target_compile_options(coax_player PRIVATE -Wall -Wextra -Wpedantic)

    if(BUILD_TESTING)
        add_executable(coax_player_adapter_tests test/player/player_event_adapter_tests.cpp)
        target_link_libraries(coax_player_adapter_tests PRIVATE
            Catch2::Catch2WithMain coax_player)
        target_compile_options(coax_player_adapter_tests PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(coax_player_adapter_tests PRIVATE -static)
        catch_discover_tests(coax_player_adapter_tests DISCOVERY_MODE PRE_TEST)
    endif()

    add_executable(coax WIN32
        src/main.cpp
        src/app/app.cpp
        src/core/channel_index.cpp
        src/xtream/xtream_client.cpp
        src/win/app_window.cpp
        src/win/composition.cpp
        src/win/credential_store.cpp
        src/win/ui_layer.cpp
        src/util/log.cpp)

    target_include_directories(coax PRIVATE src)
    target_link_libraries(coax PRIVATE
        coax_player imgui nlohmann_json
        d3d11 dxgi dxguid dcomp winhttp crypt32 shell32 ole32 uuid)
    target_compile_definitions(coax PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(coax PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    target_link_options(coax PRIVATE -municode -static)

    add_custom_command(TARGET coax POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MPV_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:coax>/libmpv-2.dll")
endif()
