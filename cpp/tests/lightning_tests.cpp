#include "test.h"

#include "client/camera.h"
#include "client/render/world_renderer.h"
#include "client/world_view.h"
#include "shared/net/protocol.h"

#include <cmath>
#include <vector>

using namespace flix;

namespace {

// The strike's damage is a field the server resolves a tick later; the only
// thing that ever reaches a screen is the Lightning event and the arms the
// renderer builds from it. These render a frame and read the pixels back,
// because "the lightning is invisible" is a claim about pixels and nothing
// short of pixels can refute it.

constexpr int kFrameSize = 400;
/// Where the strike lands. Any point does; a round one keeps the arithmetic in
/// the assertions readable.
constexpr Vec2 kStrikeAt{1000.0, 1000.0};

/// White enough to be a bolt. The ground under it is a flat biome colour and
/// nothing else on an empty frame comes near this, so the threshold only has
/// to separate "painted" from "not painted".
bool isBoltPixel(const std::vector<std::uint8_t>& rgba, std::size_t index) {
    return rgba[index] > 200 && rgba[index + 1] > 200 && rgba[index + 2] > 200;
}

std::size_t boltPixels(const std::vector<std::uint8_t>& rgba) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (isBoltPixel(rgba, i)) ++count;
    }
    return count;
}

/// True when some part of a bolt was painted within `reach` pixels of `screen`.
bool paintedNear(const std::vector<std::uint8_t>& rgba, Vec2 screen, double reach) {
    const int minX = std::max(0, static_cast<int>(screen.x - reach));
    const int maxX = std::min(kFrameSize - 1, static_cast<int>(screen.x + reach));
    const int minY = std::max(0, static_cast<int>(screen.y - reach));
    const int maxY = std::min(kFrameSize - 1, static_cast<int>(screen.y + reach));
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * kFrameSize + x) * 4;
            if (isBoltPixel(rgba, index)) return true;
        }
    }
    return false;
}

Camera frameCamera() {
    Camera camera;
    camera.setViewport(kFrameSize, kFrameSize);
    camera.userZoom = 1.0;
    camera.snapTo(kStrikeAt);
    return camera;
}

/// One frame of a strike on `targets`, `age` seconds after it landed. No
/// content and no sprites: the ground falls back to its flat biome colour,
/// which is all a white bolt has to stand out against.
std::vector<std::uint8_t> renderStrike(const std::vector<Vec2>& targets, double age) {
    Canvas canvas = Canvas::createVirtual(kFrameSize, kFrameSize);
    const Camera camera = frameCamera();

    WorldView view;
    view.setRealm(Realm::Overworld);
    WorldRenderer renderer;

    if (!targets.empty()) {
        ViewEvent strike;
        strike.kind = net::EventKind::Lightning;
        strike.position = kStrikeAt;
        strike.radius = 1000.0;
        strike.points = targets;
        view.events().push_back(strike);
        renderer.ingestEvents(view);
    }
    renderer.update(age);
    renderer.draw(canvas, view, camera, kStrikeAt, 0.0);
    return canvas.getImageData(0, 0, kFrameSize, kFrameSize);
}

} // namespace

// ---------------------------------------------------------------------------

TEST(a_strike_paints_an_arm_to_every_mob_it_names) {
    const std::vector<Vec2> targets = {
        {kStrikeAt.x + 120.0, kStrikeAt.y},
        {kStrikeAt.x, kStrikeAt.y + 130.0},
        {kStrikeAt.x - 150.0, kStrikeAt.y + 40.0},
    };

    // Nothing white on the bare ground, or the assertions below would be
    // measuring the biome.
    CHECK_EQ(boltPixels(renderStrike({}, 0.0)), std::size_t(0));

    const std::vector<std::uint8_t> frame = renderStrike(targets, 0.0);
    CHECK(boltPixels(frame) > 100);

    // Anchored at BOTH ends of every arm: the jitter displaces the runs
    // between the endpoints and must never move the endpoints themselves, or a
    // strike would draw bolts that start beside the flower and stop beside the
    // mob.
    const Camera camera = frameCamera();
    CHECK(paintedNear(frame, camera.worldToScreen(kStrikeAt), 3.0));
    for (const Vec2& target : targets) {
        CHECK(paintedNear(frame, camera.worldToScreen(target), 3.0));
    }
}

TEST(an_arm_wanders_off_the_straight_line_between_its_ends) {
    // A bolt drawn as a plain segment is a laser, not lightning. Nothing here
    // asserts a particular shape -- the jitter is random by design -- only that
    // some of the arm lies clear of the straight run between its endpoints.
    const Vec2 target{kStrikeAt.x + 180.0, kStrikeAt.y};
    const std::vector<std::uint8_t> frame = renderStrike({target}, 0.0);

    const Camera camera = frameCamera();
    const Vec2 from = camera.worldToScreen(kStrikeAt);
    const Vec2 to = camera.worldToScreen(target);
    const Vec2 along = (to - from).normalized();

    double furthest = 0;
    for (int y = 0; y < kFrameSize; ++y) {
        for (int x = 0; x < kFrameSize; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * kFrameSize + x) * 4;
            if (!isBoltPixel(frame, index)) continue;
            const Vec2 offset{x - from.x, y - from.y};
            const double sideways = std::fabs(offset.x * along.y - offset.y * along.x);
            furthest = std::max(furthest, sideways);
        }
    }
    // Half a run's length is the most the reference displaces a midpoint by,
    // and a run is at least 50 units; anything above the stroke's own width is
    // proof the arm is not a segment.
    CHECK(furthest > 4.0);
}

TEST(a_strike_fades_out_instead_of_hanging_on_the_screen) {
    const std::vector<Vec2> targets = {{kStrikeAt.x + 120.0, kStrikeAt.y}};

    // Half its life in it is dimmer, and at the end of it there is nothing
    // left: a bolt that never expires is a permanent white scar across the
    // world, which is the failure mode of drawing one and forgetting it.
    const std::size_t fresh = boltPixels(renderStrike(targets, 0.0));
    const std::size_t half = boltPixels(renderStrike(targets, 0.25));
    const std::size_t gone = boltPixels(renderStrike(targets, 0.5));
    CHECK(fresh > 0);
    CHECK(half < fresh);
    CHECK_EQ(gone, std::size_t(0));
}
