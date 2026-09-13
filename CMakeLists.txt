cmake_minimum_required(VERSION 3.16)
project(setup_ui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL3 supplies the window and the 2D renderer every host already presents
# through; RmlUi renders the setup document into it.
if(NOT TARGET SDL3::SDL3 AND NOT TARGET SDL3::SDL3-static)
    find_package(SDL3 CONFIG REQUIRED)
endif()

# RmlUi resolves the way the ports resolve their shared checkouts: an explicit
# SETUP_UI_RMLUI_DIR, or a sibling checkout next to this repository.
set(SETUP_UI_RMLUI_DEFAULT "")
foreach(candidate "${CMAKE_CURRENT_SOURCE_DIR}/../RmlUi" "${CMAKE_CURRENT_SOURCE_DIR}/../../RmlUi")
    if(IS_DIRECTORY "${candidate}")
        set(SETUP_UI_RMLUI_DEFAULT "${candidate}")
        break()
    endif()
endforeach()
if(DEFINED ENV{SETUP_UI_RMLUI_DIR} AND NOT "$ENV{SETUP_UI_RMLUI_DIR}" STREQUAL "")
    set(SETUP_UI_RMLUI_DEFAULT "$ENV{SETUP_UI_RMLUI_DIR}")
endif()
set(SETUP_UI_RMLUI_DIR "${SETUP_UI_RMLUI_DEFAULT}" CACHE PATH "Path to the RmlUi checkout")
get_filename_component(SETUP_UI_RMLUI_DIR "${SETUP_UI_RMLUI_DIR}" ABSOLUTE)
if(NOT IS_DIRECTORY "${SETUP_UI_RMLUI_DIR}")
    message(FATAL_ERROR
        "setup_ui requires an RmlUi checkout at ${SETUP_UI_RMLUI_DIR}; "
        "provide -DSETUP_UI_RMLUI_DIR=<checkout>")
endif()
# RmlUi renders text through Freetype::Freetype, so every product needs that
# target from somewhere. Three sources, in order of what the host provides:
# an already-declared target (Android consumes the shared Android prefix's
# Freetype, and a consumer may name its own), a Freetype CMake package on the
# host, or a Freetype checkout built here as a static library. Building it
# rather than linking the host's copy is what keeps a packaged product free of
# a font-engine runtime dependency it would otherwise have to bundle.
if(EMSCRIPTEN AND NOT TARGET Freetype::Freetype)
    # The browser toolchain ships FreeType as a port, not as a CMake package.
    add_library(setup_ui_freetype_port INTERFACE)
    target_include_directories(setup_ui_freetype_port SYSTEM INTERFACE
        "${EMSCRIPTEN_SYSROOT}/include/freetype2")
    target_compile_options(setup_ui_freetype_port INTERFACE "-sUSE_FREETYPE=1")
    target_link_options(setup_ui_freetype_port INTERFACE "-sUSE_FREETYPE=1")
    add_library(Freetype::Freetype ALIAS setup_ui_freetype_port)
endif()

if(NOT TARGET Freetype::Freetype)
    find_package(Freetype QUIET)
endif()

if(NOT TARGET Freetype::Freetype)
    if(IS_DIRECTORY "${SETUP_UI_FREETYPE_DIR}")
        set(SETUP_UI_FREETYPE_RESOLVED "${SETUP_UI_FREETYPE_DIR}")
    else()
        set(SETUP_UI_FREETYPE_RESOLVED "")
        foreach(candidate
                "$ENV{SETUP_UI_FREETYPE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../freetype"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../freetype")
            if(NOT "${candidate}" STREQUAL "" AND IS_DIRECTORY "${candidate}")
                set(SETUP_UI_FREETYPE_RESOLVED "${candidate}")
                break()
            endif()
        endforeach()
    endif()
    if(NOT IS_DIRECTORY "${SETUP_UI_FREETYPE_RESOLVED}")
        message(FATAL_ERROR
            "setup_ui needs a Freetype font engine: install Freetype development "
            "files (Freetype::Freetype), or provide a Freetype checkout with "
            "-DSETUP_UI_FREETYPE_DIR=<checkout>. A checkout is built as a static "
            "library, which is what a packaged product should link.")
    endif()
    set(SETUP_UI_FREETYPE_DIR "${SETUP_UI_FREETYPE_RESOLVED}" CACHE PATH
        "Freetype checkout to build statically" FORCE)
    # Freetype keeps only the font formats it was configured for; the setup
    # screen draws a TrueType face and nothing else, so the optional codecs are
    # left out instead of pulling zlib, PNG, bzip2, brotli, and HarfBuzz in.
    set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
    set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
    set(SETUP_UI_FREETYPE_SHARED "${BUILD_SHARED_LIBS}")
    # Freetype reads BUILD_SHARED_LIBS to choose its library type; restore the
    # consumer's setting afterwards so only Freetype is affected.
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${SETUP_UI_FREETYPE_DIR}" "${CMAKE_BINARY_DIR}/freetype" EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS "${SETUP_UI_FREETYPE_SHARED}")
    # Freetype exports Freetype::Freetype when it is installed; in a
    # build-tree consumer the alias has to be made here.
    add_library(Freetype::Freetype ALIAS freetype-interface)
endif()

set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
# RmlUi defaults to shared libraries and always builds its debugger; a port
# links it statically and has no debugger UI, so both are turned off here
# (these settings apply to this subdirectory tree only).
set(BUILD_SHARED_LIBS OFF)
set(RMLUI_DEBUGGER OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SETUP_UI_RMLUI_DIR}" "${CMAKE_BINARY_DIR}/rmlui" EXCLUDE_FROM_ALL)

set(SETUP_UI_TITLE "Setup" CACHE STRING "Default setup screen title")
set(SETUP_UI_MESSAGE "Choose your files to continue." CACHE STRING "Default setup message")
set(SETUP_UI_HINT "" CACHE STRING "Default hint under the choose button")
set(SETUP_UI_FOOTER "" CACHE STRING "Default footer text")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SETUP_UI_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SETUP_UI_GENERATED_DIR}")
add_custom_command(
    OUTPUT "${SETUP_UI_GENERATED_DIR}/assets.cpp" "${SETUP_UI_GENERATED_DIR}/assets.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py"
            --assets "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            --source "${SETUP_UI_GENERATED_DIR}/assets.cpp"
            --header "${SETUP_UI_GENERATED_DIR}/assets.h"
            --title "${SETUP_UI_TITLE}" --message "${SETUP_UI_MESSAGE}"
            --hint "${SETUP_UI_HINT}" --footer "${SETUP_UI_FOOTER}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/setup.rml"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/setup.rcss"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py"
    VERBATIM)

add_library(setup_ui STATIC
    src/session.cpp
    src/view.cpp
    src/rml_sdl_renderer.cpp
    "${SETUP_UI_GENERATED_DIR}/assets.cpp")
add_library(setup_ui::setup_ui ALIAS setup_ui)
target_include_directories(setup_ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${SETUP_UI_GENERATED_DIR}>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(setup_ui PUBLIC RmlUi::RmlUi)
if(EMSCRIPTEN)
    # Every executable that links this module also links FreeType, which the
    # browser toolchain provides as a port rather than a library file.
    target_link_options(setup_ui INTERFACE "-sUSE_FREETYPE=1")
endif()
if(TARGET SDL3::SDL3)
    target_link_libraries(setup_ui PUBLIC SDL3::SDL3)
else()
    target_link_libraries(setup_ui PUBLIC SDL3::SDL3-static)
endif()
target_compile_options(setup_ui PRIVATE -Wall -Wextra -Wpedantic)

# The module's own tests run where they can be executed: a cross-compiled
# consumer build (Android, the browser) links the library, not the test binary.
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(setup_ui_tests tests/test_setup_ui.cpp)
    target_link_libraries(setup_ui_tests PRIVATE setup_ui)
    target_include_directories(setup_ui_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${SETUP_UI_GENERATED_DIR}")
    add_test(NAME setup_ui_tests COMMAND setup_ui_tests)
endif()
