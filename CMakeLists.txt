cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBLC3_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_lc3.c
        ${CMAKE_CURRENT_SOURCE_DIR}/lc3_stubs.c
)

SET(LIBLC3_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/liblc3.a
)

SET(LIBLC3_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(liblc3
        "${LIBLC3_SRC}"
        "${LIBLC3_LIBS}"
        []
        ""
        "${LIBLC3_INC}"
        ""
        "1")
