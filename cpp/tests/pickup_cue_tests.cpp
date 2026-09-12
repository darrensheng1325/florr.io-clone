#include "test.h"

#include "client/camera.h"
#include "client/render/world_renderer.h"
#include "client/world_view.h"
#include "shared/net/protocol.h"

#include <vector>

using namespace flix;

// The client half of the pickup cue.
//
// A drop can be collected on the tick it spawned -- magnetism is a pickup
// RADIUS, and an apex observer's is 437 units, further out than the petals
// that did the killing -- so no snapshot ever carries the entity and the
// client has never heard of that net id. The cue is then the only thing there
// is to draw from, which is why it carries the drop's position and look.
// Ignoring a cue for an id the entity table never held is what makes a
// well-equipped flower look like mobs stopped dropping loot.

namespace {

constexpr int kFrameSize = 320;
constexpr Vec2 kDropAt{1000.0, 1000.0};
constexpr std::uint32_t kDropNetId = 4242;
constexpr std::uint32_t kTakerNetId = 7;

Camera frameCamera() {
    Camera camera;
    camera.setViewport(kFrameSize, kFrameSize);
    camera.userZoom = 1.0;
    camera.snapTo(kDropAt);
    return camera;
}

ViewEvent pickupCue(std::uint16_t petalIndex, Rarity rarity) {
    ViewEvent cue;
    cue.kind = net::EventKind::PickedUp;
    cue.netId = kDropNetId;
    cue.otherNetId = kTakerNetId;
    cue.position = kDropAt;
    // The look, in the two fixed fields this kind has no other use for.
    cue.amount = petalIndex;
    cue.flag = static_cast<std::uint8_t>(rarityIndex(rarity));
    return cue;
}

/// The taker, so the item has somewhere to fly to.
RemoteEntity flowerAt(Vec2 position) {
    RemoteEntity flower;
    flower.netId = kTakerNetId;
    flower.kind = net::EntityKind::Player;
    flower.position = position;
    flower.targetPosition = position;
    flower.needsSnap = false;
    return flower;
}

/// How many drops the renderer drew in one frame `age` seconds after the cue.
int itemsDrawnAfterCue(const ViewEvent& cue, double age) {
    Canvas canvas = Canvas::createVirtual(kFrameSize, kFrameSize);
    const Camera camera = frameCamera();

    WorldView view;
    view.setRealm(Realm::Overworld);
    view.seedForTest(flowerAt(kDropAt + Vec2{60.0, 0.0}));

    WorldRenderer renderer;
    view.events().push_back(cue);
    renderer.ingestEvents(view);
    renderer.update(age);
    renderer.draw(canvas, view, camera, kDropAt, 0.0);
    return renderer.sectionTiming().itemCount;
}

} // namespace

// ---------------------------------------------------------------------------

TEST(a_pickup_cue_for_a_drop_the_client_never_saw_still_draws_the_item) {
    // Nothing was on screen and nothing was in the entity table: the whole
    // item comes out of the cue.
    CHECK_EQ(itemsDrawnAfterCue(pickupCue(3, Rarity::Legendary), 0.0), 1);

    // And it is a FLIGHT, not a permanent fixture: past the pickup animation
    // the drop is gone.
    CHECK_EQ(itemsDrawnAfterCue(pickupCue(3, Rarity::Legendary), 1.0), 0);
}

TEST(a_pickup_cue_for_a_drop_the_client_held_animates_it_once) {
    Canvas canvas = Canvas::createVirtual(kFrameSize, kFrameSize);
    const Camera camera = frameCamera();

    WorldView view;
    view.setRealm(Realm::Overworld);
    view.seedForTest(flowerAt(kDropAt + Vec2{60.0, 0.0}));

    RemoteEntity item;
    item.netId = kDropNetId;
    item.kind = net::EntityKind::Drop;
    item.typeIndex = 3;
    item.rarity = Rarity::Rare;
    item.position = kDropAt;
    item.targetPosition = kDropAt;
    item.needsSnap = false;
    view.seedForTest(item);

    WorldRenderer renderer;
    // One frame with the drop lying there, which is what puts it in the
    // renderer's own table.
    renderer.ingestEvents(view);
    renderer.update(0.0);
    renderer.draw(canvas, view, camera, kDropAt, 0.0);
    CHECK_EQ(renderer.sectionTiming().itemCount, 1);

    // Now it is taken: the snapshot drops the entity in the same breath the
    // cue arrives, and the flight is played from the record the renderer kept.
    // ONE item, not two -- a cue that both materialised an item and left the
    // held record behind would draw the drop twice over.
    view.clear();
    view.setRealm(Realm::Overworld);
    view.seedForTest(flowerAt(kDropAt + Vec2{60.0, 0.0}));
    view.events().push_back(pickupCue(3, Rarity::Rare));
    renderer.ingestEvents(view);
    renderer.update(0.0);
    renderer.draw(canvas, view, camera, kDropAt, 0.0);
    CHECK_EQ(renderer.sectionTiming().itemCount, 1);
}
