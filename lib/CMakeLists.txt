file(GLOB AFSK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/afsk/*.c")

add_library(afsk STATIC ${AFSK_SOURCES}
        ../include/protocol.h)

target_include_directories(afsk PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/afsk
        ${CMAKE_SOURCE_DIR}/include
)
