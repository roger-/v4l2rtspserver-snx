# Toolchain file for Sonix SNX98600 SDK ARM cross-compilation
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=snx/snx98600-toolchain.cmake ..
# Requires: SNX_SDK_ROOT environment variable or CMake variable

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Allow SNX_SDK_ROOT to be set via environment or CMake variable
if(NOT DEFINED SNX_SDK_ROOT AND DEFINED ENV{SNX_SDK_ROOT})
    set(SNX_SDK_ROOT $ENV{SNX_SDK_ROOT})
endif()

if(NOT DEFINED SNX_SDK_ROOT)
    message(FATAL_ERROR "SNX_SDK_ROOT not set. Please set via environment variable or -DSNX_SDK_ROOT=/path/to/sdk")
endif()

if(NOT EXISTS "${SNX_SDK_ROOT}")
    message(FATAL_ERROR "SNX_SDK_ROOT path does not exist: ${SNX_SDK_ROOT}")
endif()

# Locate the toolchain binaries
set(TOOLCHAIN_BIN "${SNX_SDK_ROOT}/toolchain/crosstool-4.5.2/bin")

if(NOT EXISTS "${TOOLCHAIN_BIN}")
    message(FATAL_ERROR "Toolchain not found at: ${TOOLCHAIN_BIN}")
endif()

# Prefer arm-unknown-linux-uclibcgnueabi-* if present, otherwise arm-linux-*
if(EXISTS "${TOOLCHAIN_BIN}/arm-unknown-linux-uclibcgnueabi-gcc")
    set(CMAKE_C_COMPILER "${TOOLCHAIN_BIN}/arm-unknown-linux-uclibcgnueabi-gcc")
    set(CMAKE_CXX_COMPILER "${TOOLCHAIN_BIN}/arm-unknown-linux-uclibcgnueabi-g++")
else()
    set(CMAKE_C_COMPILER "${TOOLCHAIN_BIN}/arm-linux-gcc")
    set(CMAKE_CXX_COMPILER "${TOOLCHAIN_BIN}/arm-linux-g++")
endif()

# Configure search paths for libraries and headers
set(CMAKE_FIND_ROOT_PATH "${SNX_SDK_ROOT}" "${TOOLCHAIN_BIN}/..")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Optimization flags for embedded ARM
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Os")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Os")

message(STATUS "SNX98600 Toolchain configured:")
message(STATUS "  SDK Root: ${SNX_SDK_ROOT}")
message(STATUS "  Compiler: ${CMAKE_C_COMPILER}")
