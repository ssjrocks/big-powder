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
set(KERNELS air prepare love gol stickmen stacking gravity walls_pre emap_fill deferred rays emp args update freelist spawn insert hdisp gfx streams fire resolve resolve_heavy fire_smooth signs lens mip render overlay blit stats_parts stats_cells)
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

# ---- The Powder Toy (third_party/tpt), for Classic mode ----
# TPT is built by its own Meson build, so its code is compiled exactly as upstream compiles it. The `render`
# target builds the simulation and common libraries (thin archives) plus the save loader and renderer.
find_program(MESON meson REQUIRED)
find_program(NINJA ninja REQUIRED)
set(TPT_SRC "${CMAKE_SOURCE_DIR}/third_party/tpt")
set(TPT_BUILD "${CMAKE_BINARY_DIR}/tpt")
set(TPT_MESON_OPTIONS --buildtype=release -Dbuild_powder=false -Dbuild_render=true -Dbuild_font=false
    -Dlua=none -Dhttp=false -Denforce_https=true -Dignore_updates=true -Db_lto=false)
set(TPT_OBJECTS
    "${TPT_BUILD}/render.p/src_client_GameSave.cpp.o"
    "${TPT_BUILD}/render.p/src_graphics_Renderer.cpp.o"
    "${TPT_BUILD}/render.p/src_common_clipboard_Null.cpp.o")
set(TPT_LIBS "${TPT_BUILD}/libsimulation.a" "${TPT_BUILD}/libcommon.a")
add_custom_command(
    OUTPUT "${TPT_BUILD}/build.ninja"
    COMMAND ${MESON} setup "${TPT_BUILD}" "${TPT_SRC}" ${TPT_MESON_OPTIONS}
    DEPENDS "${TPT_SRC}/meson.build"
    COMMENT "Configuring The Powder Toy (meson)"
    VERBATIM)
add_custom_target(tpt_build
    COMMAND ${NINJA} -C "${TPT_BUILD}" render
    DEPENDS "${TPT_BUILD}/build.ninja"
    BYPRODUCTS ${TPT_OBJECTS} ${TPT_LIBS}
    COMMENT "Building The Powder Toy's simulation (ninja)"
    USES_TERMINAL
    VERBATIM)

find_package(PkgConfig REQUIRED)
pkg_check_modules(TPT_DEPS REQUIRED IMPORTED_TARGET fftw3f libpng16 jsoncpp bzip2)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

# Our glue to TPT, plus TPT sources the render target leaves out: Editing.cpp (brushes) and the FFT
# gravity (render links a no-op one). Built with TPT's include paths and the same flags as TPT's own build.
add_library(tpt_glue OBJECT
    src/classic/ClassicSim.cpp
    src/classic/TptPrefsShim.cpp
    "${TPT_SRC}/src/simulation/Editing.cpp"
    "${TPT_SRC}/src/simulation/gravity/Fft.cpp")
target_include_directories(tpt_glue
    PUBLIC src
    PRIVATE "${TPT_BUILD}" "${TPT_SRC}" "${TPT_BUILD}/src" "${TPT_SRC}/src" "${TPT_BUILD}/resources"
            "${TPT_SRC}/resources")
target_compile_options(tpt_glue PRIVATE
    -DNDEBUG -D_FILE_OFFSET_BITS=64 -fno-rtti -O3 -ftree-vectorize -funsafe-math-optimizations -ffast-math
    -fomit-frame-pointer -msse2 -pthread -Wno-invalid-offsetof -Wno-unused-result -Wno-missing-field-initializers
    -Wno-unused-parameter)
target_link_libraries(tpt_glue PRIVATE PkgConfig::TPT_DEPS)
add_dependencies(tpt_glue tpt_build)

add_executable(big-powder
    src/main.cpp
    src/gpu/VkContext.cpp
    src/gpu/ImageBlit.cpp
    src/sim/Elements.cpp
    src/sim/GpuSim.cpp
    src/sim/Scenes.cpp
    src/sim/Signs.cpp
    src/sim/WorldFile.cpp
    src/ui/Canvas.cpp
    src/ui/GameUi.cpp
    src/gpu/Overlay.cpp
    src/app/Viewer.cpp
    src/app/GpuWorld.cpp
    src/app/ClassicWorld.cpp
    src/app/Bench.cpp
    src/app/Compare.cpp)
target_include_directories(big-powder PRIVATE src)
target_link_libraries(big-powder PRIVATE Vulkan::Vulkan SDL3::SDL3
    tpt_glue ${TPT_OBJECTS} -Wl,--start-group ${TPT_LIBS} -Wl,--end-group PkgConfig::TPT_DEPS PkgConfig::ZSTD pthread)
target_compile_options(big-powder PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_dependencies(big-powder shaders)
