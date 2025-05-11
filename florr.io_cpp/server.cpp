#include <enet/enet.h>
#include <iostream>
#include <vector>
#include <map>
#include <cstring>
#include <random>
#include <chrono>
#include <csignal>
#include <atomic>

// Constants
const int SERVER_PORT = 1234;
const int MAX_CLIENTS = 32;
const int TICK_RATE = 60; // Server updates per second

// Player structure
struct Player {
    float x, y;
    float vx, vy;
    ENetPeer* peer;
};

// Mob structure
struct Mob {
    float x, y;
    float vx, vy;
    int type;
    int health;
};

// Game state
std::map<ENetPeer*, Player> players;
std::vector<Mob> mobs;
std::random_device rd;
std::mt19937 gen(rd());

// Network message types
enum MessageType {
    MSG_PLAYER_UPDATE = 0,
    MSG_MOB_UPDATE = 1,
    MSG_PLAYER_JOIN = 2,
    MSG_PLAYER_LEAVE = 3
};

// Message structures
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

// Global flag for server running state
std::atomic<bool> serverRunning(true);

// Signal handler for graceful shutdown
void signalHandler(int signal) {
    std::cout << "Received signal " << signal << ", shutting down..." << std::endl;
    serverRunning = false;
}

// Function to spawn a new mob
void spawnMob() {
    std::uniform_real_distribution<float> xDist(0, 1600);
    std::uniform_real_distribution<float> yDist(0, 1200);
    std::uniform_real_distribution<float> vDist(-50, 50);
    
    Mob mob;
    mob.x = xDist(gen);
    mob.y = yDist(gen);
    mob.vx = vDist(gen);
    mob.vy = vDist(gen);
    mob.type = 0; // Basic mob type
    mob.health = 100;
    
    mobs.push_back(mob);
}

// Function to broadcast player positions to all clients
void broadcastPlayerPositions(ENetHost* server) {
    for (const auto& [peer, player] : players) {
        PlayerUpdateMessage msg;
        msg.type = MSG_PLAYER_UPDATE;
        msg.x = player.x;
        msg.y = player.y;
        msg.vx = player.vx;
        msg.vy = player.vy;
        
        ENetPacket* packet = enet_packet_create(&msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(server, 0, packet);
        // ENet will destroy the packet for us after broadcast
    }
}

// Function to broadcast mob positions to all clients
void broadcastMobPositions(ENetHost* server) {
    for (const auto& mob : mobs) {
        MobUpdateMessage msg;
        msg.type = MSG_MOB_UPDATE;
        msg.x = mob.x;
        msg.y = mob.y;
        msg.vx = mob.vx;
        msg.vy = mob.vy;
        msg.mobType = mob.type;
        msg.health = mob.health;
        
        ENetPacket* packet = enet_packet_create(&msg, sizeof(msg), ENET_PACKET_FLAG_RELIABLE);
        enet_host_broadcast(server, 0, packet);
        // ENet will destroy the packet for us after broadcast
    }
}

// Function to update mob positions
void updateMobs(float deltaTime) {
    // Limit the total number of mobs to prevent server memory issues
    const size_t MAX_MOBS = 100;
    if (mobs.size() > MAX_MOBS) {
        // Remove excess mobs
        mobs.erase(mobs.begin(), mobs.begin() + (mobs.size() - MAX_MOBS));
    }
    
    for (auto& mob : mobs) {
        mob.x += mob.vx * deltaTime;
        mob.y += mob.vy * deltaTime;
        
        // Bounce off world boundaries
        if (mob.x < 0 || mob.x > 1600) mob.vx *= -1;
        if (mob.y < 0 || mob.y > 1200) mob.vy *= -1;
    }
}

int main() {
    // Set up signal handling for clean shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    if (enet_initialize() != 0) {
        std::cerr << "Failed to initialize ENet" << std::endl;
        return 1;
    }
    
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = SERVER_PORT;
    
    ENetHost* server = enet_host_create(&address, MAX_CLIENTS, 2, 0, 0);
    if (server == nullptr) {
        std::cerr << "Failed to create ENet server" << std::endl;
        enet_deinitialize();
        return 1;
    }
    
    std::cout << "Server started on port " << SERVER_PORT << std::endl;
    
    // Spawn initial mobs
    for (int i = 0; i < 10; i++) {
        spawnMob();
    }
    
    auto lastTick = std::chrono::high_resolution_clock::now();
    
    while (serverRunning) {
        ENetEvent event;
        while (enet_host_service(server, &event, 0) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_NONE:
                    break;
                    
                case ENET_EVENT_TYPE_CONNECT: {
                    std::cout << "Client connected from " 
                              << event.peer->address.host << ":"
                              << event.peer->address.port << std::endl;
                    
                    // Initialize new player
                    Player player;
                    player.x = 800; // Center of world
                    player.y = 600;
                    player.vx = 0;
                    player.vy = 0;
                    player.peer = event.peer;
                    players[event.peer] = player;
                    break;
                }
                
                case ENET_EVENT_TYPE_RECEIVE: {
                    if (event.packet->dataLength >= sizeof(uint8_t)) {
                        uint8_t msgType = event.packet->data[0];
                        
                        if (msgType == MSG_PLAYER_UPDATE && 
                            event.packet->dataLength == sizeof(PlayerUpdateMessage)) {
                            PlayerUpdateMessage* msg = (PlayerUpdateMessage*)event.packet->data;
                            auto it = players.find(event.peer);
                            if (it != players.end()) {
                                it->second.x = msg->x;
                                it->second.y = msg->y;
                                it->second.vx = msg->vx;
                                it->second.vy = msg->vy;
                            }
                        }
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                
                case ENET_EVENT_TYPE_DISCONNECT: {
                    std::cout << "Client disconnected" << std::endl;
                    players.erase(event.peer);
                    break;
                }
            }
        }
        
        // Server tick
        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTick).count();
        
        if (deltaTime >= 1.0f / TICK_RATE) {
            updateMobs(deltaTime);
            broadcastPlayerPositions(server);
            broadcastMobPositions(server);
            lastTick = now;
        }
        
        // Spawn new mobs occasionally
        if (rand() % 100 == 0 && mobs.size() < 50) {
            spawnMob();
        }
    }
    
    // Cleanup on shutdown
    std::cout << "Server shutting down, cleaning up resources..." << std::endl;
    
    // Disconnect all peers
    for (auto& [peer, player] : players) {
        enet_peer_disconnect(peer, 0);
    }
    
    // Wait briefly for disconnect messages to be sent
    ENetEvent event;
    int waitCount = 0;
    while (waitCount < 10 && enet_host_service(server, &event, 100) > 0) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
        waitCount++;
    }
    
    // Clear all game state
    players.clear();
    mobs.clear();
    
    enet_host_destroy(server);
    enet_deinitialize();
    std::cout << "Server shutdown complete" << std::endl;
    return 0;
} 