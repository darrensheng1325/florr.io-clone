#include <SDL2/SDL.h>
#include <enet/enet.h>
#include <iostream>
#include <cmath>
#include <vector>
#include <map>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <chrono>
#include "flower.h"
#include "renderer.h"
#include "ladybug.h"

// Network constants
const char* SERVER_HOST = "localhost";
const int SERVER_PORT = 1234;

// Network message types (must match server)
enum MessageType {
    MSG_PLAYER_UPDATE = 0,
    MSG_MOB_UPDATE = 1,
    MSG_PLAYER_JOIN = 2,
    MSG_PLAYER_LEAVE = 3
};

// Network message structures (must match server)
#pragma pack(push, 1)
struct PlayerUpdateMessage {
    uint8_t type;
    float x, y;
    float vx, vy;
};

struct MobUpdateMessage {
    uint8_t type;
    float x, y;
    float vx, vy;
    int mobType;
    int health;
};
#pragma pack(pop)

// Game constants
const int WINDOW_WIDTH = 800;
const int WINDOW_HEIGHT = 600;
const int WORLD_WIDTH = 1600;
const int WORLD_HEIGHT = 1200;
const int PLAYER_SIZE = 50;
const int PLAYER_SPEED = 500;

// Network state
ENetHost* client;
ENetPeer* serverPeer;
bool connected = false;

// Game state
struct RemotePlayer {
    float x, y;
    float vx, vy;
};

struct NetworkMob {
    float x, y;
    float vx, vy;
    int type;
    int health;
};

std::map<ENetPeer*, RemotePlayer> remotePlayers;
std::vector<NetworkMob> networkMobs;

struct SpinningCircle {
    float angle; // Angle in radians
    int radius;  // Distance from the player
};

struct Mob {
    int x, y;
    int alpha; // Transparency level
    int radius; // Size of the mob
    std::function<void(SDL_Renderer*, int, int, int)> render; // Render function
    std::function<void(Mob&, int, int)> update; // Update function
};

// Local visual effect mobs
std::vector<Mob> mobs;

struct PhysicsObject {
    float x, y;       // Position
    float vx, vy;     // Velocity
    float ax, ay;     // Acceleration
};

// Player physics
PhysicsObject playerPhysics = {WORLD_WIDTH / 2.0f, WORLD_HEIGHT / 2.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// Initialize eyeOffset with default values
Eye eyeOffset = {0, 0};

// List of spinning circles
std::vector<SpinningCircle> spinningCircles;

// Function to draw the green ground and grid
void drawGroundAndGrid(SDL_Renderer* renderer, int cameraX, int cameraY) {
    // Draw green ground
    SDL_SetRenderDrawColor(renderer, 34, 139, 34, 255); // Forest green
    SDL_Rect groundRect = { -cameraX, -cameraY, WORLD_WIDTH, WORLD_HEIGHT };
    SDL_RenderFillRect(renderer, &groundRect);

    // Draw grid
    SDL_SetRenderDrawColor(renderer, 28, 115, 28, 255); // Slightly darker green
    const int gridSize = 50;
    for (int x = 0; x <= WORLD_WIDTH; x += gridSize) {
        SDL_RenderDrawLine(renderer, x - cameraX, 0 - cameraY, x - cameraX, WORLD_HEIGHT - cameraY);
    }
    for (int y = 0; y <= WORLD_HEIGHT; y += gridSize) {
        SDL_RenderDrawLine(renderer, 0 - cameraX, y - cameraY, WORLD_WIDTH - cameraX, y - cameraY);
    }
}

// Function to initialize spinning circles
void initializeSpinningCircles() {
    for (int i = 0; i < 5; i++) {
        SpinningCircle circle;
        circle.angle = i * (2 * M_PI / 5); // Evenly spaced angles
        circle.radius = 80; // Distance from the player
        spinningCircles.push_back(circle);
    }
}

// Function to update and render spinning circles
void updateAndRenderSpinningCircles(SDL_Renderer* renderer, int playerX, int playerY, int cameraX, int cameraY, bool spacePressed) {
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255); // White color
    for (auto& circle : spinningCircles) {
        // Update angle to make the circles spin
        circle.angle += 0.05f;
        if (circle.angle > 2 * M_PI) {
            circle.angle -= 2 * M_PI;
        }

        // Adjust radius based on space key
        int dynamicRadius = spacePressed ? circle.radius + 20 : circle.radius;

        // Calculate circle position
        int x = playerX + std::cos(circle.angle) * dynamicRadius - cameraX;
        int y = playerY + std::sin(circle.angle) * dynamicRadius - cameraY;

        // Render the circle
        drawFilledCircle(renderer, x, y, 5);
    }
}

