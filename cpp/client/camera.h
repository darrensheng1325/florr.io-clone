#pragma once
// World <-> screen mapping.
//
// Two decisions here look wrong in isolation and are not.
//
//  * Zoom is flat. `Graphics.zoomLevel` (src/graphics/core.ts) is 1.0 -- the
//    player's zoom setting times whatever a petal asks for -- and the world
//    transform is nothing but ctx.scale(zoomLevel, zoomLevel)
//    (src/graphics/render.ts). This camera keeps that, and it is NOT what
//    stops the game zooming out when the window is resized: the viewport it
//    is handed is in DESIGN units, not pixels, so it spans at most the same
//    1920x1080 of world at every window size and on every display -- exactly
//    that at 16:9, and less on the short axis of any other shape. The scale
//    that absorbs the window's real size is a single base transform on the
//    whole frame (see kDesignWidth in client/app.cpp), applied to the world
//    and the HUD together.
//
//    That togetherness is the point. This camera did once scale its own zoom
//    by viewportHeight/1080, and it shrank every world-unit metric in the
//    renderer to 0.667x at 720p while the HUD, which knew nothing about it,
//    stayed put. A camera cannot fix a window size on its own; only something
//    above both layers can.
//
//  * The camera is pinned to the flower, never eased toward it. src/game.ts's
//    updateCamera writes the camera straight from the flower's *drawn*
//    position each frame, so the flower sits exactly on the screen centre.
//    That is load-bearing: the mouse control law reads the cursor's offset
//    from the screen centre and treats it as the offset from the flower, so a
//    lagging camera would skew steering, not just drift the flower off centre.

#include "shared/core/types.h"

namespace flix {

class Camera {
public:
    /// Centres the camera on `target`. The only way the camera moves: it is
    /// called every frame with the flower's drawn position, and again on
    /// respawn and teleport.
    void snapTo(Vec2 target) { centre_ = target; }

    /// The visible extent in DESIGN units -- Window::width()/height(), never
    /// a pixel count. Feeding it pixels is what makes a HiDPI display show
    /// twice as much world as everyone else.
    void setViewport(int width, int height) {
        viewportWidth_ = width;
        viewportHeight_ = height;
    }

    /// World units -> design units: the player's setting times what the worn
    /// loadout asks for, the same product as Graphics.zoomLevel. Independent
    /// of the window size and of the display's pixel density, both of which
    /// are absorbed by the frame's base transform long before anything asks
    /// the camera anything.
    double zoom() const { return userZoom * loadoutZoom; }

    Vec2 worldToScreen(Vec2 world) const {
        const double z = zoom();
        return {
            (world.x - centre_.x) * z + viewportWidth_ * 0.5,
            (world.y - centre_.y) * z + viewportHeight_ * 0.5,
        };
    }

    Vec2 screenToWorld(Vec2 screen) const {
        const double z = zoom();
        return {
            (screen.x - viewportWidth_ * 0.5) / z + centre_.x,
            (screen.y - viewportHeight_ * 0.5) / z + centre_.y,
        };
    }

    /// The world rectangle currently on screen, grown by `margin` world units.
    /// Used to skip drawing anything outside it.
    Rect visibleWorld(double margin = 0) const {
        const double z = zoom();
        const double halfW = viewportWidth_ * 0.5 / z + margin;
        const double halfH = viewportHeight_ * 0.5 / z + margin;
        return {centre_.x - halfW, centre_.y - halfH, halfW * 2, halfH * 2};
    }

    Vec2 centre() const { return centre_; }
    int viewportWidth() const { return viewportWidth_; }
    int viewportHeight() const { return viewportHeight_; }

    /// Player-controlled zoom: the settings value the wheel and the zoom keys
    /// move, floored at 1 -- see kMinZoom in client/ui/menus.h.
    double userZoom = 1.0;
    /// What the worn loadout asks for: the smallest cameraZoom on the active
    /// bar, or 1 -- see loadoutCameraZoom. Antennae and observer are the
    /// petals that set one, and they are the only way to see more of the
    /// world than the default view, which is what makes them worth wearing.
    /// The client's alone: the server neither computes it nor sends it.
    double loadoutZoom = 1.0;

private:
    Vec2 centre_;
    int viewportWidth_ = 1280;
    int viewportHeight_ = 720;
};

} // namespace flix
