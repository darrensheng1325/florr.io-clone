cmake_minimum_required(VERSION 3.10)
project(florr_clone_cpp)
include_directories(assets)

# Allow the user to specify the compiler, default to clang++
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER g++)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find SDL2 package
find_package(SDL2 REQUIRED)

# Specify source files
set(SOURCE_FILES
    assets/flower.cpp
    assets/renderer.cpp
    # ...other source files...
)

# Add executable
add_executable(florr_clone_cpp main.cpp ${SOURCE_FILES})

# Include SDL2 headers
target_include_directories(florr_clone_cpp PRIVATE ${SDL2_INCLUDE_DIRS} assets)

# Link SDL2
target_link_libraries(florr_clone_cpp PRIVATE ${SDL2_LIBRARIES})