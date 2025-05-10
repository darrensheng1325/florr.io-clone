#pragma once

#include <string>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Forward declaration
class GameEngine;

class Monster {
public:
    Monster(float x, float y, int health, const std::string& type);
    virtual ~Monster() = default;

    virtual void update() = 0;
    virtual json to_json() const;
    
    float get_x() const { return x_; }
    float get_y() const { return y_; }
    int get_health() const { return health_; }
    std::string get_type() const { return type_; }

protected:
    float x_;
    float y_;
    int health_;
    std::string type_;
    float target_x_;
    float target_y_;

    // Allow GameEngine to access protected members
    friend class GameEngine;
};

// Concrete monster classes
class Mouse : public Monster {
public:
    Mouse(float x, float y);
    void update() override;
};

class Cat : public Monster {
public:
    Cat(float x, float y);
    void update() override;
};

class Tank : public Monster {
public:
    Tank(float x, float y);
    void update() override;
};

class Bush : public Monster {
public:
    Bush(float x, float y);
    void update() override;
};

class Rock : public Monster {
public:
    Rock(float x, float y);
    void update() override;
};

class Ant : public Monster {
public:
    Ant(float x, float y);
    void update() override;
};

class Bee : public Monster {
public:
    Bee(float x, float y);
    void update() override;
};

class Boss : public Monster {
public:
    Boss(float x, float y);
    void update() override;
};

class Bird : public Monster {
public:
    Bird(float x, float y);
    void update() override;
}; 