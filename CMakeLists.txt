cmake_minimum_required(VERSION 3.24)
# The version has one home, and release-please owns it: the release pull
# request rewrites the line below, so it is never bumped by hand. CPack names
# the artifacts from it and the update check compiles it into the binary.
# The trailing annotation has to stay on the same line as the version -- the
# generic updater matches per line and rewrites only what it finds there, so
# a marker on its own line silently updates nothing.
project(coax_native VERSION 1.1.0 LANGUAGES CXX) # x-release-please-version

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
    src/core/presentation.cpp
    src/core/settings.cpp
    src/core/supervisor.cpp
    src/core/supervisor_host.cpp
    src/core/version.cpp
    src/util/log_ring.cpp)
target_include_directories(coax_core PUBLIC src)
target_compile_options(coax_core PRIVATE -Wall -Wextra -Wpedantic)

# The half of the player layer that has no engine in it: event correlation,
# load diagnostics, the live-offset control law and the log classifier. It sits
# above coax_core because it speaks the player's vocabulary -- which coax_core
# must not -- but it links only that portable core, so the native shell
# configures and tests it exactly as it does the core. Only
# src/player/mpv_player.cpp needs libmpv, and that is the whole reason this is a
# separate target.
add_library(coax_player_core
    src/player/live_sync.cpp
    src/player/load_diagnostics.cpp
    src/player/playback_observability.cpp
    src/player/playback_session.cpp
    src/player/player_event_adapter.cpp
    src/player/session_target_registry.cpp
    src/player/timeline_recovery.cpp
    src/player/transport_log_classifier.cpp)
