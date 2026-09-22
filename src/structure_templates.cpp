// Template-driven structure assembly for stage 5 of
// `minecraft-26.3-worldgen.dot`: Java's jigsaw placement, the template
// processors that run over the blocks it produces, and the catalog validation
// that has to pass before either is allowed to run.
//
// Only the *shape* of a start is decided here. Where a start is attempted, and
// whether its biome allows it, is src/structures.cpp; writing its blocks into
// the world is stage 8, in src/decoration.cpp.
//
// Everything below consumes `random` in a fixed order, and that order is part
// of the generated world: a draw added, removed or moved past another one
// moves every structure the catalog can build. The order is called out at each
// site that is easy to get wrong.

#include "mcworld/structure_templates.hpp"
#include "generation_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcworld {

// --- Catalog validation ----------------------------------------------------

// Checked once, before the catalog is handed to a generator, so that assembly
// below can use `.at()` freely: every template, pool and start it dereferences
// is known to exist by the time it runs.
void StructureTemplateCatalog::validate() const {
    for (const auto& [id, t] : templates) {
        if (t.size.x < 1 || t.size.y < 1 || t.size.z < 1 || t.size.x > 128 || t.size.y > 384 || t.size.z > 128)
            throw std::invalid_argument("Invalid template size: " + id);
        const auto inside = [&](BlockPosition p) {
            return p.x >= 0 && p.x < t.size.x && p.y >= 0 && p.y < t.size.y && p.z >= 0 && p.z < t.size.z;
        };
        for (const auto& block : t.blocks) if (!inside(block.position)) throw std::invalid_argument("Template block outside bounds: " + id);
        for (const auto& joint : t.connectors) {
            if (!inside(joint.position)) throw std::invalid_argument("Jigsaw outside template: " + id);
            if (!joint.pool.empty() && !pools.contains(joint.pool)) throw std::invalid_argument("Unknown jigsaw pool: " + joint.pool);
        }
    }
    for (const auto& [id, pool] : pools) {
        if (!pool.fallback.empty() && !pools.contains(pool.fallback)) throw std::invalid_argument("Unknown fallback pool: " + id);
        int total = 0;
        for (const auto& element : pool.elements) {
            if (element.weight < 1 || element.weight > 150 || element.projection == PieceProjection::NonPool)
                throw std::invalid_argument("Invalid pool element: " + id);
            if (!element.templateId.empty() && !templates.contains(element.templateId)) throw std::invalid_argument("Unknown template: " + element.templateId);
            for (const auto& processor : element.processors)
                if (!(processor.probability >= 0 && processor.probability <= 1)) throw std::invalid_argument("Invalid template processor probability");
            // Bounded because a pool is expanded weight-by-weight into a
            // shuffled candidate list on every attachment attempt.
            total += element.weight;
            if (total > 100000) throw std::invalid_argument("Template pool is too large: " + id);
        }
    }
    for (const auto& [variant, start] : starts) {
        if (!pools.contains(start.pool) || pools.at(start.pool).elements.empty()) throw std::invalid_argument("Invalid start pool: " + start.pool);
        if (start.maxDepth < 0 || start.maxDepth > 20 || start.maxDistance < 1 || start.maxDistance > 128
            || start.startY < TerrainChunk::minY || start.startY >= TerrainChunk::maxY)
            throw std::invalid_argument("Invalid jigsaw start limits");
    }
}

