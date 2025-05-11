#include "ladybug.h"
#include <SDL2/SDL.h>
#include "renderer.h"

// Helper function to draw a filled circle for the ladybug
void drawLadybugCircle(SDL_Renderer* renderer, int centerX, int centerY, int radius) {
    for (int w = 0; w < radius * 2; w++) {
        for (int h = 0; h < radius * 2; h++) {
            int dx = radius - w;
            int dy = radius - h;
            if ((dx*dx + dy*dy) <= (radius * radius)) {
                SDL_RenderDrawPoint(renderer, centerX + dx, centerY + dy);
            }
        }
    }
}

// Helper function to draw a circle outline
void drawCircleOutline(SDL_Renderer* renderer, int centerX, int centerY, int radius) {
    const int diameter = radius * 2;
    int x = radius - 1;
    int y = 0;
    int tx = 1;
    int ty = 1;
    int error = tx - diameter;

    while (x >= y) {
        // Each of the following renders an octant of the circle
        SDL_RenderDrawPoint(renderer, centerX + x, centerY - y);
        SDL_RenderDrawPoint(renderer, centerX + x, centerY + y);
        SDL_RenderDrawPoint(renderer, centerX - x, centerY - y);
        SDL_RenderDrawPoint(renderer, centerX - x, centerY + y);
        SDL_RenderDrawPoint(renderer, centerX + y, centerY - x);
        SDL_RenderDrawPoint(renderer, centerX + y, centerY + x);
        SDL_RenderDrawPoint(renderer, centerX - y, centerY - x);
        SDL_RenderDrawPoint(renderer, centerX - y, centerY + x);

        if (error <= 0) {
            ++y;
            error += ty;
            ty += 2;
        }

        if (error > 0) {
            --x;
            tx += 2;
            error += (tx - diameter);
        }
    }
}

// Helper function to draw a filled circle clipped by another circle
void drawClippedFilledCircle(SDL_Renderer* renderer, int circleX, int circleY, int circleRadius, 
                             int clipCenterX, int clipCenterY, int clipRadius, bool insideClip) {
    // Calculate bounding box with padding
    int minX = circleX - circleRadius - 1;
    int maxX = circleX + circleRadius + 1;
    int minY = circleY - circleRadius - 1;
    int maxY = circleY + circleRadius + 1;
    
    for (int px = minX; px <= maxX; px++) {
        for (int py = minY; py <= maxY; py++) {
            // Check if point is inside the circle we're drawing
            int dx = px - circleX;
            int dy = py - circleY;
            int distSq = dx*dx + dy*dy;
            
            if (distSq <= circleRadius * circleRadius) {
                // Check if point is inside/outside clip circle
                int clipDx = px - clipCenterX;
                int clipDy = py - clipCenterY;
                int clipDistSq = clipDx*clipDx + clipDy*clipDy;
                
                bool shouldDraw = insideClip ? 
                    (clipDistSq <= clipRadius * clipRadius) : 
                    (clipDistSq > clipRadius * clipRadius);
                
                if (shouldDraw) {
                    SDL_RenderDrawPoint(renderer, px, py);
                }
            }
        }
    }
}

// Render ladybug with given alpha
void renderLadybug(SDL_Renderer* renderer, int x, int y, int alpha) {
    // Enable blending for alpha
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    
    // Draw main body (red circle)
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, alpha);
    drawFilledCircle(renderer, x, y, 20);
    
    // Draw spots that are clipped within the ladybug's body
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
    
    // Draw the small spots - clipped to stay inside the body
    // These use the clip function with insideClip=true
    drawClippedFilledCircle(renderer, x - 3, y - 3, 3, x, y, 20, true);
    drawClippedFilledCircle(renderer, x + 3, y - 3, 3, x, y, 20, true);
    drawClippedFilledCircle(renderer, x, y + 3, 3, x, y, 20, true);
    
    // Draw the large 15px radius black spot, 12px to the right of center
    // This is semi-transparent to show overlapping with the ladybug
    drawClippedFilledCircle(renderer, x + 12, y, 10, x, y, 20, true);
}