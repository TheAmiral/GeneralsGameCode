# AoT @feature 64-bit: D3DX8 replacement used only by 64-bit builds (see cmake/dx8.cmake).
add_library(d3dx8compat STATIC
    d3dx8compat.cpp
)
target_include_directories(d3dx8compat PRIVATE ${dx8_SOURCE_DIR})
target_compile_definitions(d3dx8compat PRIVATE WIN32_LEAN_AND_MEAN)
