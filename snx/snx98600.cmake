# snx98600.cmake
# SNX98600 SDK integration support for v4l2rtspserver
# Conditionally includes SNX SDK libraries and sources when building for SNX platform

message(STATUS "Loading SNX98600 SDK support...")

# Helper function to configure ALSA library (static or shared)
function(configure_alsa_library TARGET_NAME)
    # Check if user disabled static build
    if(DEFINED STATIC_BUILD AND NOT STATIC_BUILD)
        message(STATUS "Using shared ALSA library (STATIC_BUILD=OFF)")
        target_link_libraries(${TARGET_NAME} PRIVATE asound)
        return()
    endif()
    
    # Look for static ALSA library in common locations
    set(ALSA_SEARCH_PATHS
        "${CMAKE_SOURCE_DIR}/../libs"
        "${CMAKE_SOURCE_DIR}/libs"
        "${SNX_SDK_ROOT}/middleware/_install/lib"
    )
    
    find_library(ALSA_STATIC_LIB
        NAMES libasound.a
        PATHS ${ALSA_SEARCH_PATHS}
        NO_DEFAULT_PATH
    )
    
    if(ALSA_STATIC_LIB)
        message(STATUS "Using static ALSA library: ${ALSA_STATIC_LIB}")
        target_link_libraries(${TARGET_NAME} PRIVATE "${ALSA_STATIC_LIB}" dl rt)
    else()
        message(STATUS "Static ALSA not found, using shared library (libasound.so)")
        target_link_libraries(${TARGET_NAME} PRIVATE asound)
    endif()
endfunction()

# Helper function to configure SNX libraries (static or shared)
function(configure_snx_library TARGET_NAME LIB_NAME)
    set(SEARCH_PATH "${SNX_SDK_ROOT}/middleware/_install/lib")
    
    # Try static library first if STATIC_BUILD is enabled
    if(STATIC_BUILD)
        find_library(SNX_STATIC_LIB_${LIB_NAME}
            NAMES lib${LIB_NAME}.a
            PATHS ${SEARCH_PATH}
            NO_DEFAULT_PATH
        )
        
        if(SNX_STATIC_LIB_${LIB_NAME})
            message(STATUS "Using static SNX library: ${LIB_NAME} -> ${SNX_STATIC_LIB_${LIB_NAME}}")
            target_link_libraries(${TARGET_NAME} PRIVATE ${SNX_STATIC_LIB_${LIB_NAME}})
            return()
        else()
            message(STATUS "Static lib${LIB_NAME}.a not found, trying shared...")
        endif()
    endif()
    
    # Fall back to shared library
    find_library(SNX_LIB_${LIB_NAME}
        NAMES ${LIB_NAME}
        PATHS ${SEARCH_PATH}
        NO_DEFAULT_PATH
    )
    
    if(SNX_LIB_${LIB_NAME})
        message(STATUS "Using shared SNX library: ${LIB_NAME} -> ${SNX_LIB_${LIB_NAME}}")
        target_link_libraries(${TARGET_NAME} PRIVATE ${SNX_LIB_${LIB_NAME}})
    else()
        message(WARNING "SNX library not found: ${LIB_NAME}")
    endif()
endfunction()

# Validate SNX SDK availability
if(NOT DEFINED SNX_SDK_ROOT)
    message(WARNING "SNX_SDK_ROOT not defined. SNX support will be limited.")
    return()
endif()

if(NOT EXISTS "${SNX_SDK_ROOT}")
    message(WARNING "SNX_SDK_ROOT does not exist: ${SNX_SDK_ROOT}")
    return()
endif()

message(STATUS "SNX_SDK_ROOT: ${SNX_SDK_ROOT}")

# Discover SNX-specific source files
file(GLOB SNX_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/snx/*.cpp"
)

list(LENGTH SNX_SOURCES SNX_SOURCE_COUNT)
if(SNX_SOURCE_COUNT EQUAL 0)
    message(WARNING "No SNX source files found in src/snx/")
else()
    message(STATUS "Found ${SNX_SOURCE_COUNT} SNX source file(s)")
endif()

# Add SNX sources to the main library
target_sources(libv4l2rtspserver PRIVATE ${SNX_SOURCES})

# Add SNX SDK include paths
target_include_directories(libv4l2rtspserver PUBLIC
    ${SNX_SDK_ROOT}/middleware/_install/include
    ${SNX_SDK_ROOT}/buildscript/include
)

# Locate and link SNX SDK libraries
set(SNX_LIBS snx_vc snx_isp snx_common snx_rc)
foreach(LIB ${SNX_LIBS})
    configure_snx_library(libv4l2rtspserver ${LIB})
endforeach()

# Configure ALSA for audio support
configure_alsa_library(libv4l2rtspserver)

# Add SNX-specific compiler definitions
target_compile_definitions(libv4l2rtspserver PUBLIC 
    HAVE_SNX_SDK=1
    HAVE_ALSA=1
    LOCALE_NOT_USED=1  # Required for old GCC 4.5.2 in SNX SDK toolchain
)

# Strip the final executable to minimize size for embedded target
if(CMAKE_CROSSCOMPILING AND CMAKE_STRIP)
    add_custom_command(TARGET v4l2rtspserver POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:v4l2rtspserver>
        COMMENT "Stripping v4l2rtspserver binary for embedded deployment")
endif()

message(STATUS "SNX98600 SDK support configured successfully")
