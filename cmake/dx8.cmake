if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # AoT @feature 64-bit: there is no 64-bit D3D8 import library and no 64-bit
    # D3DX8 at all. D3D8.DLL is loaded at runtime by dx8wrapper.cpp, and the D3DX8
    # functions the engine uses are reimplemented in Dependencies/d3dx8compat.
    # Only the SDK headers are used, so its CMakeLists (which links the bundled
    # 32-bit .lib files) is skipped through a SOURCE_SUBDIR that does not exist.
    set(RTS_DX8_SOURCE_SUBDIR "headers-only")
else()
    set(RTS_DX8_SOURCE_SUBDIR "")
endif()

FetchContent_Declare(
    dx8
    GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
    GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
    SOURCE_SUBDIR  ${RTS_DX8_SOURCE_SUBDIR}
)

FetchContent_MakeAvailable(dx8)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_subdirectory(${CMAKE_SOURCE_DIR}/Dependencies/d3dx8compat ${CMAKE_BINARY_DIR}/d3dx8compat)

    add_library(d3d8lib INTERFACE)
    target_include_directories(d3d8lib INTERFACE ${dx8_SOURCE_DIR})
    target_compile_definitions(d3d8lib INTERFACE -DBUILD_WITH_D3D8)
    target_link_libraries(d3d8lib INTERFACE d3dx8compat dinput8 dxguid)
endif()
