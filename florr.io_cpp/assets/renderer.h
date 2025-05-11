#ifndef RENDERER_H
#define RENDERER_H

#include <SDL2/SDL.h>

// Draw a filled circle
void drawFilledCircle(SDL_Renderer* renderer, int centerX, int centerY, int radius);

// Draw a filled ellipse
void drawFilledEllipse(SDL_Renderer* renderer, int centerX, int centerY, int radiusX, int radiusY);

#endif // RENDERER_H