// Function to check if a bubble touches the petals
bool isBubbleTouchingPetals(const Mob& mob, int playerX, int playerY) {
    for (const auto& circle : spinningCircles) {
        int petalX = playerX + std::cos(circle.angle) * circle.radius;
        int petalY = playerY + std::sin(circle.angle) * circle.radius;
        int dx = mob.x - petalX;
        int dy = mob.y - petalY;
        if (std::sqrt(dx * dx + dy * dy) <= 15) { // 15 accounts for bubble and petal radii
            return true;
        }
    }
    return false;
}

// Function to spawn a generic mob
void spawnMob(int x, int y, int alpha, int radius, 
              std::function<void(SDL_Renderer*, int, int, int)> renderFunc,
              std::function<void(Mob&, int, int)> updateFunc) {
    Mob mob;
    mob.x = x;
    mob.y = y;
    mob.alpha = alpha;
    mob.radius = radius;
    mob.render = renderFunc;
    mob.update = updateFunc;
    // Local mobs used for rendering effects
    mobs.push_back(mob);
}

// Update function for bubbles
void updateBubble(Mob& mob, int playerX, int playerY) {
    // Check if the bubble is touching the petals
    if (isBubbleTouchingPetals(mob, playerX, playerY)) {
        mob.alpha -= 20; // Fade out quickly
    }
}

// Update physics for a PhysicsObject
void updatePhysics(PhysicsObject& obj, float deltaTime) {
    obj.vx += obj.ax * deltaTime;
    obj.vy += obj.ay * deltaTime;
    obj.x += obj.vx * deltaTime;
    obj.y += obj.vy * deltaTime;

    // Apply reduced friction to make the player feel lighter
    obj.vx *= 0.98f; // Reduced friction
    obj.vy *= 0.98f; // Reduced friction
}

// Update player physics based on input
void handlePlayerInput(const Uint8* state, PhysicsObject& player) {
    player.ax = 0.0f;
    player.ay = 0.0f;

    if (state[SDL_SCANCODE_LEFT]) {
        player.ax = -PLAYER_SPEED;
    }
    if (state[SDL_SCANCODE_RIGHT]) {
        player.ax = PLAYER_SPEED;
    }
    if (state[SDL_SCANCODE_UP]) {
        player.ay = -PLAYER_SPEED;
    }
    if (state[SDL_SCANCODE_DOWN]) {
        player.ay = PLAYER_SPEED;
    }
}

// Update mob physics (example: simple downward gravity)
void updateMobPhysics(Mob& mob, float deltaTime) {
    mob.y += 50 * deltaTime; // Gravity effect
}

// Update eye offset based on active controls
void updateEyeOffset(Eye& eye, const Uint8* state) {
    // Set limits for eye movement
    const int X_LIMIT = 2;
    const int Y_LIMIT = 3;
    
    // Move eyes based on key presses, respecting limits
    if (state[SDL_SCANCODE_LEFT]) {
        eye.x = std::max(eye.x - 1, -X_LIMIT);  // Move eyes left with a limit
    }
    else if (state[SDL_SCANCODE_RIGHT]) {
        eye.x = std::min(eye.x + 1, X_LIMIT);   // Move eyes right with a limit
    }
    // No else clause to reset x position

    if (state[SDL_SCANCODE_UP]) {
        eye.y = std::max(eye.y - 1, -Y_LIMIT);  // Move eyes up with a limit
    }
    else if (state[SDL_SCANCODE_DOWN]) {
        eye.y = std::min(eye.y + 1, Y_LIMIT);   // Move eyes down with a limit
    }
    // No else clause to reset y position
    
    // Eye positions will remain at their last set values when keys are released
}

// Function to update and render all mobs
void updateAndRenderMobs(SDL_Renderer* renderer, int cameraX, int cameraY, int playerX, int playerY) {
    // Handle local effect mobs (not network mobs)
    for (auto it = mobs.begin(); it != mobs.end();) {
        // Update mob
        it->update(*it, playerX, playerY);

        // Remove mob if fully transparent
        if (it->alpha <= 0) {
            it = mobs.erase(it);
        } else {
            // Render mob
            it->render(renderer, it->x - cameraX, it->y - cameraY, it->alpha);
            ++it;
        }
    }
}

