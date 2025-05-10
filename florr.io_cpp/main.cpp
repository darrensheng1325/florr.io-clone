#include <SDL.h>
#include <iostream>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <functional>
#include "flower.h"
#include "renderer.h"

const int WINDOW_WIDTH = 800;
const int WINDOW_HEIGHT = 600;
const int WORLD_WIDTH = 1600; // Larger world width
const int WORLD_HEIGHT = 1200; // Larger world height
const int PLAYER_SIZE = 50;
const int PLAYER_SPEED = 500;

// Struct to represent eye offsets

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

// List of mobs
std::vector<Mob> mobs;

// Helper function to draw a filled circle

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
    mobs.push_back(mob);
}

// Render function for bubbles
void renderBubble(SDL_Renderer* renderer, int x, int y, int alpha) {
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, alpha);
    drawFilledCircle(renderer, x, y, 20);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha / 2);
    drawFilledCircle(renderer, x+10, y, 10);
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

int main(int argc, char* argv[]) {
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

    // Camera position
    int cameraX = 0;
    int cameraY = 0;

    bool running = true;
    SDL_Event event;

    // Seed random number generator
    srand(static_cast<unsigned>(time(0)));

    // Initialize spinning circles
    initializeSpinningCircles();

    while (running) {
        // Handle events
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        // Handle keyboard input
        const Uint8* state = SDL_GetKeyboardState(NULL);
        handlePlayerInput(state, playerPhysics);

        // Update player physics
        updatePhysics(playerPhysics, 0.016f); // Assuming ~60 FPS (16ms per frame)

        // Keep player within world bounds
        if (playerPhysics.x < 0) playerPhysics.x = 0;
        if (playerPhysics.x > WORLD_WIDTH) playerPhysics.x = WORLD_WIDTH;
        if (playerPhysics.y < 0) playerPhysics.y = 0;
        if (playerPhysics.y > WORLD_HEIGHT) playerPhysics.y = WORLD_HEIGHT;

        // Update eye offset based on active controls
        updateEyeOffset(eyeOffset, state);

        // Update camera position to follow the player
        cameraX = static_cast<int>(playerPhysics.x) - WINDOW_WIDTH / 2;
        cameraY = static_cast<int>(playerPhysics.y) - WINDOW_HEIGHT / 2;

        // Clamp camera to world bounds
        if (cameraX < 0) cameraX = 0;
        if (cameraX > WORLD_WIDTH - WINDOW_WIDTH) cameraX = WORLD_WIDTH - WINDOW_WIDTH;
        if (cameraY < 0) cameraY = 0;
        if (cameraY > WORLD_HEIGHT - WINDOW_HEIGHT) cameraY = WORLD_HEIGHT - WINDOW_HEIGHT;

        // Update mobs' physics
        for (auto& mob : mobs) {
            updateMobPhysics(mob, 0.016f);
        }

        // Clear screen
        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red for out-of-bounds
        SDL_RenderClear(renderer);

        // Draw green ground and grid
        drawGroundAndGrid(renderer, cameraX, cameraY);

        // Draw player
        drawFlower(renderer, static_cast<int>(playerPhysics.x) - cameraX, static_cast<int>(playerPhysics.y) - cameraY, eyeOffset);

        // Check if space key is pressed
        bool spacePressed = state[SDL_SCANCODE_SPACE];

        // Draw spinning circles with dynamic radius
        updateAndRenderSpinningCircles(renderer, static_cast<int>(playerPhysics.x), static_cast<int>(playerPhysics.y), cameraX, cameraY, spacePressed);

        // Randomly spawn bubbles
        if (rand() % 100 < 2) { // 2% chance per frame
            spawnMob(rand() % WORLD_WIDTH, rand() % WORLD_HEIGHT, 255, 10, renderBubble, updateBubble);
        }

        // Update and render mobs
        updateAndRenderMobs(renderer, cameraX, cameraY, static_cast<int>(playerPhysics.x), static_cast<int>(playerPhysics.y));

        // Update screen
        SDL_RenderPresent(renderer);

        // Cap frame rate
        SDL_Delay(16); // Approximately 60 FPS
    }

    // Cleanup
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}