cmake_minimum_required(VERSION 3.20)

# An independent downstream project. It is not part of the Path Failover Fabric
# build tree: it consumes the installed package through find_package(... CONFIG).
project(pff_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(pff 1.0 CONFIG REQUIRED)

add_executable(pff_consumer main.cpp)
target_link_libraries(pff_consumer PRIVATE pff::pff)

if(MSVC)
  target_compile_options(pff_consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
  target_compile_options(pff_consumer PRIVATE -Wall -Wextra -Wpedantic)
endif()
