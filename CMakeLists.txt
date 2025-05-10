cmake_minimum_required(VERSION 3.10)
project(florr_game)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set SFML paths for macOS
set(SFML_DIR "/opt/homebrew/lib/cmake/SFML")
set(SFML_ROOT "/opt/homebrew")

# Set policy to use SFML_ROOT
cmake_policy(SET CMP0074 NEW)

# Find SFML
find_package(SFML 3.0 COMPONENTS Graphics Window System REQUIRED)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${SFML_INCLUDE_DIRS}
    /opt/homebrew/include
)

# Add link directories
link_directories(
    /opt/homebrew/lib
)

# Add executable
add_executable(florr_game 
    src/main.cpp
    src/game_engine.cpp
    src/game_socket.cpp
    src/monster.cpp
    src/p2p_game.cpp
)

# Link libraries
target_link_libraries(florr_game
    PRIVATE
    sfml-graphics
    sfml-window
    sfml-system
    pthread
)

# Add nlohmann_json
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)
target_link_libraries(florr_game PRIVATE nlohmann_json::nlohmann_json)