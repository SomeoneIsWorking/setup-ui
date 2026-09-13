cmake_minimum_required(VERSION 3.16)
project(setup_ui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Lucent supplies the bounded HTTP transport. Resolve the checkout the way the
# ports do: SETUP_UI_LUCENT_DIR, or a sibling ../lucent directory.
# Candidate roots: a sibling inside shared/, then the repo-root checkout.
set(SETUP_UI_LUCENT_DEFAULT "")
foreach(candidate "${CMAKE_CURRENT_SOURCE_DIR}/../lucent"
                  "${CMAKE_CURRENT_SOURCE_DIR}/../../lucent")
    if(IS_DIRECTORY "${candidate}")
        set(SETUP_UI_LUCENT_DEFAULT "${candidate}")
        break()
    endif()
endforeach()
if(DEFINED ENV{SETUP_UI_LUCENT_DIR} AND NOT "$ENV{SETUP_UI_LUCENT_DIR}" STREQUAL "")
    set(SETUP_UI_LUCENT_DEFAULT "$ENV{SETUP_UI_LUCENT_DIR}")
endif()
set(SETUP_UI_LUCENT_DIR "${SETUP_UI_LUCENT_DEFAULT}" CACHE PATH "Path to the lucent library")
get_filename_component(SETUP_UI_LUCENT_DIR "${SETUP_UI_LUCENT_DIR}" ABSOLUTE)
if(NOT IS_DIRECTORY "${SETUP_UI_LUCENT_DIR}")
    message(FATAL_ERROR
        "setup_ui requires lucent at ${SETUP_UI_LUCENT_DIR}; "
        "provide -DSETUP_UI_LUCENT_DIR=<checkout>")
endif()

set(SETUP_UI_SETUP_UI OFF CACHE BOOL "" FORCE)
set(LUCENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SETUP_UI_LUCENT_DIR}" "${CMAKE_BINARY_DIR}/lucent")

# Embed the web assets. Defaults come from Config when the consumer passes
# wording; the generator bakes the consumer's defaults so the page renders
# complete content even before /api/config returns.
set(SETUP_UI_TITLE "Setup" CACHE STRING "Default setup page title")
set(SETUP_UI_MESSAGE "Choose your files to continue." CACHE STRING "Default setup message")
set(SETUP_UI_HINT "" CACHE STRING "Default hint under the drop zone")
set(SETUP_UI_FOOTER "" CACHE STRING "Default footer text")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SETUP_UI_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SETUP_UI_GENERATED_DIR}")
add_custom_command(
    OUTPUT "${SETUP_UI_GENERATED_DIR}/assets.cpp" "${SETUP_UI_GENERATED_DIR}/assets.h"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py"
            --assets "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            --source "${SETUP_UI_GENERATED_DIR}/assets.cpp"
            --header "${SETUP_UI_GENERATED_DIR}/assets.h"
            --title "${SETUP_UI_TITLE}"
            --message "${SETUP_UI_MESSAGE}"
            --hint "${SETUP_UI_HINT}"
            --footer "${SETUP_UI_FOOTER}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/setup.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py"
    VERBATIM)

add_library(setup_ui STATIC
    src/setup_ui.cpp
    src/setup_ui_c.cpp
    "${SETUP_UI_GENERATED_DIR}/assets.cpp")
add_library(setup_ui::setup_ui ALIAS setup_ui)
target_include_directories(setup_ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${SETUP_UI_GENERATED_DIR}>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(setup_ui PUBLIC lucent::lucent)
target_compile_options(setup_ui PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
target_link_libraries(setup_ui PUBLIC Threads::Threads)

enable_testing()
add_executable(setup_ui_tests tests/test_setup_ui.cpp)
target_link_libraries(setup_ui_tests PRIVATE setup_ui)
target_include_directories(setup_ui_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${SETUP_UI_GENERATED_DIR}")
add_test(NAME setup_ui_tests COMMAND setup_ui_tests)
