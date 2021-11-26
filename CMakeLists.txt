cmake_minimum_required(VERSION 3.16)

project(shared-memory C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(__USE_GNU _GNU_SOURCE)

add_executable(shared-memory-sender sender.c)
add_executable(shared-memory-receiver receiver.c)
