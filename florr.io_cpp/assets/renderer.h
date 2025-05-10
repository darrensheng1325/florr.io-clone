#ifndef RENDERER_H
#define RENDERER_H
#include <SDL.h>

void drawFilledCircle(SDL_Renderer* renderer, int centerX, int centerY, int radius);
void drawFilledEllipse(SDL_Renderer* renderer, int centerX, int centerY, int radiusX, int radiusY);
void drawOutlineCircle(SDL_Renderer* renderer, int centerX, int centerY, int radius);

struct Eye {
    int x;
    int y;
};

#endif // RENDERER_H