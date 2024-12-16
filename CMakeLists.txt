cmake_minimum_required(VERSION 3.30)
project(Filesystem)

set(CMAKE_CXX_STANDARD 20)

add_executable(Filesystem main.cpp
        cmake-build-debug/test.cpp
        fs.cpp
        fs.cpp)