namespace detail {
namespace {

// --- Template-local geometry -----------------------------------------------

// Rotations are quarter turns about the Y axis, counted clockwise, and are
// stored as the plain 0-3 that indexes them; Java's `Rotation` enum has the
// same ordering.
constexpr int kRotationCount = 4;

[[nodiscard]] BlockPosition add(BlockPosition a, BlockPosition b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] BlockPosition subtract(BlockPosition a, BlockPosition b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] BlockPosition negate(BlockPosition p) { return {-p.x, -p.y, -p.z}; }

[[nodiscard]] BlockPosition rotate(BlockPosition p, int rotation) {
    switch (rotation) {
    case 1: return {-p.z, p.y, p.x};
    case 2: return {-p.x, p.y, -p.z};
    case 3: return {p.z, p.y, -p.x};
    default: return p;
    }
}

[[nodiscard]] BlockPosition direction(TemplateDirection d) {
    switch (d) {
    case TemplateDirection::Down: return {0, -1, 0};
    case TemplateDirection::Up: return {0, 1, 0};
    case TemplateDirection::North: return {0, 0, -1};
    case TemplateDirection::South: return {0, 0, 1};
    case TemplateDirection::West: return {-1, 0, 0};
    case TemplateDirection::East: return {1, 0, 0};
    }
    return {};
}

// World box of a template placed at `origin`. Rotation can move the far corner
// to negative X/Z, so the box is rebuilt from the extremes rather than assumed
// to start at the origin. Y is never rotated.
[[nodiscard]] BoundingBox bounds(const StructureTemplate& t, BlockPosition origin, int rotation) {
    const BlockPosition corner = rotate({t.size.x - 1, t.size.y - 1, t.size.z - 1}, rotation);
    return {origin.x + std::min(0, corner.x), origin.y, origin.z + std::min(0, corner.z),
        origin.x + std::max(0, corner.x), origin.y + corner.y, origin.z + std::max(0, corner.z)};
}

[[nodiscard]] bool contains(const BoundingBox& box, BlockPosition p) {
    return p.x >= box.minX && p.x <= box.maxX && p.y >= box.minY && p.y <= box.maxY && p.z >= box.minZ && p.z <= box.maxZ;
}

[[nodiscard]] bool contains(const BoundingBox& a, const BoundingBox& b) {
    return contains(a, BlockPosition{b.minX, b.minY, b.minZ}) && contains(a, BlockPosition{b.maxX, b.maxY, b.maxZ});
}

[[nodiscard]] bool overlaps(const BoundingBox& a, const BoundingBox& b) {
    return a.minX <= b.maxX && b.minX <= a.maxX && a.minY <= b.maxY && b.minY <= a.maxY && a.minZ <= b.maxZ && b.minZ <= a.maxZ;
}

// Java's `(min + max) / 2`: integer division truncating towards zero, *not*
// towards negative infinity, so it cannot be replaced by `floorDiv`. The
// intermediate is widened because the two bounds can be far apart.
[[nodiscard]] int midpoint(int minimum, int maximum) {
    return static_cast<int>((static_cast<std::int64_t>(minimum) + maximum) / 2);
}

// Java: `Util.shuffle`, a Fisher-Yates walking downwards. One `nextInt` per
// element after the first.
template <class T>
void shuffle(std::vector<T>& values, LegacyRandom& random) {
    for (int i = static_cast<int>(values.size()); i > 1; --i) {
        const int j = random.nextInt(i);
        std::swap(values[i - 1], values[j]);
    }
}

// A template's connector indices in the order jigsaw placement considers them:
// shuffled, then stably ordered by descending selection priority. Costs one
// shuffle from the stream, so it is called exactly where Java calls it - once
// per source piece, and again for every candidate rotation.
[[nodiscard]] std::vector<std::size_t> shuffledConnectors(const StructureTemplate& t, LegacyRandom& random) {
    std::vector<std::size_t> result(t.connectors.size());
    std::iota(result.begin(), result.end(), 0);
    shuffle(result, random);
    std::stable_sort(result.begin(), result.end(), [&](auto a, auto b) {
        return t.connectors[a].selectionPriority > t.connectors[b].selectionPriority;
    });
    return result;
}

// A pool's elements as a shuffled candidate list, each repeated `weight` times
// so that a plain walk down the list is a weighted draw. The empty pool name
// yields no candidates and consumes no randomness.
[[nodiscard]] std::vector<const TemplatePoolElement*> shuffledElements(
    const StructureTemplateCatalog& catalog, const std::string& pool, LegacyRandom& random
) {
    std::vector<const TemplatePoolElement*> result;
    if (pool.empty()) return result;
    for (const auto& entry : catalog.pools.at(pool).elements) {
        for (int i = 0; i < entry.weight; ++i) result.push_back(&entry);
    }
    shuffle(result, random);
    return result;
}

// --- Block states ----------------------------------------------------------

// Java: `BlockState.rotate`, narrowed to the property families a template
// block can carry.
//
//   facing=<north|east|south|west>  single-faced blocks: stairs, doors, chests
//   axis=<x|z>                      pillar-like blocks, swapped by odd turns
//   rotation=<0..15>                signs and banners, in sixteenths of a turn
//
// Multi-faced blocks - fences, walls, panes, redstone - instead encode their
// direction in the property *name* (`north=true`), so names are rotated too.
//
// The state is rebuilt from a `std::map`, so the result lists its properties
// in alphabetical order whatever order they arrived in. `state` is assumed to
// be `name[key=value,...]`, which is what the catalog importer emits.
[[nodiscard]] std::string rotateBlockState(const std::string& state, int rotation) {
    const auto bracket = state.find('[');
    if (bracket == std::string::npos) return state;

    const auto rotateFacing = [rotation](const std::string& value) {
        static const std::array<std::string, kRotationCount> names{"north", "east", "south", "west"};
        const auto found = std::ranges::find(names, value);
        return found == names.end() ? value : names[(found - names.begin() + rotation) % kRotationCount];
    };

    std::map<std::string, std::string> properties;
    std::istringstream input(state.substr(bracket + 1, state.size() - bracket - 2));
    std::string property;
    while (std::getline(input, property, ',')) {
        const auto equals = property.find('=');
        if (equals == std::string::npos) continue;
        const auto key = property.substr(0, equals);
        auto value = property.substr(equals + 1);
        if (key == "facing") value = rotateFacing(value);
        else if (key == "axis" && rotation % 2 && (value == "x" || value == "z")) value = value == "x" ? "z" : "x";
        else if (key == "rotation") value = std::to_string((std::stoi(value) + rotation * 4) % 16);
        properties[rotateFacing(key)] = value;
    }

    std::string result = state.substr(0, bracket + 1);
    for (const auto& [key, value] : properties) {
        if (result.back() != '[') result += ',';
        result += key + "=" + value;
    }
    return result + ']';
}

// --- Piece construction ----------------------------------------------------

// Resolves a column's ground level. Passed down from stage 5 rather than taken
// from a router directly, so that assembly stays a pure function of its inputs.
using SurfaceHeight = std::function<int(int, int)>;

// Java: `SinglePoolElement.place` reduced to the data stage 8 needs. Template
// coordinates are rotated and translated into world coordinates once, here, so
// that placing the piece later is only a clip and a write.
[[nodiscard]] StructurePiece makePiece(
    const StructureTemplate& source,
    const TemplatePoolElement& element,
    BlockPosition origin,
    int rotation,
    const SurfaceHeight& surfaceHeight
) {
    StructurePiece piece;
    piece.bounds = bounds(source, origin, rotation);
    piece.groundLevelDelta = source.groundLevelDelta;
    piece.projection = element.projection;
    piece.templatePiece = true;
    piece.processors = element.processors;
    piece.feature = source.feature;

    // A terrain-matching element follows the surface column by column instead
    // of sitting at one Y, so its blocks cannot share the piece's origin.
    const auto toWorld = [&](BlockPosition local) {
        BlockPosition world = add(origin, rotate(local, rotation));
        if (element.projection == PieceProjection::TerrainMatching) {
            world.y = surfaceHeight(world.x, world.z) + local.y - 1;
        }
        return world;
    };

    // Rotating a state is string work and a template reuses the same state
    // object for thousands of blocks, so each distinct one is rotated once and
    // the rotated copy is shared by every block that had it.
    std::map<const BlockData*, std::shared_ptr<const BlockData>> rotated;
    for (const auto& block : source.blocks) {
        auto data = block.data;
        if (data && rotation != 0) {
            auto [entry, inserted] = rotated.try_emplace(data.get());
            if (inserted) {
                auto copy = std::make_shared<BlockData>(*data);
                copy->state = rotateBlockState(copy->state, rotation);
                entry->second = std::move(copy);
            }
            data = entry->second;
        }
        piece.blocks.push_back({toWorld(block.position), block.block, std::move(data)});
    }
    // The jigsaw blocks themselves are not part of the finished structure;
    // each names the block that should be left where it stood.
    for (const auto& connector : source.connectors) {
        piece.blocks.push_back({toWorld(connector.position), connector.finalBlock});
    }
    return piece;
}

// --- Jigsaw assembly -------------------------------------------------------

// Java: `JigsawPlacement.addPieces`. One assembly run, and single-use: the
// piece list is moved out of it at the end.
//
// The shape is a work list rather than recursion. A piece is placed, then
// queued; expanding it walks its jigsaw connectors and, for each, tries the
// candidate templates its pool offers until one fits. Each attachment queues
// the piece it just placed, so the structure grows outwards until every
// connector is either satisfied, blocked, or past `maxDepth`.
//
// Draws come off `random_` in this order and no other: root rotation, root
// element, the root's connector shuffle (only when the pool names a start
// jigsaw), then per source piece its connector shuffle, and per connector the
// candidate and fallback shuffles followed by a rotation shuffle and one
// connector shuffle per rotation tried.
class JigsawAssembler {
public:
    JigsawAssembler(const StructureTemplateCatalog& catalog, const TemplateStartPool& config,
                    LegacyRandom& random, const SurfaceHeight& surfaceHeight)
        : catalog_(catalog), config_(config), random_(random), surfaceHeight_(surfaceHeight) {}