// Function to connect to the server
bool connectToServer() {
    if (enet_initialize() != 0) {
        std::cerr << "Failed to initialize ENet" << std::endl;
        return false;
    }

    client = enet_host_create(nullptr, 1, 2, 0, 0);
    if (client == nullptr) {
        std::cerr << "Failed to create ENet client" << std::endl;
        enet_deinitialize();
        return false;
    }

    ENetAddress address;
    enet_address_set_host(&address, SERVER_HOST);
    address.port = SERVER_PORT;

    serverPeer = enet_host_connect(client, &address, 2, 0);
    if (serverPeer == nullptr) {
        std::cerr << "Failed to initiate connection" << std::endl;
        enet_host_destroy(client);
        enet_deinitialize();
        return false;
    }

    return true;
}

// Function to send player position to server
void sendPlayerPosition(float x, float y, float vx, float vy) {
    if (!connected) return;

    PlayerUpdateMessage msg;
    msg.type = MSG_PLAYER_UPDATE;
    msg.x = x;
    msg.y = y;
    msg.vx = vx;
    msg.vy = vy;

    ENetPacket* packet = enet_packet_create(&msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(serverPeer, 0, packet) != 0) {
        // If sending fails, we need to destroy the packet to avoid leak
        enet_packet_destroy(packet);
    }
    // Note: If sending succeeds, ENet will destroy the packet for us
}

// Function to clean up network mobs that are too far from the player or exceed the limit
void cleanupNetworkMobs(float playerX, float playerY) {
    constexpr float MAX_DISTANCE = 2000.0f; // Mobs further than this will be removed
    constexpr size_t MAX_MOBS = 100; // Maximum number of mobs to keep in memory
    
    // Remove mobs that are too far away
    auto it = networkMobs.begin();
    while (it != networkMobs.end()) {
        float dx = it->x - playerX;
        float dy = it->y - playerY;
        float distanceSquared = dx*dx + dy*dy;
        
        if (distanceSquared > MAX_DISTANCE * MAX_DISTANCE) {
            it = networkMobs.erase(it);
        } else {
            ++it;
        }
    }
    
    // If we still have too many mobs, remove the oldest ones
    if (networkMobs.size() > MAX_MOBS) {
        // Remove excess mobs, keeping the newest ones
        networkMobs.erase(networkMobs.begin(), networkMobs.begin() + (networkMobs.size() - MAX_MOBS));
    }
}

