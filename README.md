# Florr.io Clone

A C++ implementation of a P2P multiplayer game inspired by Florr.io.

## Requirements

- C++17 compatible compiler
- CMake 3.10 or higher
- nlohmann/json library (automatically downloaded by CMake)

## Building

1. Create a build directory:
```bash
mkdir build
cd build
```

2. Configure and build:
```bash
cmake ..
cmake --build .
```

## Running

The game can be run with the following command:
```bash
./florr_game [host] [port] [peer_host] [peer_port]
```

Arguments:
- `host`: The host address to bind to (default: "localhost")
- `port`: The port to bind to (default: 6666)
- `peer_host`: Optional peer host to connect to
- `peer_port`: Optional peer port to connect to

Example:
```bash
# Start first instance
./florr_game localhost 6666

# Start second instance and connect to first
./florr_game localhost 6667 localhost 6666
```

## Features

- P2P networking using TCP
- Multiple monster types with different behaviors
- Real-time position synchronization
- JSON-based message protocol
- Thread-safe game state management

## Implementation Details

The game is implemented using modern C++ features and follows these design principles:
- RAII for resource management
- Smart pointers for memory management
- Thread-safe operations using mutexes
- Exception handling for error management
- Cross-platform networking support