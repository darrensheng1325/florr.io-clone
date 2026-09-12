// Times the mob layer for one mob type, so "this mob is slow to draw" can be
// measured rather than argued about.
//
// It builds a scene of N mobs of one type around a stationary viewer, renders
// it through the real WorldRenderer, and reports what drawing the mob layer
// cost. The figure to compare is ms/mob: a rock is about 0.03, a hornet 0.14,
// and anything an order of magnitude past those is drawing something it should
// not be drawing every frame.
//
//   mob_render_bench [mob id] [count] [frames]
//
// ONEBIG=<factor> makes the first mob that much larger than the rest. Effects
// that share one scratch buffer between entities are sized by the biggest
// thing on screen, and this is how you see whether one oversized body is
// taxing everything beside it.
//
// DUMP=<path> writes the last frame as a PPM, for diffing a render change
// against the same scene rendered before it.
#include "canvas.h"
#include "client/camera.h"
#include "client/render/sprites.h"
#include "client/render/world_renderer.h"
#include "client/world_view.h"
#include "shared/game/config.h"
#include "shared/net/protocol.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace flix;

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "glitch_flower";
    const int count = argc > 2 ? std::atoi(argv[2]) : 8;
    const int frames = argc > 3 ? std::atoi(argv[3]) : 150;

    ContentRegistry content;
    std::string error;
    if (!content.loadFiles("data/mobs.json", "data/petals.json", "data/mob_xp.json", error)) {
        std::printf("content: %s\n", error.c_str());
        return 1;
    }
    SpriteCache sprites;
    sprites.build(content, "data");

    const std::uint16_t index = content.mobIndex(which);
    if (index == kInvalidIndex) {
        std::printf("no such mob: %s\n", which.c_str());
        return 1;
    }
    const MobConfig& config = content.mob(index);

    constexpr int kW = 1280, kH = 720;
    Canvas canvas = Canvas::createVirtual(kW, kH);
    Camera camera;
    camera.setViewport(kW, kH);
    camera.userZoom = 1.0;
    const Vec2 centre{5000.0, 5000.0};
    camera.snapTo(centre);

    WorldRenderer renderer;
    renderer.setContent(&content);
    renderer.setSprites(&sprites);
    // Names and bars are a text cost, not a mob-art cost, and they would sit
    // in the same timing section.
    renderer.options.names = false;
    renderer.options.healthBars = false;

    WorldView view;
    view.setRealm(Realm::Overworld);
    RemoteEntity self;
    self.netId = 1;
    self.kind = net::EntityKind::Player;
    self.position = self.targetPosition = centre;
    self.needsSnap = false;
    view.seedForTest(self);

    const char* oneBig = std::getenv("ONEBIG");
    for (int i = 0; i < count; ++i) {
        RemoteEntity mob;
        mob.netId = static_cast<std::uint32_t>(100 + i);
        mob.kind = net::EntityKind::Mob;
        mob.typeIndex = index;
        mob.rarity = Rarity::Common;
        mob.healthFraction = 1.0;
        mob.radius = config.size * 25.0;
        if (i == 0 && oneBig != nullptr) mob.radius *= std::atof(oneBig);
        mob.position = mob.targetPosition =
            Vec2{centre.x - 400.0 + (i % 4) * 250.0, centre.y - 200.0 + (i / 4) * 220.0};
        mob.needsSnap = false;
        view.seedForTest(mob);
    }

    // Warm-up, so first-touch page faults and lazily compiled art are not in
    // the sample.
    for (int i = 0; i < 10; ++i) renderer.draw(canvas, view, camera, centre, i * 0.016);

    double mobsMillis = 0, wallMillis = 0, peak = 0;
    for (int f = 0; f < frames; ++f) {
        const auto start = std::chrono::steady_clock::now();
        renderer.draw(canvas, view, camera, centre, 100.0 + f * 0.016);
        const double wall =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        wallMillis += wall;
        if (wall > peak) peak = wall;
        mobsMillis += renderer.sectionTiming().mobsMillis;
    }

    if (const char* dump = std::getenv("DUMP")) {
        const std::vector<std::uint8_t> pixels = canvas.getImageData(0, 0, kW, kH);
        if (FILE* file = std::fopen(dump, "wb")) {
            std::fprintf(file, "P6\n%d %d\n255\n", kW, kH);
            for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) std::fwrite(&pixels[i], 1, 3, file);
            std::fclose(file);
        }
    }

    std::printf("%-16s x%-3d  mobs %7.3f ms/frame  whole %7.3f ms  peak %7.3f ms  (%.3f ms/mob)\n",
                which.c_str(), count, mobsMillis / frames, wallMillis / frames, peak,
                mobsMillis / frames / (count > 0 ? count : 1));
    return 0;
}
