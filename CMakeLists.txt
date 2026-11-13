cmake_minimum_required(VERSION 3.20)

project(keepMoving VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KM_SOURCES
    src/main.c
    src/log.c
    src/util.c
    src/state.c
    src/input.c
    src/bsod.c
    src/worker.c
    src/driver_launch.c
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Vendored GDRVLoader (third_party/gdrvloader, MIT): exploit/hde sources
    # compiled into the exe; gdrv_bridge.cpp exposes an extern "C" boundary.
    list(APPEND KM_SOURCES
        third_party/gdrvloader/exploit/pe.cpp
        third_party/gdrvloader/exploit/swind2.cpp
        third_party/gdrvloader/hde/hde64.c
        src/gdrv_bridge.cpp)
    set(KM_LOADER_INCLUDES
        third_party/gdrvloader
        third_party/gdrvloader/exploit
        third_party/gdrvloader/binary
        third_party/gdrvloader/hde
        third_party/gdrvloader/utils)
else()
    set(KM_LOADER_INCLUDES "")
endif()

add_executable(keepMoving ${KM_SOURCES})

target_include_directories(keepMoving PRIVATE src ${KM_LOADER_INCLUDES})

target_compile_definitions(keepMoving PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(keepMoving PRIVATE /W4 /utf-8)
    # Static CRT: the exe runs on a bare Windows box with no redistributable.
    set_property(TARGET keepMoving PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# user32 for GetLastInputInfo / window station, advapi32 for the token
# privileges, kernel32 (default) for everything else.
target_link_libraries(keepMoving PRIVATE user32 advapi32)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # SHDeleteKeyW used by the vendored loader service cleanup.
    target_link_libraries(keepMoving PRIVATE shlwapi)
endif()
