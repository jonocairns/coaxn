# Per-generator CPack overrides. CPack loads this once per generator, with
# CPACK_GENERATOR set to the one currently running.

if(CPACK_GENERATOR STREQUAL "NSIS")
    # The installer is for people who just want to run the app, so it carries
    # the runtime only. Debug symbols stay a separate download for whoever is
    # diagnosing a crash. Without this the NSIS generator, which does not use
    # component pages here, would merge every component into one installer.
    set(CPACK_COMPONENTS_ALL runtime)

    # The archives unpack into a named folder so they do not scatter files into
    # Downloads. An installer already owns a directory, and leaving this on
    # would nest the payload one level deeper than $INSTDIR, which is where the
    # Start Menu shortcut points.
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)

    # Distinguish the installer from the portable archive of the same version.
    # This has to happen here: the name is resolved per generator, so setting
    # it once in CMakeLists would rename the archives too.
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}-setup")
endif()
