cmake_minimum_required(VERSION 3.24)
project(telemetry LANGUAGES CXX)

# ---- Language and tooling ---------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)            # -std=c++23, not -std=gnu++23
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)    # compile_commands.json for clangd/IDEs

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# ---- Shared build settings --------------------------------------------------
# An INTERFACE target carries flags, not code. Only targets that link it get
# the warnings, so Catch2 itself is never built with -Werror.
add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)

#Sanitizers go on EVERY target, including Catch2. ASan needs all code that
# touches a std::vector/std::string to be instrumented, or it reports false
# container-overflow errors. Must come before FetchContent_MakeAvailable.
if(SANITIZE)
    add_compile_options(-fsanitize=address,undefined
                        -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---- Core library: core/inc (headers), core/src (sources) -------------------
# Header-only until the first .cpp appears in core/src; then it becomes a
# static library automatically on the next configure.
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.cpp)

if(CORE_SOURCES)
    add_library(core STATIC ${CORE_SOURCES})
    target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/inc)
    target_link_libraries(core PRIVATE project_options)
else()
    add_library(core INTERFACE)
    target_include_directories(core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/inc)
endif()

# ---- Tests ------------------------------------------------------------------
# Built from source with this project's compiler and flags (not Homebrew's
# prebuilt library), so sanitizer builds instrument Catch2 too.
include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)                           # provides catch_discover_tests()

enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(tests ${TEST_SOURCES})
target_link_libraries(tests PRIVATE core project_options Catch2::Catch2WithMain)

catch_discover_tests(tests)              # registers each TEST_CASE with ctest
