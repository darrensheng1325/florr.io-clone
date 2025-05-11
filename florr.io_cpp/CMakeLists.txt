cmake_minimum_required(VERSION 3.10)
project(florr_clone_cpp)

# Add cmake modules path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Allow the user to specify the compiler, default to clang++
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER g++)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set SDL2 paths for macOS (Homebrew)
if(APPLE)
    set(SDL2_PATH "/opt/homebrew/opt/sdl2")
    set(SDL2_LIBRARIES "-framework Cocoa -L${SDL2_PATH}/lib -lSDL2")
    set(SDL2_INCLUDE_DIRS "${SDL2_PATH}/include/SDL2")
endif()

# Find SDL2 package
find_package(SDL2 REQUIRED)

# Find ENet package
find_package(ENet REQUIRED)

# Common source files
set(COMMON_SOURCE_FILES
    assets/flower.cpp
    assets/renderer.cpp
    assets/ladybug.cpp
)

# Client executable
add_executable(client main.cpp ${COMMON_SOURCE_FILES})
target_include_directories(client PRIVATE 
    ${SDL2_INCLUDE_DIRS}
    ${ENet_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
target_link_libraries(client PRIVATE ${SDL2_LIBRARIES} ${ENet_LIBRARIES})

# Server executable
add_executable(server server.cpp ${COMMON_SOURCE_FILES})
target_include_directories(server PRIVATE 
    ${SDL2_INCLUDE_DIRS}
    ${ENet_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
)
target_link_libraries(server PRIVATE ${SDL2_LIBRARIES} ${ENet_LIBRARIES})