#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <SFML/Graphics.hpp>
#include <nlohmann/json.hpp>
#include "monster.hpp"

using json = nlohmann::json;

class GameEngine {
public:
    GameEngine();
    ~GameEngine();

    void add_player(const std::string& player_id);
    json handle_local_input();
    void update_player(const std::string& player_id, double x, double y);
    void update_monster_positions(const std::map<int, json>& monster_data);
    std::map<int, json> get_monster_positions() const;
    json get_player_position(const std::string& player_id) const;
    void render();
    void handle_input();

    // Game state
    std::string game_state = "playing";
    bool is_host = false;
    std::string my_id;
    int max_fps = 144;
    bool test_connection = false;
    std::string connect_ip;

private:
    void initialize_graphics();
    void draw_monsters();
    void draw_players();
    void draw_ui();

    std::map<std::string, json> players_;
    std::map<int, std::unique_ptr<Monster>> monsters_;
    mutable std::mutex game_mutex_;
    double last_monster_sync_;
    
    // Graphics
    std::unique_ptr<sf::RenderWindow> window_;
    sf::View game_view_;
    sf::Font font_;
    std::map<std::string, sf::Color> player_colors_;
    
    // Game constants
    static constexpr float WORLD_WIDTH = 2000.f;
    static constexpr float WORLD_HEIGHT = 2000.f;
    static constexpr float PLAYER_SPEED = 5.f;
    static constexpr int WINDOW_WIDTH = 800;
    static constexpr int WINDOW_HEIGHT = 600;
}; 