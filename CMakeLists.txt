cmake_minimum_required(VERSION 3.16)
project(perftest_vk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(perftest-vk
    src/main.cpp
    src/app.hpp
    src/app.cpp
    src/gpu.hpp
    src/gpu.cpp
    src/original_suite.cpp
    src/original_suite.hpp
    src/bindless_suite.cpp
    src/bindless_suite.hpp
    src/bindless_layout.cpp
    src/bindless_layout.hpp)

target_include_directories(perftest-vk PRIVATE src)
target_link_libraries(perftest-vk PRIVATE Vulkan::Vulkan)

if(MSVC)
    target_compile_options(perftest-vk PRIVATE /W4)
    target_compile_definitions(perftest-vk PRIVATE _CRT_SECURE_NO_WARNINGS)   # fopen/snprintf are used portably on purpose
else()
    target_compile_options(perftest-vk PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

# Shader compilers: each one found produces its own SPIR-V set in <build>/shaders/<compiler>.
# Pick one at runtime with --shaders <build>/shaders/<compiler> (default order: slang, dxc, glslang).
option(PERFTEST_SHADERS_SLANG   "Compile shaders with Slang (slangc)"          ON)
option(PERFTEST_SHADERS_DXC     "Compile shaders with DXC"                     ON)
option(PERFTEST_SHADERS_GLSLANG "Compile shaders with glslang (HLSL frontend)" ON)

set(_hint_dirs "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")

find_program(SLANGC_EXECUTABLE slangc HINTS ${_hint_dirs})
find_program(DXC_EXECUTABLE dxc HINTS ${_hint_dirs})
find_program(GLSLANG_EXECUTABLE glslangValidator HINTS ${_hint_dirs})

file(GLOB_RECURSE PERFTEST_SHADER_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/shaders/*.hlsl ${CMAKE_SOURCE_DIR}/shaders/*.hlsli ${CMAKE_SOURCE_DIR}/shaders/*.h)

set(_shader_targets "")

foreach(_compiler slang dxc glslang)
    string(TOUPPER ${_compiler} _upper)
    if(_compiler STREQUAL "slang")
        set(_exe ${SLANGC_EXECUTABLE})
    elseif(_compiler STREQUAL "dxc")
        set(_exe ${DXC_EXECUTABLE})
    else()
        set(_exe ${GLSLANG_EXECUTABLE})
    endif()

    if(PERFTEST_SHADERS_${_upper} AND _exe)
        set(_stamp ${CMAKE_BINARY_DIR}/shaders/${_compiler}/compiler.txt)
        add_custom_command(
            OUTPUT ${_stamp}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/compile_shaders.py
                    --compiler ${_compiler} --exe ${_exe} --out ${CMAKE_BINARY_DIR}/shaders
            DEPENDS ${PERFTEST_SHADER_SOURCES} ${CMAKE_SOURCE_DIR}/scripts/compile_shaders.py
            COMMENT "Compiling perftest shaders with ${_compiler}"
            VERBATIM)
        add_custom_target(perftest_shaders_${_compiler} DEPENDS ${_stamp})
        list(APPEND _shader_targets perftest_shaders_${_compiler})
        message(STATUS "perftest-vk: shaders will be compiled with ${_compiler} (${_exe})")
    endif()

endforeach()

if(_shader_targets)
    add_dependencies(perftest-vk ${_shader_targets})
else()
    message(WARNING "perftest-vk: no shader compiler found (slangc, dxc or glslangValidator). "
                    "Compile shaders manually with scripts/compile_shaders.py.")
endif()
