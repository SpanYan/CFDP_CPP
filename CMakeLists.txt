cmake_minimum_required(VERSION 3.16)
project(cfdp_class2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings utiles pour embedded
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)

# ─── Bibliothèque statique ────────────────────────────────────────────────────
add_library(cfdp STATIC
    receiver/Crc.cpp
    receiver/PacketList.cpp
    receiver/UartTransport.cpp
    receiver/CfdpReceiver.cpp
    receiver/Journal.cpp
)

target_include_directories(cfdp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/receiver
)

find_package(ZLIB REQUIRED)
target_link_libraries(cfdp PUBLIC ZLIB::ZLIB)

# Pour cross-compilation aarch64 Yocto :
# cmake -DCMAKE_TOOLCHAIN_FILE=<sdk>/environment-setup-aarch64-poky-linux \
#       -DCMAKE_BUILD_TYPE=Release ..

add_executable(receiver_proc receiver/receiver_main.cpp)
target_link_libraries(receiver_proc PRIVATE cfdp)
