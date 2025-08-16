file(GLOB SRC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

add_executable(pwcp ${SRC_SOURCES})

target_include_directories(pwcp PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/lib
)

target_link_libraries(pwcp PRIVATE afsk)