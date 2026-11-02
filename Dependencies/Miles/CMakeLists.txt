# Armies Of Tomorrow: 64-bit builds have no mss32.dll, so they implement the Miles API on
# miniaudio instead of loading it. AOT_MILES_MINIAUDIO can also be turned on for 32-bit tests.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(AOT_MILES_MINIAUDIO_DEFAULT ON)
else()
    set(AOT_MILES_MINIAUDIO_DEFAULT OFF)
endif()
option(AOT_MILES_MINIAUDIO "Implement the Miles API on miniaudio instead of loading mss32.dll" ${AOT_MILES_MINIAUDIO_DEFAULT})

set(MILES_SRC
    mss/mss.h
    MilesLoader.h
    AoTVideoAudio.h
)
if(AOT_MILES_MINIAUDIO)
    list(APPEND MILES_SRC MilesMiniaudio.cpp)
else()
    list(APPEND MILES_SRC MilesLoader.cpp)
endif()

add_library(deps_miles STATIC)

target_sources(deps_miles PRIVATE ${MILES_SRC})

target_include_directories(deps_miles PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/mss
)

target_link_libraries(deps_miles PRIVATE
    depsi_always # Gives force included headers. Link early.
    deps_utility
)

if(AOT_MILES_MINIAUDIO)
    target_include_directories(deps_miles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../miniaudio)
    target_compile_definitions(deps_miles PUBLIC AOT_MILES_MINIAUDIO=1)
    if(WIN32)
        target_link_libraries(deps_miles PRIVATE ole32 winmm)
    endif()
endif()