    // Builds the structure rooted at `origin`, whose Y is replaced by the
    // pool's own start height. Returns an empty list when the pool cannot
    // produce a root piece, which the caller treats as "no structure here".
    //
    // `generationPoint`, when given, receives the point the structure is
    // considered to be at: the centre of the root box at ground level. Stage 5
    // re-checks the biome there.
    [[nodiscard]] std::vector<StructurePiece> assemble(BlockPosition origin, BlockPosition* generationPoint);

private:
    // A placed piece whose connectors have not been expanded yet.
    struct Pending {
        const StructureTemplate* source{};
        BlockPosition origin;
        int rotation{};
        int depth{};
        std::size_t space{};    // index into spaces_
        int priority{};         // the connector's placementPriority; highest expands first
        std::size_t piece{};    // index into pieces_
    };

    // The room an attachment has to fit into: inside `bounds`, and clear of
    // everything already in `occupied`. The first space is the whole structure;
    // a piece whose connector points back into itself gets one of its own, so
    // that an interior fitting is not rejected by the piece surrounding it.
    struct Space {
        BoundingBox bounds;
        std::vector<BoundingBox> occupied;
    };

    // One connector of a source piece, resolved into world space. Everything
    // an attachment attempt needs to know about where it is attaching from.
    struct Joint {
        const Pending* source{};
        BoundingBox sourceBounds;          // copy: pieces_ may reallocate mid-expansion
        const TemplateConnector* connector{};
        BlockPosition position;            // world position of the jigsaw block
        BlockPosition attachment;          // the block in front of it, where the child's jigsaw goes
        BlockPosition facing;              // world direction the connector points in
        std::size_t space{};
    };

