cmake_minimum_required(VERSION 3.16)
project(fir_filter C)

set(CMAKE_C_STANDARD 11)

# Find packages
find_package(PkgConfig REQUIRED)

# Find PipeWire
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)

# Create PipeWire imported target (more modern approach)
add_library(PipeWire::PipeWire INTERFACE IMPORTED)
target_include_directories(PipeWire::PipeWire INTERFACE ${PIPEWIRE_INCLUDE_DIRS})
target_compile_options(PipeWire::PipeWire INTERFACE ${PIPEWIRE_CFLAGS_OTHER})
target_link_directories(PipeWire::PipeWire INTERFACE ${PIPEWIRE_LIBRARY_DIRS})
target_link_libraries(PipeWire::PipeWire INTERFACE ${PIPEWIRE_LIBRARIES})

# Add executable
add_executable(fir_filter
        main.c
        fir.c
        fir.h
        44100.c
        88200.c
        176400.c
        48000.c
        96000.c
        192000.c
        filters.c
)

# Link libraries using modern targets
target_link_libraries(fir_filter
        PRIVATE
        PipeWire::PipeWire
        m
)

# Optional: Enable more warnings and optimizations
target_compile_options(fir_filter PRIVATE
        -Wall
        -Wextra
        $<$<CONFIG:Release>:-O3 -march=native -mtune=native -mavx512f -ffast-math -funroll-loops -ftree-vectorize -flto>
        $<$<CONFIG:Debug>:-g -O0>
)

# Enable link-time optimization for Release builds
set_target_properties(fir_filter PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
)