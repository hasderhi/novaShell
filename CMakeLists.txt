cmake_minimum_required(VERSION 3.31)
project(novaShell C)

set(CMAKE_C_STANDARD 23)

add_executable(novaShell main.c nvs.rc)