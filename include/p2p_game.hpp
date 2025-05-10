#pragma once

#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <memory>
#include <queue>
#include <set>
#include <functional>
#include <nlohmann/json.hpp>
#include "game_engine.hpp"
#include "game_socket.hpp"

using json = nlohmann::json;

class P2PGame {
public:
    P2PGame(const std::string& host, int port);
    ~P2PGame();

    void start();
    void connect_to_peer(const std::string& peer_host, int peer_port);
    bool test_peer_connection(const std::string& peer_host, int peer_port);

private:
    void accept_connections();
    void handle_peer(std::shared_ptr<GameSocket> peer_socket, const std::string& address);
    void broadcast_monster_positions(const std::map<int, json>& monster_data);
    void send_data(const json& data, std::shared_ptr<GameSocket> peer_socket);
    void broadcast_message(const json& message);
    void broadcast_position(const json& position);
    void receive_data();
    void handle_item_pickup(const json& item);

    std::unique_ptr<GameEngine> game_;
    std::unique_ptr<GameSocket> socket_;
    std::map<std::string, std::shared_ptr<GameSocket>> peers_;
    std::mutex peers_mutex_;
    
    std::thread accept_thread_;
    std::thread receive_thread_;
    
    double last_monster_sync_;
    const double monster_sync_interval_ = 0.05;
    
    const std::set<std::string, std::less<std::string>> valid_monster_types_ = {
        "Mouse", "Cat", "Tank", "Bush", "Rock", 
        "Ant", "Bee", "Boss", "Tree", "StaticMonster",
        "Bird"
    };
}; 