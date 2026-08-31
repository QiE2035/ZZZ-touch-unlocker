# =============================================================================
# zzz-touch-unlocker — runtime-only native touch + mobile UI restore
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(zzz_touch_unlocker CXX C)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-cast-function-type
                        -fno-strict-aliasing -Wno-pedantic)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
                            UNICODE _UNICODE)
endif()

add_library(minhook STATIC
    third_party/MinHook/src/buffer.c
    third_party/MinHook/src/hook.c
    third_party/MinHook/src/trampoline.c
    third_party/MinHook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC third_party/MinHook/include)
target_compile_definitions(minhook PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
if(NOT MSVC)
    target_compile_options(minhook PRIVATE -Wno-unused-parameter)
endif()

add_library(zzz_touch_hook SHARED
    hook/dllmain.cpp
    hook/touch_hook.cpp
    hook/legacy_input_switch.cpp
    hook/layout_platform_override.cpp
    hook/general_data_persistence_guard.cpp
    hook/logger.cpp
    hook/patterns.cpp
)
set_target_properties(zzz_touch_hook PROPERTIES
    PREFIX ""
    OUTPUT_NAME "zzz-touch-hook"
)
target_include_directories(zzz_touch_hook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hook)
target_link_libraries(zzz_touch_hook PRIVATE minhook kernel32)

add_executable(zzz_touch_unlocker launcher/main.cpp)
set_target_properties(zzz_touch_unlocker PROPERTIES OUTPUT_NAME "zzz-touch-unlocker")
target_link_libraries(zzz_touch_unlocker PRIVATE kernel32)
if(NOT MSVC)
    set_target_properties(zzz_touch_unlocker PROPERTIES LINK_FLAGS "-municode")
endif()
