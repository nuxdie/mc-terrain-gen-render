// Implementation of the placement vocabulary declared in
// feature_placement.hpp. Each modifier here is one of Java's, and the comment
// on it says what it draws: that, not what it computes, is the part a refactor
// can break without any test noticing until a world looks different.

#include "feature_placement.hpp"

#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mcworld::detail {

// Java: `PlacedFeature.placeWithContext`. The chain is walked depth-first: a
// modifier that fans out (`count`) runs the whole remaining chain for its first
// position before starting its second, so positions interleave with draws
// exactly as Java's stream pipeline does.
bool PlacedFeature::place(FeatureContext& context, BlockPosition origin) const {
    bool placed = false;
    // Self-passing because a lambda cannot name itself; `depth` is the index of
    // the modifier to apply next, and `modifiers.size()` is the terminal.
    const auto visit = [&](auto&& self, std::size_t depth, BlockPosition position) -> void {
        if (depth == modifiers.size()) {
            // The result is accumulated, never short-circuited: every terminal
            // position has to run, because every one of them draws.
            placed = feature(context, position) || placed;
            return;
        }
        modifiers[depth](context, position, [&](BlockPosition next) { self(self, depth + 1, next); });
    };
    visit(visit, 0, origin);
    return placed;
}

namespace placement {

// --- Int providers ---------------------------------------------------------
//
// Java's `IntProvider` types. The ranges are validated at construction rather
// than per draw, because a bad range is a catalog bug, not a runtime condition.
// `constant` is the only one that draws nothing.

IntProvider constant(int value) { return [value](WorldgenRandom&) { return value; }; }

// One draw, flat over the inclusive range.
IntProvider uniform(int minimum, int maximum) {
    if (maximum < minimum || static_cast<std::int64_t>(maximum) - minimum >= std::numeric_limits<int>::max())
        throw std::invalid_argument("Invalid uniform integer range");
    return [=](WorldgenRandom& random) { return minimum + random.nextInt(maximum - minimum + 1); };
}
// Two draws, summed, so the distribution peaks in the middle of the range.
// The degenerate range still draws once, which is why it is not short-circuited.
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
// A fixed count with a chance of a few more, e.g. "10 trees, sometimes 11".
IntProvider countExtra(int base, float chance, int extra) {
    return [=](WorldgenRandom& random) { return base + (random.nextFloat() < chance ? extra : 0); };
}
// Java's `VeryBiasedToBottomHeight`, used by lava springs: three nested draws
// that pile results towards `minimum`. Each `next` call draws only when its
// range is non-degenerate, so the count varies with the range.
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
// --- Modifiers -------------------------------------------------------------

// Fans the position out `provider` times. The draw happens once, up front.
PlacementModifier count(IntProvider provider) {
    return [provider = std::move(provider)](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        const int count = provider(c.random);
        for (int i = 0; i < count; ++i) next(p);
    };
}
// Passes the position on with probability 1/`chance`. Always draws.
PlacementModifier rarity(int chance) {
    if (chance <= 0) throw std::invalid_argument("Feature rarity must be positive");
    return [=](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        if (c.random.nextFloat() < 1.0F / chance) next(p);
    };
}
// A random column within the chunk the position starts at. Two draws, X then
// Z; the origin is a chunk corner, so this never leaves that chunk.
PlacementModifier square() {
    return [](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        p.x += c.random.nextInt(16);
        p.z += c.random.nextInt(16);
        next(p);
    };
}
// Replaces Y from a distribution. Draws whatever the provider draws.
PlacementModifier heightRange(IntProvider provider) {
    return [provider = std::move(provider)](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        p.y = provider(c.random);
        next(p);
    };
}
// Snaps Y to a heightmap. Draws nothing, and drops the position when the
// column is empty, which is what `minY` signals.
PlacementModifier heightmap(FeatureHeightmap type) {
    return [=](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        p.y = c.world.height(type, p.x, p.z);
        if (p.y > TerrainChunk::minY) next(p);
    };
}
// Re-checks the top-level feature's biome predicate at this exact position,
// because a feature is selected from the whole 3x3 neighbourhood but must only
// generate in the biomes that registered it. Draws nothing.
PlacementModifier biome() {
    return [](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        if (!c.biomeFilter) throw std::logic_error("Biome filter requires a top-level feature");
        if (c.biomeFilter(c.world.biomeAt(p.x, p.y, p.z))) next(p);
    };
}
// An arbitrary world test, e.g. "is this column under water". Draws nothing.
PlacementModifier filter(PositionPredicate predicate) {
    return [predicate = std::move(predicate)](FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        if (predicate(c.world, p)) next(p);
    };
}
// Displaces the position. The horizontal provider is drawn twice - once for X
// and again for Z - so the two offsets are independent.
PlacementModifier offset(IntProvider horizontal, IntProvider vertical) {
    return [horizontal = std::move(horizontal), vertical = std::move(vertical)]
        (FeatureContext& c, BlockPosition p, const PositionConsumer& next) {
        p.x += horizontal(c.random);
        p.y += vertical(c.random);
        p.z += horizontal(c.random);
        next(p);
    };
}
// Java's `EnvironmentScanPlacement`: walk up or down while `allowed` holds,
// stopping at the first position where `target` does. The build limits end the
// scan silently; leaving the `allowed` region still gives `target` one last
// chance at the boundary. Draws nothing.
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

// Java: `FeatureSorter.buildFeaturesPerStep`.
//
// Each biome contributes one sequence of (step, feature) nodes in its own
// registration order, and every adjacent pair in that sequence is an edge "this
// one comes first". The global order is a reverse DFS postorder over the union
// of those edges, which is the order Java's indices - and therefore the feature
// seeds - are assigned from. Biomes that disagree about the relative order of
// two features form a cycle, which is an error rather than something to break
// arbitrarily.
FeatureOrder sortFeatures(const std::vector<BiomeFeatureList>& sources) {
    // A feature is a node once per step it appears in. `identity` is a dense
    // ordinal assigned on first encounter, so node ordering - and hence the
    // iteration order of the maps below - follows first registration rather
    // than the numeric value of a `FeatureId`.
    struct Node {
        int step{};
        std::size_t identity{};
        auto operator<=>(const Node&) const = default;
    };

    std::map<FeatureId, std::size_t> identities;
    std::vector<FeatureId> ids;
    std::map<Node, std::set<Node>> successors;
    for (const BiomeFeatureList& source : sources) {
        std::vector<Node> sequence;
        for (int step = 0; step < kDecorationStepCount; ++step) {
            for (FeatureId id : source[step]) {
                const auto [entry, inserted] = identities.emplace(id, ids.size());
                if (inserted) ids.push_back(id);
                sequence.push_back({step, entry->second});
            }
        }
        for (std::size_t i = 0; i < sequence.size(); ++i) {
            // Every node is entered, including the last: a node with no
            // successors still has to be visited by the traversal below, which
            // iterates the edge map rather than the feature list.
            std::set<Node>& next = successors[sequence[i]];
            if (i + 1 < sequence.size()) next.insert(sequence[i + 1]);
        }
    }

    enum class Mark { Unvisited, OnStack, Done };
    std::map<Node, Mark> marks;
    std::vector<Node> postorder;
    const auto visit = [&](auto&& self, Node node) -> void {
        if (marks[node] == Mark::Done) return;
        if (marks[node] == Mark::OnStack) {
            throw std::invalid_argument("Feature order cycle at identity " + std::to_string(ids[node.identity]));
        }
        marks[node] = Mark::OnStack;
        for (Node next : successors.at(node)) self(self, next);
        marks[node] = Mark::Done;
        postorder.push_back(node);
    };
    for (const auto& [node, next] : successors) visit(visit, node);

    FeatureOrder result;
    for (auto it = postorder.rbegin(); it != postorder.rend(); ++it) {
        result[it->step].push_back(ids[it->identity]);
    }
    return result;
}

// --- Composite features ----------------------------------------------------

// Java: `RandomFeatureConfiguration` with no chances - every child runs, in
// order, on the parent's stream and with the parent's biome context.
ConfiguredFeature sequenceFeature(std::vector<PlacedFeature> children) {
    return [children = std::move(children)](FeatureContext& c, BlockPosition p) {
        bool placed = false;
        for (const auto& child : children) placed = child.place(c, p) || placed;
        return placed;
    };
}
// Java: `RandomFeature`. Each child is offered its own chance in turn, and the
// first that takes it runs alone; the draws stop there, so a later child is
// cheaper to reach than its listed chance suggests.
ConfiguredFeature randomSelectorFeature(std::vector<std::pair<float, PlacedFeature>> children, PlacedFeature fallback) {
    return [children = std::move(children), fallback = std::move(fallback)](FeatureContext& c, BlockPosition p) {
        for (const auto& [chance, child] : children) if (c.random.nextFloat() < chance) return child.place(c, p);
        return fallback.place(c, p);
    };
}

} // namespace mcworld::detail