    [[nodiscard]] const TemplatePoolElement* pickRootElement();
    [[nodiscard]] const TemplateConnector* findStartConnector(const StructureTemplate& root);
    void expand(const Pending& source);
    [[nodiscard]] bool attachElement(const Joint& joint, const TemplatePoolElement& element);
    [[nodiscard]] bool attachConnector(const Joint& joint, const TemplatePoolElement& element,
                                       const StructureTemplate& candidate, int rotation,
                                       const TemplateConnector& target, int expandTo);
    [[nodiscard]] int expansionHeight(const StructureTemplate& candidate, int rotation,
                                      const std::vector<std::size_t>& connectors) const;
    [[nodiscard]] int tallestTemplate(const std::string& pool) const;

    const StructureTemplateCatalog& catalog_;
    const TemplateStartPool& config_;
    LegacyRandom& random_;
    const SurfaceHeight& surfaceHeight_;
    std::vector<StructurePiece> pieces_;
    std::vector<Pending> queue_;
    std::vector<Space> spaces_;
};

std::vector<StructurePiece> JigsawAssembler::assemble(BlockPosition origin, BlockPosition* generationPoint) {
    const int rotation = random_.nextInt(kRotationCount);
    const TemplatePoolElement* root = pickRootElement();
    // An empty template ID is `EmptyPoolElement`: a valid pool entry, but not
    // something a structure can be rooted in.
    if (root == nullptr || root->templateId.empty()) return {};
    const StructureTemplate& rootTemplate = catalog_.templates.at(root->templateId);

    // When the pool names a start jigsaw, the structure is positioned by that
    // connector rather than by the template's own corner, so the root is
    // shifted until the connector lands on the requested point.
    origin.y = config_.startY;
    BlockPosition anchor{};
    if (!config_.startJigsawName.empty()) {
        const TemplateConnector* start = findStartConnector(rootTemplate);
        if (start == nullptr) return {};
        anchor = rotate(start->position, rotation);
    }
    origin = subtract(origin, anchor);

    const BoundingBox rootBox = bounds(rootTemplate, origin, rotation);
    const int centerX = midpoint(rootBox.minX, rootBox.maxX);
    const int centerZ = midpoint(rootBox.minZ, rootBox.maxZ);
    // `startY` is an offset from the surface when the pool projects to it, and
    // an absolute height otherwise.
    const int groundY = config_.projectToSurface ? config_.startY + surfaceHeight_(centerX, centerZ) : origin.y;
    const int centerY = groundY + anchor.y;
    origin.y = groundY - rootTemplate.groundLevelDelta;
    if (generationPoint != nullptr) *generationPoint = {centerX, centerY, centerZ};

    pieces_.push_back(makePiece(rootTemplate, *root, origin, rotation, surfaceHeight_));
    if (config_.maxDepth == 0) return std::move(pieces_);

    // Everything the jigsaw grows has to stay inside one box centred on the
    // generation point. `maxDistance` is what keeps a structure local; the
    // build limits clamp it so a deep or tall pool cannot reach out of the
    // world.
    spaces_.push_back({{centerX - config_.maxDistance,
                        std::max(TerrainChunk::minY, centerY - config_.maxDistance),
                        centerZ - config_.maxDistance,
                        centerX + config_.maxDistance,
                        std::min(TerrainChunk::maxY - 1, centerY + config_.maxDistance),
                        centerZ + config_.maxDistance},
                       {pieces_.front().bounds}});
    queue_.push_back({&rootTemplate, origin, rotation, 0, 0, 0, 0});

    while (!queue_.empty()) {
        // Highest placement priority first. `max_element` returns the first of
        // several equal maxima, so ties keep insertion order.
        const auto next = std::max_element(queue_.begin(), queue_.end(),
            [](const Pending& a, const Pending& b) { return a.priority < b.priority; });
        const Pending source = *next;
        queue_.erase(next);
        expand(source);
    }
    return std::move(pieces_);
}

// Java: `StructureTemplatePool.getRandomTemplate`. `validate` guarantees a
// non-empty pool of positive weights, so the draw always has a bound.
const TemplatePoolElement* JigsawAssembler::pickRootElement() {
    const StructureTemplatePool& pool = catalog_.pools.at(config_.pool);
    int total = 0;
    for (const auto& element : pool.elements) total += element.weight;
    int choice = random_.nextInt(total);
    for (const auto& element : pool.elements) {
        choice -= element.weight;
        if (choice < 0) return &element;
    }
    return nullptr;
}

// The root's `startJigsawName` connector. The shuffle is consumed whether or
// not the connector is found, because Java searches the same shuffled list.
const TemplateConnector* JigsawAssembler::findStartConnector(const StructureTemplate& root) {
    for (std::size_t index : shuffledConnectors(root, random_)) {
        if (root.connectors[index].name == config_.startJigsawName) return &root.connectors[index];
    }
    return nullptr;
}

void JigsawAssembler::expand(const Pending& source) {
    // Copied out because attaching a child can reallocate `pieces_`, and this
    // box is read for the whole expansion.
    const BoundingBox sourceBounds = pieces_[source.piece].bounds;
    // Allocated lazily and shared by every inward-pointing connector of this
    // piece, so two of them cannot each claim the interior.
    std::optional<std::size_t> interior;

    for (std::size_t index : shuffledConnectors(*source.source, random_)) {
        const TemplateConnector& connector = source.source->connectors[index];
        if (connector.pool.empty()) continue;

        Joint joint;
        joint.source = &source;
        joint.sourceBounds = sourceBounds;
        joint.connector = &connector;
        joint.position = add(source.origin, rotate(connector.position, source.rotation));
        joint.facing = rotate(direction(connector.front), source.rotation);
        joint.attachment = add(joint.position, joint.facing);
        joint.space = source.space;
        if (contains(sourceBounds, joint.attachment)) {
            if (!interior) {
                interior = spaces_.size();
                spaces_.push_back({sourceBounds, {}});
            }
            joint.space = *interior;
        }

        // At the depth limit only the fallback pool is offered, which is how a
        // structure is capped off rather than left with an open corridor.
        auto candidates = source.depth == config_.maxDepth
            ? std::vector<const TemplatePoolElement*>{}
            : shuffledElements(catalog_, connector.pool, random_);
        const auto fallback = shuffledElements(catalog_, catalog_.pools.at(connector.pool).fallback, random_);
        candidates.insert(candidates.end(), fallback.begin(), fallback.end());

        for (const TemplatePoolElement* element : candidates) {
            // `EmptyPoolElement` is a successful outcome - the connector is
            // meant to stay bare - so the rest of the list is not tried.
            if (element->templateId.empty()) break;
            if (attachElement(joint, *element)) break;
        }
    }
}

// Tries one candidate element in every rotation, and within a rotation every
// one of its connectors, stopping at the first that fits.
bool JigsawAssembler::attachElement(const Joint& joint, const TemplatePoolElement& element) {
    const StructureTemplate& candidate = catalog_.templates.at(element.templateId);
    std::vector<int> rotations{0, 1, 2, 3};
    shuffle(rotations, random_);
    for (int rotation : rotations) {
        // Re-shuffled per rotation: Java asks the element for its jigsaw
        // blocks once per rotation it tries, and each ask costs a shuffle.
        const std::vector<std::size_t> connectors = shuffledConnectors(candidate, random_);
        const int expandTo = expansionHeight(candidate, rotation, connectors);
        for (std::size_t index : connectors) {
            if (attachConnector(joint, element, candidate, rotation, candidate.connectors[index], expandTo)) {
                return true;
            }
        }
    }
    return false;
}

bool JigsawAssembler::attachConnector(
    const Joint& joint,
    const TemplatePoolElement& element,
    const StructureTemplate& candidate,
    int rotation,
    const TemplateConnector& target,
    int expandTo
) {
    // The two connectors have to point at each other, and the target's name
    // has to be the one the source asks for; an unnamed target matches any.
    if (rotate(direction(target.front), rotation) != negate(joint.facing)) return false;
    if (!target.name.empty() && joint.connector->target != target.name) return false;
    // A connector that is not rollable also has to agree on which way is up,
    // which is what keeps stairs and doorways upright.
    if (!joint.connector->rollable
        && rotate(direction(joint.connector->top), joint.source->rotation) != rotate(direction(target.top), rotation)) {
        return false;
    }

    // Position the candidate so its own connector lands on the attachment
    // block in front of the source's.
    const BlockPosition local = rotate(target.position, rotation);
    BlockPosition position = subtract(joint.attachment, local);

    const int sourceLocalY = joint.position.y - joint.sourceBounds.minY;
    const int delta = sourceLocalY - local.y + joint.facing.y;
    const StructurePiece& sourcePiece = pieces_[joint.source->piece];
    const bool sourceRigid = sourcePiece.projection == PieceProjection::Rigid;
    const bool targetRigid = element.projection == PieceProjection::Rigid;
    // With a terrain-matching piece on either side the join is anchored to the
    // surface instead of to the source piece's own Y. `surface` is only
    // sampled - and only meaningful - in that case.
    const bool followsTerrain = !sourceRigid || !targetRigid;
    const int surface = followsTerrain ? surfaceHeight_(joint.position.x, joint.position.z) : 0;
    if (followsTerrain) position.y = surface - local.y;

    BoundingBox box = bounds(candidate, position, rotation);
    if (expandTo > 0) box.maxY = box.minY + std::max(expandTo + 1, box.maxY - box.minY);

    Space& space = spaces_[joint.space];
    if (!contains(space.bounds, box)) return false;
    if (std::ranges::any_of(space.occupied, [&](const BoundingBox& taken) { return overlaps(box, taken); })) return false;

    StructurePiece child = makePiece(candidate, element, position, rotation, surfaceHeight_);
    // The reserved height from the expansion hack, not the template's own.
    child.bounds = box;
    const int sourceGround = sourcePiece.groundLevelDelta;
    if (targetRigid) child.groundLevelDelta = sourceGround - delta;

    // Both sides record the join, because stage 5's beardifier pulls terrain
    // towards a junction from whichever piece a chunk happens to reference.
    const int junctionY = sourceRigid ? joint.position.y
        : targetRigid ? position.y + local.y
        : surface + delta / 2;
    pieces_[joint.source->piece].junctions.push_back(
        {joint.attachment.x, junctionY - sourceLocalY + sourceGround, joint.attachment.z, delta, element.projection});
    child.junctions.push_back(
        {joint.position.x, junctionY - local.y + child.groundLevelDelta, joint.position.z, -delta, sourcePiece.projection});

    space.occupied.push_back(box);
    const std::size_t index = pieces_.size();
    pieces_.push_back(std::move(child));
    if (joint.source->depth + 1 <= config_.maxDepth) {
        queue_.push_back({&candidate, position, rotation, joint.source->depth + 1, joint.space,
                          joint.connector->placementPriority, index});
    }
    return true;
}

// Java's village "expansion hack". A short piece whose connectors point back
// into itself reserves enough vertical room for the tallest piece those
// connectors could ever attach, so that a later tall piece is not rejected by
// a space its own predecessor failed to claim.
int JigsawAssembler::expansionHeight(
    const StructureTemplate& candidate, int rotation, const std::vector<std::size_t>& connectors
) const {
    if (!config_.expansionHack || candidate.size.y > 16) return 0;
    const BoundingBox localBox = bounds(candidate, {}, rotation);
    int expandTo = 0;
    for (std::size_t index : connectors) {
        const TemplateConnector& connector = candidate.connectors[index];
        if (connector.pool.empty()) continue;
        const BlockPosition attachment = add(rotate(connector.position, rotation), rotate(direction(connector.front), rotation));
        if (!contains(localBox, attachment)) continue;
        expandTo = std::max({expandTo,
                             tallestTemplate(connector.pool),
                             tallestTemplate(catalog_.pools.at(connector.pool).fallback)});
    }
    return expandTo;
}

int JigsawAssembler::tallestTemplate(const std::string& pool) const {
    if (pool.empty()) return 0;
    int tallest = 0;
    for (const auto& element : catalog_.pools.at(pool).elements) {
        if (!element.templateId.empty()) {
            tallest = std::max(tallest, catalog_.templates.at(element.templateId).size.y);
        }
    }
    return tallest;
}

} // namespace

std::vector<StructurePiece> assembleJigsaw(
    const StructureTemplateCatalog& catalog,
    StructureVariant variant,
    BlockPosition origin,
    LegacyRandom& random,
    const std::function<int(int, int)>& surfaceHeight,
    BlockPosition* generationPoint
) {
    JigsawAssembler assembler(catalog, catalog.starts.at(variant), random, surfaceHeight);
    return assembler.assemble(origin, generationPoint);
}

// --- Template processors ---------------------------------------------------

// Java: `StructurePlaceSettings.getRandom(pos)`, i.e. `Mth.getSeed` fed to a
// legacy random. Every processor decision for a block is a function of that
// block's world position alone, which is what lets pieces be processed in any
// order. The signed multiplication and arithmetic shift are spelled out so
// that C++ wraparound stays defined and matches Java's.
[[nodiscard]] std::uint64_t positionalProcessorSeed(BlockPosition position) {
    const auto x = static_cast<std::int32_t>(static_cast<std::uint32_t>(position.x) * 3129871U);
    std::uint64_t seed = static_cast<std::uint64_t>(static_cast<std::int64_t>(x))
        ^ static_cast<std::uint64_t>(position.z) * 116129781ULL
        ^ static_cast<std::uint64_t>(position.y);
    seed = seed * seed * 42317861ULL + seed * 11ULL;
    return static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(seed) >> 16);
}

