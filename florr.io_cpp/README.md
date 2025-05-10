# Simple SDL2 Game

A simple game where you control a red rectangle using arrow keys.

## Prerequisites

- CMake (version 3.10 or higher)
- C++ compiler with C++17 support
- SDL2 development libraries

### Installing SDL2

#### macOS
```bash
brew install sdl2
```

#### Ubuntu/Debian
```bash
sudo apt-get install libsdl2-dev
```

#### Windows
Download SDL2 development libraries from the [official website](https://www.libsdl.org/download-2.0.php) and set up your environment variables accordingly.

## Building the Game

1. Create a build directory:
```bash
mkdir build
cd build
```

2. Generate build files:
```bash
cmake ..
```

3. Build the project:
```bash
cmake --build .
```

## Running the Game

After building, run the executable:
```bash
./SimpleGame
```

## Controls

- Arrow keys to move the red rectangle
- Close the window to exit the game 