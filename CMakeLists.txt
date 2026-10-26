cmake_minimum_required(VERSION 3.21)
project(LatencyZero)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Set the GEODE_SDK environment variable to point at a Geode checkout")
endif()
message(STATUS "Found Geode: $ENV{GEODE_SDK}")

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.hpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE src)

if (WIN32)
    # winmm   - timeBeginPeriod / timeEndPeriod
    # opengl32 - glFinish / glFlush prototypes and entry points
    # ntdll and kernel32 extras (NtSetTimerResolution, SetProcessInformation,
    # SetThreadInformation, wglSwapIntervalEXT) are resolved at run time through
    # GetProcAddress, so no import library is needed for them and an older SDK
    # still builds.
    target_link_libraries(${PROJECT_NAME} PRIVATE winmm opengl32)
endif()

setup_geode_mod(${PROJECT_NAME})
