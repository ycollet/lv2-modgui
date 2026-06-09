cmake_minimum_required(VERSION 3.10)
project(modgui-host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LV2   REQUIRED lv2)
pkg_check_modules(LILV  REQUIRED lilv-0)
pkg_check_modules(GTK3  REQUIRED gtk+-3.0)

pkg_check_modules(WEBKIT webkit2gtk-4.1)
if(NOT WEBKIT_FOUND)
    pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.0)
    add_definitions(-DWEBKIT_API_40)
    message(STATUS "Using webkit2gtk-4.0")
else()
    message(STATUS "Using webkit2gtk-4.1")
endif()

set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/modgui-host.lv2")
set(PLUGIN_LIB "modgui-host${CMAKE_SHARED_MODULE_SUFFIX}")
set(UI_LIB     "modgui-host-ui${CMAKE_SHARED_MODULE_SUFFIX}")

configure_file(manifest.ttl.in     "${BUNDLE_DIR}/manifest.ttl"     @ONLY)
configure_file(modgui-host.ttl.in  "${BUNDLE_DIR}/modgui-host.ttl"  @ONLY)

# ── DSP plugin ────────────────────────────────────────────────────────────────
add_library(modgui-host MODULE src/plugin.c)
set_target_properties(modgui-host PROPERTIES
    PREFIX ""
    OUTPUT_NAME "modgui-host"
    LIBRARY_OUTPUT_DIRECTORY "${BUNDLE_DIR}"
)
target_include_directories(modgui-host PRIVATE
    ${LV2_INCLUDE_DIRS}
    ${LILV_INCLUDE_DIRS}
    src
)
target_link_libraries(modgui-host PRIVATE ${LILV_LIBRARIES})
target_compile_options(modgui-host PRIVATE
    ${LV2_CFLAGS_OTHER} ${LILV_CFLAGS_OTHER} -Wall -Wextra
)

# ── UI plugin ─────────────────────────────────────────────────────────────────
add_library(modgui-host-ui MODULE src/ui.c)
set_target_properties(modgui-host-ui PROPERTIES
    PREFIX ""
    OUTPUT_NAME "modgui-host-ui"
    LIBRARY_OUTPUT_DIRECTORY "${BUNDLE_DIR}"
)
target_include_directories(modgui-host-ui PRIVATE
    ${LV2_INCLUDE_DIRS}
    ${LILV_INCLUDE_DIRS}
    ${GTK3_INCLUDE_DIRS}
    ${WEBKIT_INCLUDE_DIRS}
    src
)
target_link_libraries(modgui-host-ui PRIVATE
    ${LILV_LIBRARIES}
    ${GTK3_LIBRARIES}
    ${WEBKIT_LIBRARIES}
)
target_compile_options(modgui-host-ui PRIVATE
    ${LV2_CFLAGS_OTHER} ${LILV_CFLAGS_OTHER}
    ${GTK3_CFLAGS_OTHER} ${WEBKIT_CFLAGS_OTHER}
    -Wall -Wextra
)

# ── Resources ─────────────────────────────────────────────────────────────────
add_custom_target(copy-resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources"
        "${BUNDLE_DIR}/resources"
    DEPENDS modgui-host modgui-host-ui
)

# ── Install ───────────────────────────────────────────────────────────────────
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.lv2" CACHE PATH "Install prefix" FORCE)
endif()
install(DIRECTORY "${BUNDLE_DIR}/" DESTINATION "modgui-host.lv2")