target_include_directories(coax_player_core PUBLIC src)
target_link_libraries(coax_player_core PUBLIC coax_core)
target_compile_options(coax_player_core PRIVATE -Wall -Wextra -Wpedantic)

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
        test/core/playback_power_tests.cpp
        test/core/presentation_tests.cpp
        test/core/settings_tests.cpp
        test/core/supervisor_tests.cpp
        test/core/supervisor_host_tests.cpp
        test/core/version_tests.cpp
        test/util/log_ring_tests.cpp)
    target_link_libraries(coax_core_tests PRIVATE Catch2::Catch2WithMain coax_core)
    target_compile_options(coax_core_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(WIN32)
        target_link_options(coax_core_tests PRIVATE -static)
    endif()
    catch_discover_tests(coax_core_tests DISCOVERY_MODE PRE_TEST)

    # Built and run by the same native configuration as the core suite. These
    # cases predate the target split and passed the whole time -- they were
    # simply linked into a Windows executable no runner could execute.
    add_executable(coax_player_core_tests
        test/player/live_sync_tests.cpp
        test/player/player_core_tests.cpp
        test/player/recovery_integration_tests.cpp)
    target_link_libraries(coax_player_core_tests PRIVATE
        Catch2::Catch2WithMain coax_player_core)
    target_compile_options(coax_player_core_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(WIN32)
        target_link_options(coax_player_core_tests PRIVATE -static)
    endif()
    catch_discover_tests(coax_player_core_tests DISCOVERY_MODE PRE_TEST)
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

    # The engine adapter, and the only translation unit under src/player/ that
    # needs libmpv or Windows. Everything portable is in coax_player_core.
    add_library(coax_mpv src/player/mpv_player.cpp)
    target_include_directories(coax_mpv PUBLIC src)
    target_link_libraries(coax_mpv PUBLIC coax_player_core PRIVATE libmpv)
    # The swap chain is a COM object, so the adapter that owns its reference
    # pulls in <unknwn.h>. These keep that from dragging in the rest of Win32
    # and its min/max macros along with it.
    target_compile_definitions(coax_mpv PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(coax_mpv PRIVATE -Wall -Wextra -Wpedantic)

    # Icon and version metadata. Generated into the build tree so the version
    # has exactly one source of truth: the project() call above.
    enable_language(RC)
    set(COAX_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/coax.ico")
    configure_file(src/win/coax.rc.in "${CMAKE_BINARY_DIR}/coax.rc" @ONLY)

    add_executable(coax WIN32
        "${CMAKE_BINARY_DIR}/coax.rc"
        src/main.cpp
        src/app/app.cpp
        src/app/theme.cpp
        src/app/widgets.cpp
        src/app/update_check.cpp
        src/core/channel_index.cpp
        src/xtream/xtream_client.cpp
        src/util/http.cpp
        src/win/app_paths.cpp
        src/win/app_window.cpp
        src/win/composition.cpp
        src/win/credential_store.cpp
        src/win/power_request.cpp
        src/win/settings_store.cpp
        src/win/ui_layer.cpp
        src/util/log.cpp)

    target_include_directories(coax PRIVATE src)
    target_link_libraries(coax PRIVATE
        coax_mpv imgui nlohmann_json
        d3d11 dxgi dxguid dcomp dwmapi winhttp crypt32 shell32 ole32 uuid)
    target_compile_definitions(coax PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        # The running build's own version, for the update check to compare
        # against and for the HTTP user agent. Wide form for WinHttpOpen.
        COAX_VERSION="${PROJECT_VERSION}"
        COAX_VERSION_W=L"${PROJECT_VERSION}")
    target_compile_options(coax PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    target_link_options(coax PRIVATE -municode -static)

    add_custom_command(TARGET coax POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MPV_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:coax>/libmpv-2.dll")

    # ---------------------------------------------------------------------
    # Packaging
    #
    # The distributable is two files: coax.exe links everything but libmpv
    # statically, and every other import is a stock Windows DLL. A RelWithDebInfo
    # coax.exe carries ~25 MiB of DWARF, so the package ships a stripped binary
    # and a separate .debug sidecar rather than choosing between a fat download
    # and unsymbolisable crashes. The build tree keeps the unstripped binary:
    # ./build/coax.exe stays directly runnable and debuggable.
    # ---------------------------------------------------------------------
    set(COAX_DEBUG_FILE "${CMAKE_BINARY_DIR}/coax.debug")

    # Splitting runs at install time rather than as a POST_BUILD step so the
    # ordinary edit-build loop does not pay for it. Both components run it, so
    # each is independently packageable.
    set(COAX_SPLIT_DEBUG "
        execute_process(
            COMMAND \"${CMAKE_OBJCOPY}\" --only-keep-debug
                    \"$<TARGET_FILE:coax>\" \"${COAX_DEBUG_FILE}\"
            COMMAND_ERROR_IS_FATAL ANY)")

    install(TARGETS coax RUNTIME DESTINATION . COMPONENT runtime)
    install(FILES "${MPV_DIR}/libmpv-2.dll" DESTINATION . COMPONENT runtime)
    install(FILES README.md LICENSE DESTINATION . COMPONENT runtime)
    # Which libmpv build this package was linked against, carried alongside it.
    install(FILES "${MPV_DIR}/PINNED.txt"
            DESTINATION . RENAME libmpv-PINNED.txt COMPONENT runtime)

    install(CODE "${COAX_SPLIT_DEBUG}" COMPONENT runtime)
    install(CODE "
        set(_staged \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/coax.exe\")
        execute_process(
            COMMAND \"${CMAKE_STRIP}\" --strip-all \"\${_staged}\"
            COMMAND_ERROR_IS_FATAL ANY)
        # objcopy reads the sidecar to checksum it, but records only its base
        # name, so a debugger finds coax.debug next to the executable. The flag
        # and its value are one argv entry, hence the assembled variable.
        set(_debuglink \"--add-gnu-debuglink=${COAX_DEBUG_FILE}\")
        execute_process(
            COMMAND \"${CMAKE_OBJCOPY}\" \"\${_debuglink}\" \"\${_staged}\"
            COMMAND_ERROR_IS_FATAL ANY)
        " COMPONENT runtime)

    install(CODE "${COAX_SPLIT_DEBUG}" COMPONENT debug)
    install(FILES "${COAX_DEBUG_FILE}" DESTINATION . COMPONENT debug)

    set(CPACK_PACKAGE_NAME coax)
    set(CPACK_PACKAGE_VENDOR "Jono Cairns")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Windows live-TV player built on libmpv")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_SOURCE_DIR}/cmake/CPackOptions.cmake")
    set(CPACK_GENERATOR ZIP NSIS)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_ALL runtime debug)
    set(CPACK_PACKAGE_FILE_NAME "coax-${PROJECT_VERSION}-win64")
    set(CPACK_ARCHIVE_RUNTIME_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}")
    set(CPACK_ARCHIVE_DEBUG_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}-debug")
    # Unpack into a named folder rather than scattering files into Downloads.
    # Component archives need this variable specifically; the non-component
    # CPACK_INCLUDE_TOPLEVEL_DIRECTORY has no effect here. Both archives share
    # the folder name on purpose, so unpacking the debug archive over the
    # runtime one drops coax.debug beside the executable, where it is looked up.
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)

    # Installer. Everything above already describes what a release contains, so
    # NSIS only adds the parts a double-click install needs: a stable directory
    # that upgrades in place, a Start Menu entry, and an uninstaller that
    # appears in Add/Remove Programs.
    set(CPACK_NSIS_PACKAGE_NAME "Coax")
    set(CPACK_NSIS_DISPLAY_NAME "Coax")
    # Unversioned, so an upgrade replaces the previous install instead of
    # leaving a copy per version behind.
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Coax")
    # The shortcut is written by hand rather than through
    # CPACK_PACKAGE_EXECUTABLES, whose template hardcodes $INSTDIR\bin. This
    # payload is flat, because the same install rules produce the portable
    # archive, where a lone bin/ directory would be noise.
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Coax.lnk' '$INSTDIR\\\\coax.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$MUI_TEMP\\\\Coax.lnk'")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "coax.exe")
    set(CPACK_NSIS_MUI_ICON "${COAX_ICON_PATH}")
    set(CPACK_NSIS_MUI_UNIICON "${COAX_ICON_PATH}")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/jonocairns/coaxn")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MANIFEST_DPI_AWARE ON)

    include(CPack)
endif()
