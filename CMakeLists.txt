cmake_minimum_required(VERSION 3.18)
project(FPlusPlus VERSION 1.0.0 LANGUAGES CXX)

# ─── Global settings ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─── Compiler flags ───────────────────────────────────────────────────────────
if(MSVC)
    set(FPP_CXX_FLAGS /W4 /WX- /O2 /permissive-)
else()
    set(FPP_CXX_FLAGS
        -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
        -ffast-math -march=native
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        list(APPEND FPP_CXX_FLAGS -O3 -DNDEBUG)
    else()
        list(APPEND FPP_CXX_FLAGS -g -O0 -fsanitize=address,undefined)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
    endif()
endif()

# ─── Source files ─────────────────────────────────────────────────────────────
set(FPP_CORE_SOURCES
    src/lexer.cpp
    src/parser.cpp
    src/types.cpp
    src/ir.cpp
    src/codegen.cpp
    src/runtime.cpp
    stdlib/stdlib.cpp
)

set(FPP_INCLUDE_DIRS include stdlib)

# ─── Core static library ──────────────────────────────────────────────────────
add_library(fpp_core STATIC ${FPP_CORE_SOURCES})
target_include_directories(fpp_core PUBLIC ${FPP_INCLUDE_DIRS})
target_compile_options(fpp_core PRIVATE ${FPP_CXX_FLAGS})
set_target_properties(fpp_core PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Link threads & filesystem
find_package(Threads REQUIRED)
target_link_libraries(fpp_core PUBLIC Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(fpp_core PUBLIC stdc++fs)
endif()

# ─── Standalone interpreter binary ───────────────────────────────────────────
add_executable(fpp tools/main.cpp)
target_link_libraries(fpp PRIVATE fpp_core)
target_include_directories(fpp PRIVATE ${FPP_INCLUDE_DIRS})
target_compile_options(fpp PRIVATE ${FPP_CXX_FLAGS})
set_target_properties(fpp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ─── pybind11 Python extension ────────────────────────────────────────────────
option(BUILD_PYTHON_BINDINGS "Build pybind11 Python extension" ON)

if(BUILD_PYTHON_BINDINGS)
    find_package(Python3 COMPONENTS Interpreter Development QUIET)
    if(Python3_FOUND)
        # Try to find pybind11 from pip or system
        execute_process(
            COMMAND ${Python3_EXECUTABLE} -c "import pybind11; print(pybind11.get_cmake_dir())"
            OUTPUT_VARIABLE pybind11_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(pybind11_DIR)
            find_package(pybind11 REQUIRED PATHS ${pybind11_DIR})
        else()
            find_package(pybind11 QUIET)
        endif()

        if(pybind11_FOUND)
            pybind11_add_module(fpp_native bindings/fpp_bindings.cpp ${FPP_CORE_SOURCES})
            target_include_directories(fpp_native PRIVATE ${FPP_INCLUDE_DIRS})
            target_compile_options(fpp_native PRIVATE ${FPP_CXX_FLAGS})
            target_link_libraries(fpp_native PRIVATE Threads::Threads)
            if(UNIX AND NOT APPLE)
                target_link_libraries(fpp_native PRIVATE stdc++fs)
            endif()
            set_target_properties(fpp_native PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
            )
            message(STATUS "✓ Python bindings enabled (pybind11 found)")
        else()
            message(STATUS "✗ pybind11 not found — skipping Python bindings")
            message(STATUS "  Install with: pip install pybind11")
        endif()
    else()
        message(STATUS "✗ Python3 not found — skipping Python bindings")
    endif()
endif()

# ─── Tests ────────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build test suite" ON)

if(BUILD_TESTS)
    enable_testing()
    add_executable(fpp_tests tools/tests.cpp)
    target_link_libraries(fpp_tests PRIVATE fpp_core)
    target_include_directories(fpp_tests PRIVATE ${FPP_INCLUDE_DIRS})
    target_compile_options(fpp_tests PRIVATE ${FPP_CXX_FLAGS})
    set_target_properties(fpp_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME FppTests COMMAND fpp_tests)
endif()

# ─── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "F++ v${PROJECT_VERSION}")
message(STATUS "  Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:     C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:         ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Python bindings:  ${BUILD_PYTHON_BINDINGS}")
message(STATUS "  Tests:            ${BUILD_TESTS}")
message(STATUS "")
