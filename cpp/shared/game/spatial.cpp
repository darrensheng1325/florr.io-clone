#include "shared/game/spatial.h"

#include <algorithm>
#include <cmath>

#include "shared/game/terrain.h"

namespace flix {

SpatialGrid::SpatialGrid(double cellSize) {
    cellSize_ = (std::isfinite(cellSize) && cellSize > 1.0) ? cellSize : kDefaultCellSize;
    invCellSize_ = 1.0 / cellSize_;

    for (int i = 0; i < kRealmCount; ++i) {
        Layer& layer = layers_[static_cast<std::size_t>(i)];
        const double span = Terrain::realmSize(static_cast<Realm>(i));
        const double side = std::isfinite(span) && span > 0 ? span : kWorldSize;
        layer.cols = clamp(static_cast<int>(std::ceil(std::min(side, 1e9) * invCellSize_)), 1,
                           kMaxAxisCells);
        layer.rows = layer.cols;
        const std::size_t cells =
            static_cast<std::size_t>(layer.cols) * static_cast<std::size_t>(layer.rows);
        layer.buckets.resize(cells);
        layer.bucketEpoch.assign(cells, 0);
    }
}

int SpatialGrid::cellIndex(double offset, double invCellSize, int axisCells) {
    const double c = std::floor(offset * invCellSize);
    // Written as failed comparisons so NaN lands in cell 0 instead of taking
    // an undefined trip through the cast.
    if (!(c > 0.0)) return 0;
    if (!(c < static_cast<double>(axisCells))) return axisCells - 1;
    return static_cast<int>(c);
}

void SpatialGrid::clear() {
    inserted_ = 0;
    if (++epoch_ == 0) {
        // Once every four billion ticks the epoch wraps onto the value stale
        // buckets already carry, so retire them all for real. Never hit in a
        // session; cheap enough that it does not need to be.
        for (Layer& layer : layers_) {
            std::fill(layer.bucketEpoch.begin(), layer.bucketEpoch.end(), 0);
        }
        epoch_ = 1;
    }
}

void SpatialGrid::insert(Entity e, Realm realm, Vec2 position, double radius) {
    if (e == NULL_ENTITY) return;
    // A NaN position cannot be found by any query, so filing it would only
    // give a later reader a corrupt candidate to trip over.
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) return;
    if (!std::isfinite(radius) || radius < 0.0) radius = 0.0;

    const std::uint32_t idx = entityIndex(e);
    if (idx >= stamp_.size()) {
        // Grown here rather than in query(), so the read path never allocates.
        stamp_.resize(std::max<std::size_t>(static_cast<std::size_t>(idx) + 1, stamp_.size() * 2), 0);
    }

    Layer& target = layer(realm);
    const int x0 = cellX(realm, position.x - radius);
    const int x1 = cellX(realm, position.x + radius);
    const int y0 = cellY(realm, position.y - radius);
    const int y1 = cellY(realm, position.y + radius);
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const std::size_t b = target.bucketAt(cx, cy);
            if (target.bucketEpoch[b] != epoch_) {
                target.buckets[b].clear();
                target.bucketEpoch[b] = epoch_;
            }
            target.buckets[b].push_back(e);
        }
    }
    ++inserted_;
}

void SpatialGrid::query(Realm realm, Vec2 center, double radius, std::vector<Entity>& out) const {
    if (!std::isfinite(radius) || radius < 0.0) radius = 0.0;
    queryRect(realm, Vec2{center.x - radius, center.y - radius},
              Vec2{center.x + radius, center.y + radius}, out);
}

void SpatialGrid::queryRect(Realm realm, const Rect& area, std::vector<Entity>& out) const {
    queryRect(realm, Vec2{area.left(), area.top()}, Vec2{area.right(), area.bottom()}, out);
}

void SpatialGrid::queryRect(Realm realm, Vec2 min, Vec2 max, std::vector<Entity>& out) const {
    out.clear();
    if (!std::isfinite(min.x) || !std::isfinite(min.y) || !std::isfinite(max.x) || !std::isfinite(max.y)) {
        return;
    }
    if (min.x > max.x) std::swap(min.x, max.x);
    if (min.y > max.y) std::swap(min.y, max.y);

    if (++queryEpoch_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0);
        queryEpoch_ = 1;
    }

    const Layer& source = layer(realm);
    const int x0 = cellX(realm, min.x);
    const int x1 = cellX(realm, max.x);
    const int y0 = cellY(realm, min.y);
    const int y1 = cellY(realm, max.y);
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const std::size_t b = source.bucketAt(cx, cy);
            if (source.bucketEpoch[b] != epoch_) continue;   // last tick's contents
            for (const Entity e : source.buckets[b]) {
                const std::uint32_t idx = entityIndex(e);
                if (idx < stamp_.size()) {
                    if (stamp_[idx] == queryEpoch_) continue;   // another of its cells
                    stamp_[idx] = queryEpoch_;
                }
                out.push_back(e);
            }
        }
    }
}

std::size_t SpatialGrid::reservedEntries() const {
    std::size_t total = 0;
    for (const Layer& layer : layers_) {
        for (const std::vector<Entity>& bucket : layer.buckets) total += bucket.capacity();
    }
    return total;
}

} // namespace flix
