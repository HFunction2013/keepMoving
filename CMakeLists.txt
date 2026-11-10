cmake_minimum_required(VERSION 3.20)

project(keepMoving VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(KM_SOURCES
    src/main.c
    src/log.c
    src/util.c
    src/state.c
    src/input.c
    src/bsod.c
    src/worker.c
)

add_executable(keepMoving ${KM_SOURCES})

target_include_directories(keepMoving PRIVATE src)

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