int main(int argc, char* argv[]) {
    if (!connectToServer()) {
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Flower Game",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_SHOWN
    );

    if (!window) {
        std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::cerr << "Renderer creation failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Initialize game state
    auto lastTime = std::chrono::high_resolution_clock::now();
    initializeSpinningCircles();

    bool running = true;
    SDL_Event event;

    while (running) {
        // Handle SDL events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        // Calculate delta time
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
        lastTime = currentTime;

        // Handle network events
        ENetEvent netEvent;
        while (enet_host_service(client, &netEvent, 0) > 0) {
            switch (netEvent.type) {
                case ENET_EVENT_TYPE_NONE:
                    break;

                case ENET_EVENT_TYPE_CONNECT:
                    std::cout << "Connected to server" << std::endl;
                    connected = true;
                    break;

                case ENET_EVENT_TYPE_RECEIVE:
                    if (netEvent.packet->dataLength >= sizeof(uint8_t)) {
                        uint8_t msgType = netEvent.packet->data[0];

                        if (msgType == MSG_PLAYER_UPDATE && 
                            netEvent.packet->dataLength == sizeof(PlayerUpdateMessage)) {
                            PlayerUpdateMessage* msg = (PlayerUpdateMessage*)netEvent.packet->data;
                            RemotePlayer& player = remotePlayers[netEvent.peer];
                            player.x = msg->x;
                            player.y = msg->y;
                            player.vx = msg->vx;
                            player.vy = msg->vy;
                        }
                        else if (msgType == MSG_MOB_UPDATE && 
                                netEvent.packet->dataLength == sizeof(MobUpdateMessage)) {
                            MobUpdateMessage* msg = (MobUpdateMessage*)netEvent.packet->data;
                            // Update or add mob
                            bool found = false;
                            
                            // Use a more robust method to identify mobs - check for proximity rather than exact position
                            const float proximityThreshold = 5.0f; // Consider mobs within 5 units to be the same
                            
                            for (auto& mob : networkMobs) {
                                float dx = mob.x - msg->x;
                                float dy = mob.y - msg->y;
                                float distanceSquared = dx*dx + dy*dy;
                                
                                // If a mob is found nearby, consider it the same mob and update it
                                if (distanceSquared < proximityThreshold * proximityThreshold) {
                                    mob.x = msg->x;  // Update to exact position
                                    mob.y = msg->y;
                                    mob.vx = msg->vx;
                                    mob.vy = msg->vy;
                                    mob.type = msg->mobType;
                                    mob.health = msg->health;
                                    found = true;
                                    break;
                                }
                            }
                            
                            if (!found) {
                                NetworkMob mob;
                                mob.x = msg->x;
                                mob.y = msg->y;
                                mob.vx = msg->vx;
                                mob.vy = msg->vy;
                                mob.type = msg->mobType;
                                mob.health = msg->health;
                                networkMobs.push_back(mob);
                            }
                        }
                    }
                    enet_packet_destroy(netEvent.packet);
                    break;

                case ENET_EVENT_TYPE_DISCONNECT:
                    std::cout << "Disconnected from server" << std::endl;
                    connected = false;
                    // Clear all remote players and network mobs when disconnected
                    remotePlayers.clear();
                    networkMobs.clear();
                    break;
            }
        }

        // Update player physics
        const Uint8* state = SDL_GetKeyboardState(nullptr);
        handlePlayerInput(state, playerPhysics);
        updatePhysics(playerPhysics, deltaTime);

        // Send player position to server
        if (connected) {
            sendPlayerPosition(playerPhysics.x, playerPhysics.y, 
                             playerPhysics.vx, playerPhysics.vy);
        }

        // Update eye offset
        updateEyeOffset(eyeOffset, state);

        // Clear screen
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        // Calculate camera position (centered on player)
        int cameraX = playerPhysics.x - WINDOW_WIDTH / 2;
        int cameraY = playerPhysics.y - WINDOW_HEIGHT / 2;

        // Draw ground and grid
        drawGroundAndGrid(renderer, cameraX, cameraY);
        drawFlower(renderer, WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2, eyeOffset);

        // Draw remote players
        if (remotePlayers.size() > 1) {
            for (const auto& [peer, player] : remotePlayers) {
                SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255); // Blue for other players
                drawFlower(renderer, player.x - cameraX, player.y - cameraY, eyeOffset);
            }
        }

        // Draw network mobs
        for (const auto& mob : networkMobs) {
            SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red for mobs
            renderLadybug(renderer, mob.x - cameraX, mob.y - cameraY, 255);
        }

        // Update and render spinning circles
        updateAndRenderSpinningCircles(renderer, 
                                     playerPhysics.x, 
                                     playerPhysics.y, 
                                     cameraX, 
                                     cameraY, 
                                     state[SDL_SCANCODE_SPACE]);

        // Update and render mobs
        updateAndRenderMobs(renderer, cameraX, cameraY, playerPhysics.x, playerPhysics.y);
        
        // Clean up network mobs to prevent memory leaks
        cleanupNetworkMobs(playerPhysics.x, playerPhysics.y);

        // Present render
        SDL_RenderPresent(renderer);

        // Cap frame rate
        SDL_Delay(16); // Approximately 60 FPS
    }

    // Cleanup
    if (connected) {
        enet_peer_disconnect(serverPeer, 0);
        
        // Wait for disconnect to complete
        ENetEvent event;
        bool disconnected = false;
        uint32_t timeout = 3000; // 3 seconds timeout
        uint32_t start = SDL_GetTicks();
        
        // Wait for the disconnect event or timeout
        while (SDL_GetTicks() - start < timeout && !disconnected) {
            while (enet_host_service(client, &event, 100) > 0) {
                if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    disconnected = true;
                    break;
                }
                
                // Destroy any received packets during shutdown
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                }
            }
            
            if (disconnected) {
                break;
            }
        }
        
        // If we didn't get a disconnect event, force it
        if (!disconnected) {
            enet_peer_reset(serverPeer);
        }
        
        enet_host_flush(client);
    }
    
    // Clean up all resources
    remotePlayers.clear();
    networkMobs.clear();
    mobs.clear();
    
    enet_host_destroy(client);
    enet_deinitialize();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}