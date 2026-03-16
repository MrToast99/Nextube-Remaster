
# Read firmware version from version.json so we can propagate it as a
# compile definition to every component that links against config_mgr.
#
# Why here and not in the root CMakeLists?
#   add_compile_definitions() at project root only affects the current CMake
#   directory scope — it does NOT reach component subdirectories.
#   target_compile_definitions(${COMPONENT_LIB} PUBLIC ...) propagates the
#   definition to every consumer via CMake's target dependency graph, which
#   is exactly what ESP-IDF components use.  Since every component that needs
#   FW_VERSION_STR already REQUIRES config_mgr, this one line covers them all.
#
# FW_VERSION is set by the root CMakeLists.txt before project() is called,
# so it is inherited into this directory scope when ESP-IDF processes
# components during idf_build_process().
if(NOT FW_VERSION)
    # Fallback: re-parse version.json when FW_VERSION wasn't inherited.
    #
    # IMPORTANT: use CMAKE_CURRENT_LIST_DIR (always the directory of THIS file)
    # rather than CMAKE_SOURCE_DIR.  During the ESP-IDF component requirements
    # scan (__component_get_requirements), CMake evaluates component files in a
    # subprocess where CMAKE_SOURCE_DIR resolves to the *build* directory, not
    # the source root.  CMAKE_CURRENT_LIST_DIR is immune to this remapping.
    #
    # components/config_mgr/CMakeLists.txt → ../../ → project root
    set(_VER_JSON_PATH "${CMAKE_CURRENT_LIST_DIR}/../../version.json")
    if(EXISTS "${_VER_JSON_PATH}")
        file(READ "${_VER_JSON_PATH}" _VER_JSON)
        string(REGEX MATCH "\"version\"[^\"]*\"([0-9]+[.][0-9]+[.][0-9]+)\"" _ "${_VER_JSON}")
        set(FW_VERSION "${CMAKE_MATCH_1}")
    endif()
    if(NOT FW_VERSION)
        set(FW_VERSION "0.0.0")
        message(WARNING "config_mgr: could not read version from version.json – using 0.0.0")
    endif()
endif()

idf_component_register(
    SRCS "config_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES json spiffs
)

# PUBLIC: applies to config_mgr itself AND to every component that REQUIRES
# config_mgr (main, web_server, weather, display, …).
# The #ifndef guard in fw_version.h prevents double-definition.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    "FW_VERSION_STR=\"${FW_VERSION}\""
)
