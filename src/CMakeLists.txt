cmake_minimum_required(VERSION 3.16)
project(PBCP C)

include(FetchContent)

FetchContent_Declare(
        portaudio
        GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
        GIT_TAG        master
)
FetchContent_MakeAvailable(portaudio)

# -------------------
# Transmitter executable
# -------------------
add_executable(tx tx.c)
target_include_directories(tx PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/lib
        ${portaudio_SOURCE_DIR}/include
)
target_link_libraries(tx PRIVATE afsk portaudio)

# -------------------
# Receiver executable
# -------------------
add_executable(rx rx.c)
target_include_directories(rx PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/lib
        ${portaudio_SOURCE_DIR}/include
)
target_link_libraries(rx PRIVATE afsk portaudio)