std::optional<Block> processStructureBlock(const StructureBlock& block, Block existing,
    const std::vector<TemplateProcessor>& processors, std::shared_ptr<const BlockData>* outputData) {
    const std::uint64_t seed = positionalProcessorSeed(block.position);
    LegacyRandom random(seed);

    Block result = block.block;
    auto data = block.data;
    // Java's `RuleProcessor` lists share one random stream per list and stop
    // at the first rule that matches. Both are modelled by `ruleGroup`: a
    // nonzero group reseeds once at its start and is skipped once matched,
    // while group 0 means a standalone processor that reseeds every time.
    int lastGroup = -1;
    int matchedGroup = -1;

    for (const auto& processor : processors) {
        if (processor.ruleGroup != 0 && matchedGroup == processor.ruleGroup) continue;
        if (processor.ruleGroup == 0 || processor.ruleGroup != lastGroup) random.setSeed(seed);
        lastGroup = processor.ruleGroup;

        // An empty filter list means "any". Names match on the block ID only,
        // so a rule keyed on `minecraft:oak_stairs` also matches its states.
        if (!processor.inputNames.empty()) {
            if (!data) continue;
            const auto name = data->state.substr(0, data->state.find('['));
            if (std::ranges::find(processor.inputNames, name) == processor.inputNames.end()) continue;
        }
        if (!processor.inputs.empty() && std::ranges::find(processor.inputs, result) == processor.inputs.end()) continue;
        if (!processor.locations.empty() && std::ranges::find(processor.locations, existing) == processor.locations.end()) continue;

        switch (processor.kind) {
        case TemplateProcessorKind::Ignore:
            // The block is dropped entirely; the world keeps what it had.
            return std::nullopt;
        case TemplateProcessorKind::Rule:
            if (processor.probability >= 1 || random.nextFloat() < processor.probability) {
                result = processor.output;
                if (processor.ruleGroup != 0) matchedGroup = processor.ruleGroup;
                data = processor.outputState.empty() ? nullptr
                    : std::make_shared<BlockData>(BlockData{processor.outputState, {}});
            }
            break;
        case TemplateProcessorKind::Rot:
            // `probability` is retained integrity here, not a match chance.
            if (random.nextFloat() > processor.probability) return std::nullopt;
            break;
        }
    }
    if (outputData != nullptr) *outputData = std::move(data);
    return result;
}

} // namespace detail
} // namespace mcworld
