#include <SDL.h>
void drawFilledCircle(SDL_Renderer* renderer, int centerX, int centerY, int radius) {
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x*x + y*y <= radius*radius) {
                SDL_RenderDrawPoint(renderer, centerX + x, centerY + y);
            }
        }
    }
}

// Helper function to draw a filled ellipse
void drawFilledEllipse(SDL_Renderer* renderer, int centerX, int centerY, int radiusX, int radiusY) {
    for (int y = -radiusY; y <= radiusY; y++) {
        for (int x = -radiusX; x <= radiusX; x++) {
            if ((x*x)/(float)(radiusX*radiusX) + (y*y)/(float)(radiusY*radiusY) <= 1.0f) {
                SDL_RenderDrawPoint(renderer, centerX + x, centerY + y);
            }
        }
    }
}

void drawOutlineCircle(SDL_Renderer* renderer, int centerX, int centerY, int radius) {
    for (int w = 0; w < radius * 2; w++) {
        for (int h = 0; h < radius * 2; h++) {
            int dx = radius - w; // horizontal offset
            int dy = radius - h; // vertical offset
            if ((dx*dx + dy*dy) <= (radius * radius)) {
                SDL_RenderDrawPoint(renderer, centerX + dx, centerY + dy);
            }
        }
    }
}