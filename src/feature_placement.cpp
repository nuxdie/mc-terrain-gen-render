#include "feature_placement.hpp"

#include <limits>
#include <set>
#include <stdexcept>

namespace mcworld::detail {

bool PlacedFeature::place(FeatureContext& context, BlockPosition origin) const {
    bool placed = false;
    const auto visit = [&](auto&& self, std::size_t depth, BlockPosition pos) -> void {
        if (depth == modifiers.size()) {
            // Never short-circuit: every terminal position consumes randomness.
            placed = feature(context, pos) || placed;
        } else {
            modifiers[depth](context, pos, [&](BlockPosition next) { self(self, depth + 1, next); });
        }
    };
    visit(visit, 0, origin);
    return placed;
}

namespace placement {
IntProvider constant(int value) { return [value](WorldgenRandom&) { return value; }; }
IntProvider uniform(int minimum, int maximum) {
    if (maximum < minimum || static_cast<std::int64_t>(maximum) - minimum >= std::numeric_limits<int>::max())
        throw std::invalid_argument("Invalid uniform integer range");
    return [=](WorldgenRandom& random) { return minimum + random.nextInt(maximum - minimum + 1); };
}
IntProvider triangle(int minimum, int maximum) {
    if (maximum < minimum || static_cast<std::int64_t>(maximum) - minimum >= std::numeric_limits<int>::max())
        throw std::invalid_argument("Invalid triangular integer range");
    return [=](WorldgenRandom& random) {
        if (minimum == maximum) return minimum + random.nextInt(1);
        const int lower = (maximum - minimum) / 2;
        const int upper = maximum - minimum - lower;
        const int first = random.nextInt(upper + 1);
        return minimum + first + random.nextInt(lower + 1);
    };
}
IntProvider countExtra(int base, float chance, int extra) {
    return [=](WorldgenRandom& random) { return base + (random.nextFloat() < chance ? extra : 0); };
}
IntProvider veryBiasedToBottom(int minimum, int maximum, int inner) {
    if (inner < 1 || maximum < minimum || static_cast<std::int64_t>(maximum) - minimum >= std::numeric_limits<int>::max())
        throw std::invalid_argument("Invalid biased height range");
    return [=](WorldgenRandom& random) {
        if (maximum - minimum - inner + 1 <= 0) return minimum;
        const auto next = [&](int low, int high) { return low >= high ? low : low + random.nextInt(high - low + 1); };
        const int upper = next(minimum + inner, maximum);
        const int biasedUpper = next(minimum, upper - 1);
        return next(minimum, biasedUpper - 1 + inner);
    };
}
PlacementModifier count(IntProvider provider) {
    return [provider = std::move(provider)](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        const int count = provider(c.random);
        for (int i = 0; i < count; ++i) next(p);
    };
}
PlacementModifier rarity(int chance) {
    if (chance <= 0) throw std::invalid_argument("Feature rarity must be positive");
    return [=](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        if (c.random.nextFloat() < 1.0F / chance) next(p);
    };
}
PlacementModifier square() {
    return [](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        p.x += c.random.nextInt(16);
        p.z += c.random.nextInt(16);
        next(p);
    };
}
PlacementModifier heightRange(IntProvider provider) {
    return [provider = std::move(provider)](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        p.y = provider(c.random);
        next(p);
    };
}
PlacementModifier heightmap(FeatureHeightmap type) {
    return [=](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        p.y = c.world.height(type, p.x, p.z);
        if (p.y > TerrainChunk::minY) next(p);
    };
}
PlacementModifier biome() {
    return [](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        if (!c.biomeFilter) throw std::logic_error("Biome filter requires a top-level feature");
        if (c.biomeFilter(c.world.biomeAt(p.x, p.y, p.z))) next(p);
    };
}
PlacementModifier filter(PositionPredicate predicate) {
    return [predicate = std::move(predicate)](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        if (predicate(c.world, p)) next(p);
    };
}
PlacementModifier offset(IntProvider horizontal, IntProvider vertical) {
    return [horizontal = std::move(horizontal), vertical = std::move(vertical)]
        (FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        p.x += horizontal(c.random);
        p.y += vertical(c.random);
        p.z += horizontal(c.random);
        next(p);
    };
}
PlacementModifier scan(int directionY, int maxSteps, PositionPredicate target, PositionPredicate allowed) {
    if ((directionY != -1 && directionY != 1) || maxSteps < 1 || maxSteps > 32)
        throw std::invalid_argument("Invalid environment scan");
    return [=](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        if (!allowed(c.world, p)) return;
        for (int step = 0; step < maxSteps; ++step) {
            if (target(c.world, p)) { next(p); return; }
            p.y += directionY;
            if (p.y < TerrainChunk::minY || p.y >= TerrainChunk::maxY) return;
            if (!allowed(c.world, p)) break;
        }
        if (target(c.world, p)) next(p);
    };
}
} // namespace placement

FeatureOrder sortFeatures(const std::vector<BiomeFeatureList>& sources) {
    // Java orders nodes by step, then first-encounter identity ordinal. Reverse
    // DFS postorder determines both stable output order and each feature seed.
    using Node = std::pair<int, std::size_t>;
    std::map<FeatureId, std::size_t> identities;
    std::vector<FeatureId> ids;
    std::map<Node, std::set<Node>> edges;
    for (const auto& source : sources) {
        std::vector<Node> sequence;
        for (int step = 0; step < kDecorationStepCount; ++step) {
            for (FeatureId id : source[step]) {
                const auto [it, inserted] = identities.emplace(id, ids.size());
                if (inserted) ids.push_back(id);
                sequence.emplace_back(step, it->second);
            }
        }
        for (std::size_t i = 0; i < sequence.size(); ++i) {
            auto& next = edges[sequence[i]];
            if (i + 1 < sequence.size()) next.insert(sequence[i + 1]);
        }
    }
    std::map<Node, int> state;
    std::vector<Node> sorted;
    const auto visit = [&](auto&& self, Node node) -> void {
        if (state[node] == 2) return;
        if (state[node] == 1) throw std::invalid_argument("Feature order cycle at identity " + std::to_string(ids[node.second]));
        state[node] = 1;
        for (Node next : edges.at(node)) self(self, next);
        state[node] = 2;
        sorted.push_back(node);
    };
    for (const auto& [node, next] : edges) visit(visit, node);
    FeatureOrder result;
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) result[it->first].push_back(ids[it->second]);
    return result;
}

ConfiguredFeature sequenceFeature(std::vector<PlacedFeature> children) {
    return [children = std::move(children)](FeatureContext& c, BlockPosition p) {
        bool placed = false;
        for (const auto& child : children) placed = child.place(c, p) || placed;
        return placed;
    };
}
ConfiguredFeature randomSelectorFeature(std::vector<std::pair<float, PlacedFeature>> children, PlacedFeature fallback) {
    return [children = std::move(children), fallback = std::move(fallback)](FeatureContext& c, BlockPosition p) {
        for (const auto& [chance, child] : children) if (c.random.nextFloat() < chance) return child.place(c, p);
        return fallback.place(c, p);
    };
}

} // namespace mcworld::detail
