#include <SDL.h>
#include "flower.h"
#include "renderer.h"

// Function to draw the flower character
void drawFlower(SDL_Renderer* renderer, int x, int y, Eye eye) {
    // Draw outer circle (yellow-brown)
    SDL_SetRenderDrawColor(renderer, 207, 187, 80, 255); // #CFBB50
    drawFilledCircle(renderer, x, y, 26);

    // Draw inner circle (yellow)
    SDL_SetRenderDrawColor(renderer, 255, 231, 99, 255); // #FFE763
    drawFilledCircle(renderer, x, y, 23);

    // Draw smile using quadratic curve
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    const int numPoints = 20; // More points for smoother curve
    for (int i = 0; i <= numPoints; i++) {
        float t = i / (float)numPoints;
        // Quadratic curve: P(t) = (1-t)²P₀ + 2(1-t)tP₁ + t²P₂
        // P₀ = start point, P₁ = control point, P₂ = end point
        int px = x - 6 + t * 12; // Move from -6 to +6
        float curve = 4.5f * (1.0f - t) * t; // Quadratic curve factor
        int py = y + 10 + curve;
        SDL_RenderDrawPoint(renderer, px, py);
    }

    // Draw eyes with dynamic offsets
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    drawFilledEllipse(renderer, x - 7, y - 4, 3, 6); // Left eye
    drawFilledEllipse(renderer, x + 7, y - 4, 3, 6); // Right eye

    // Draw eye highlights with the offset applied
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    drawFilledCircle(renderer, x - 7 + eye.x, y - 4 + eye.y, 2); // Left eye highlight
    drawFilledCircle(renderer, x + 7 + eye.x, y - 4 + eye.y, 2); // Right eye highlight
}