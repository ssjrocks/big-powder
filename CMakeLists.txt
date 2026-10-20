cmake_minimum_required(VERSION 3.25)
project(big_powder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED CONFIG)

set(SLANGC "${CMAKE_SOURCE_DIR}/tools/slang/bin/slangc" CACHE FILEPATH "Slang compiler")
if(NOT EXISTS "${SLANGC}")
    message(FATAL_ERROR "slangc not found at ${SLANGC}. Run tools/get-slang.sh first.")
endif()

# Compute kernels; each file has one entry point called main.
set(KERNELS prepare args update freelist spawn render stats_parts stats_cells)
file(GLOB_RECURSE SHADER_DEPS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/shaders/*.slang")
set(SPV_DIR "${CMAKE_BINARY_DIR}/shaders")
set(SPV_OUTPUTS)
foreach(k ${KERNELS})
    set(out "${SPV_DIR}/${k}.spv")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SPV_DIR}"
        COMMAND "${SLANGC}" "${CMAKE_SOURCE_DIR}/shaders/${k}.slang"
                -target spirv -profile spirv_1_5 -entry main -stage compute -O2
                -I "${CMAKE_SOURCE_DIR}/shaders" -o "${out}"
        DEPENDS ${SHADER_DEPS}
        COMMENT "slangc ${k}.slang"
        VERBATIM)
    list(APPEND SPV_OUTPUTS "${out}")
endforeach()
add_custom_target(shaders ALL DEPENDS ${SPV_OUTPUTS})

add_executable(big-powder
    src/main.cpp
    src/gpu/VkContext.cpp
    src/sim/Elements.cpp
    src/sim/GpuSim.cpp
    src/sim/Scenes.cpp
    src/app/Viewer.cpp
    src/app/Bench.cpp)
target_include_directories(big-powder PRIVATE src)
target_link_libraries(big-powder PRIVATE Vulkan::Vulkan SDL3::SDL3)
target_compile_options(big-powder PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_dependencies(big-powder shaders)
