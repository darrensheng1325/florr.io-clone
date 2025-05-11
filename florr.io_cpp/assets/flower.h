#ifndef FLOWER_H
#define FLOWER_H

#include <SDL2/SDL.h>

// Include the eye structure
struct Eye {
    int x, y;
};

// Draw the player flower with eye movement
void drawFlower(SDL_Renderer* renderer, int x, int y, const Eye& eyeOffset);

#endif // FLOWER_H
