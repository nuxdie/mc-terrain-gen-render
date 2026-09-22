#include "mcworld/structure_templates.hpp"
#include "generation_internal.hpp"

#include <deque>
#include <numeric>
#include <stdexcept>
#include <sstream>

namespace mcworld {
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
BlockPosition add(BlockPosition a, BlockPosition b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
BlockPosition subtract(BlockPosition a, BlockPosition b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
BlockPosition rotate(BlockPosition p, int rotation) {
    switch (rotation) {
    case 1: return {-p.z, p.y, p.x};
    case 2: return {-p.x, p.y, -p.z};
    case 3: return {p.z, p.y, -p.x};
    default: return p;
    }
}
BlockPosition direction(TemplateDirection d) {
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
BoundingBox bounds(const StructureTemplate& t, BlockPosition origin, int rotation) {
    const BlockPosition corner = rotate({t.size.x - 1, t.size.y - 1, t.size.z - 1}, rotation);
    return {origin.x + std::min(0, corner.x), origin.y, origin.z + std::min(0, corner.z),
        origin.x + std::max(0, corner.x), origin.y + corner.y, origin.z + std::max(0, corner.z)};
}
bool contains(const BoundingBox& box, BlockPosition p) {
    return p.x >= box.minX && p.x <= box.maxX && p.y >= box.minY && p.y <= box.maxY && p.z >= box.minZ && p.z <= box.maxZ;
}
bool contains(const BoundingBox& a, const BoundingBox& b) {
    return contains(a, BlockPosition{b.minX, b.minY, b.minZ}) && contains(a, BlockPosition{b.maxX, b.maxY, b.maxZ});
}
bool overlaps(const BoundingBox& a, const BoundingBox& b) {
    return a.minX <= b.maxX && b.minX <= a.maxX && a.minY <= b.maxY && b.minY <= a.maxY && a.minZ <= b.maxZ && b.minZ <= a.maxZ;
}
template<class T> void shuffle(std::vector<T>& values, LegacyRandom& random) {
    for (int i = static_cast<int>(values.size()); i > 1; --i) {
        const int j = random.nextInt(i);
        std::swap(values[i - 1], values[j]);
    }
}
std::vector<std::size_t> connectors(const StructureTemplate& t, LegacyRandom& random) {
    std::vector<std::size_t> result(t.connectors.size());
    std::iota(result.begin(), result.end(), 0);
    shuffle(result, random);
    std::stable_sort(result.begin(), result.end(), [&](auto a, auto b) {
        return t.connectors[a].selectionPriority > t.connectors[b].selectionPriority;
    });
    return result;
}
std::vector<const TemplatePoolElement*> elements(const StructureTemplateCatalog& catalog, const std::string& pool, LegacyRandom& random) {
    std::vector<const TemplatePoolElement*> result;
    if (pool.empty()) return result;
    for (const auto& entry : catalog.pools.at(pool).elements)
        for (int i = 0; i < entry.weight; ++i) result.push_back(&entry);
    shuffle(result, random);
    return result;
}
StructurePiece piece(const StructureTemplate& t, const TemplatePoolElement& e, BlockPosition origin, int rotation,
                     const std::function<int(int, int)>& height) {
    StructurePiece result;
    result.bounds = bounds(t, origin, rotation);
    result.groundLevelDelta = t.groundLevelDelta;
    result.projection = e.projection;
    result.templatePiece = true;
    result.processors = e.processors;
    result.feature = t.feature;
    std::map<const BlockData*, std::shared_ptr<const BlockData>> rotated;
    for (const auto& block : t.blocks) {
        BlockPosition p = add(origin, rotate(block.position, rotation));
        if (e.projection == PieceProjection::TerrainMatching) p.y = height(p.x, p.z) + block.position.y - 1;
        auto data = block.data;
        if (data && rotation != 0) {
            auto [it, inserted] = rotated.try_emplace(data.get());
            if (inserted) {
                auto copy = std::make_shared<BlockData>(*data);
                const auto bracket = copy->state.find('[');
                if (bracket != std::string::npos) {
                    std::map<std::string, std::string> properties;
                    std::istringstream input(copy->state.substr(bracket + 1, copy->state.size() - bracket - 2));
                    std::string property;
                    const auto rotateFacing = [&](std::string value) {
                        const std::array<std::string, 4> names{"north", "east", "south", "west"};
                        const auto found = std::ranges::find(names, value);
                        return found == names.end() ? value : names[(found - names.begin() + rotation) % 4];
                    };
                    while (std::getline(input, property, ',')) {
                        const auto eq = property.find('=');
                        if (eq == std::string::npos) continue;
                        auto key = property.substr(0, eq), value = property.substr(eq + 1);
                        if (key == "facing") value = rotateFacing(value);
                        else if (key == "axis" && rotation % 2 && (value == "x" || value == "z")) value = value == "x" ? "z" : "x";
                        else if (key == "rotation") value = std::to_string((std::stoi(value) + rotation * 4) % 16);
                        properties[rotateFacing(key)] = value;
                    }
                    copy->state.resize(bracket + 1);
                    for (const auto& [key, value] : properties) { if (copy->state.back() != '[') copy->state += ','; copy->state += key + "=" + value; }
                    copy->state += ']';
                }
                it->second = std::move(copy);
            }
            data = it->second;
        }
        result.blocks.push_back({p, block.block, std::move(data)});
    }
    for (const auto& connector : t.connectors) {
        BlockPosition p = add(origin, rotate(connector.position, rotation));
        if (e.projection == PieceProjection::TerrainMatching) p.y = height(p.x, p.z) + connector.position.y - 1;
        result.blocks.push_back({p, connector.finalBlock});
    }
    return result;
}
} // namespace

std::vector<StructurePiece> assembleJigsaw(const StructureTemplateCatalog& catalog, StructureVariant variant,
    BlockPosition origin, LegacyRandom& random, const std::function<int(int, int)>& surfaceHeight, BlockPosition* generationPoint) {
    const auto& config = catalog.starts.at(variant);
    const int rotation = random.nextInt(4);
    const auto& pool = catalog.pools.at(config.pool);
    int weight = 0;
    for (const auto& e : pool.elements) weight += e.weight;
    int choice = random.nextInt(weight);
    const TemplatePoolElement* root = nullptr;
    for (const auto& e : pool.elements) { choice -= e.weight; if (choice < 0) { root = &e; break; } }
    if (root == nullptr || root->templateId.empty()) return {};
    const auto& rootTemplate = catalog.templates.at(root->templateId);
    origin.y = config.startY;
    BlockPosition anchor{};
    if (!config.startJigsawName.empty()) {
        bool found = false;
        for (auto i : connectors(rootTemplate, random)) if (rootTemplate.connectors[i].name == config.startJigsawName) {
            anchor = rotate(rootTemplate.connectors[i].position, rotation); found = true; break;
        }
        if (!found) return {};
    }
    origin = subtract(origin, anchor);
    const auto rootBox = bounds(rootTemplate, origin, rotation);
    const int centerX = static_cast<int>((static_cast<std::int64_t>(rootBox.minX) + rootBox.maxX) / 2);
    const int centerZ = static_cast<int>((static_cast<std::int64_t>(rootBox.minZ) + rootBox.maxZ) / 2);
    const int groundY = config.projectToSurface ? config.startY + surfaceHeight(centerX, centerZ) : origin.y;
    origin.y = groundY - rootTemplate.groundLevelDelta;
    const int centerY = groundY + anchor.y;
    if (generationPoint) *generationPoint = {centerX, centerY, centerZ};
    std::vector<StructurePiece> pieces{piece(rootTemplate, *root, origin, rotation, surfaceHeight)};
    if (config.maxDepth == 0) return pieces;
    struct State { const StructureTemplate* t; BlockPosition origin; int rotation, depth, space, priority; std::size_t piece; };
    std::vector<State> queue{{&rootTemplate, origin, rotation, 0, 0, 0, 0}};
    struct Space { BoundingBox bounds; std::vector<BoundingBox> occupied; };
    std::vector<Space> spaces{{{centerX - config.maxDistance, std::max(TerrainChunk::minY, centerY - config.maxDistance), centerZ - config.maxDistance,
        centerX + config.maxDistance, std::min(TerrainChunk::maxY - 1, centerY + config.maxDistance), centerZ + config.maxDistance}, {pieces[0].bounds}}};
    while (!queue.empty()) {
        const auto it = std::max_element(queue.begin(), queue.end(), [](const State& a, const State& b) { return a.priority < b.priority; });
        const State source = *it;
        queue.erase(it);
        const BoundingBox sourceBox = pieces[source.piece].bounds;
        int insideSpace = -1;
        for (std::size_t connectorIndex : connectors(*source.t, random)) {
            const auto& joint = source.t->connectors[connectorIndex];
            if (joint.pool.empty()) continue;
            const BlockPosition sourcePos = add(source.origin, rotate(joint.position, source.rotation));
            const BlockPosition facing = rotate(direction(joint.front), source.rotation);
            const BlockPosition targetPos = add(sourcePos, facing);
            int space = source.space;
            if (contains(sourceBox, targetPos)) {
                if (insideSpace < 0) { insideSpace = static_cast<int>(spaces.size()); spaces.push_back({sourceBox, {}}); }
                space = insideSpace;
            }
            auto candidates = source.depth == config.maxDepth ? std::vector<const TemplatePoolElement*>{} : elements(catalog, joint.pool, random);
            const auto fallback = elements(catalog, catalog.pools.at(joint.pool).fallback, random);
            candidates.insert(candidates.end(), fallback.begin(), fallback.end());
            bool attached = false;
            for (const auto* e : candidates) {
                if (e->templateId.empty()) break;
                const auto& t = catalog.templates.at(e->templateId);
                std::vector<int> rotations{0, 1, 2, 3};
                shuffle(rotations, random);
                for (int r : rotations) {
                    const auto joints = connectors(t, random);
                    int expandTo = 0;
                    if (config.expansionHack && t.size.y <= 16) {
                        const auto localBox = bounds(t, {}, r);
                        for (auto j : joints) {
                            const auto& joint = t.connectors[j];
                            if (joint.pool.empty() || !contains(localBox, add(rotate(joint.position, r), rotate(direction(joint.front), r)))) continue;
                            const auto maxHeight = [&](const std::string& name) {
                                if (name.empty()) return 0;
                                int maximum = 0;
                                for (const auto& element : catalog.pools.at(name).elements)
                                    if (!element.templateId.empty()) maximum = std::max(maximum, catalog.templates.at(element.templateId).size.y);
                                return maximum;
                            };
                            expandTo = std::max({expandTo, maxHeight(joint.pool), maxHeight(catalog.pools.at(joint.pool).fallback)});
                        }
                    }
                    for (std::size_t targetIndex : joints) {
                        const auto& target = t.connectors[targetIndex];
                        const auto targetFacing = rotate(direction(target.front), r);
                        if (targetFacing != BlockPosition{-facing.x, -facing.y, -facing.z}
                            || (!target.name.empty() && joint.target != target.name)) continue;
                        if (!joint.rollable && rotate(direction(joint.top), source.rotation) != rotate(direction(target.top), r)) continue;
                        const BlockPosition local = rotate(target.position, r);
                        BlockPosition position = subtract(targetPos, local);
                        const int sourceLocalY = sourcePos.y - sourceBox.minY;
                        const int delta = sourceLocalY - local.y + facing.y;
                        const bool sourceRigid = pieces[source.piece].projection == PieceProjection::Rigid;
                        const bool targetRigid = e->projection == PieceProjection::Rigid;
                        const int surface = (!sourceRigid || !targetRigid) ? surfaceHeight(sourcePos.x, sourcePos.z) : 0;
                        if (!sourceRigid || !targetRigid) position.y = surface - local.y;
                        auto box = bounds(t, position, r);
                        if (expandTo > 0) box.maxY = box.minY + std::max(expandTo + 1, box.maxY - box.minY);
                        if (!contains(spaces[space].bounds, box)) continue;
                        if (std::ranges::any_of(spaces[space].occupied, [&](const BoundingBox& b) { return overlaps(box, b); })) continue;
                        auto child = piece(t, *e, position, r, surfaceHeight);
                        child.bounds = box;
                        const int sourceGround = pieces[source.piece].groundLevelDelta;
                        if (targetRigid) child.groundLevelDelta = sourceGround - delta;
                        const int junctionY = sourceRigid ? sourcePos.y : targetRigid ? position.y + local.y : surface + delta / 2;
                        pieces[source.piece].junctions.push_back({targetPos.x, junctionY - sourceLocalY + sourceGround, targetPos.z, delta, e->projection});
                        child.junctions.push_back({sourcePos.x, junctionY - local.y + child.groundLevelDelta, sourcePos.z, -delta, pieces[source.piece].projection});
                        spaces[space].occupied.push_back(box);
                        const auto index = pieces.size();
                        pieces.push_back(std::move(child));
                        if (source.depth + 1 <= config.maxDepth) queue.push_back({&t, position, r, source.depth + 1, space, joint.placementPriority, index});
                        attached = true;
                        break;
                    }
                    if (attached) break;
                }
                if (attached) break;
            }
        }
    }
    return pieces;
}

std::optional<Block> processStructureBlock(const StructureBlock& block, Block existing,
    const std::vector<TemplateProcessor>& processors, std::shared_ptr<const BlockData>* outputData) {
    // StructurePlaceSettings.getRandom(pos), using Mth.getSeed's signed int
    // multiplication and arithmetic right shift, with defined C++ wraparound.
    const auto x = static_cast<std::int32_t>(static_cast<std::uint32_t>(block.position.x) * 3129871U);
    std::uint64_t seed = static_cast<std::uint64_t>(static_cast<std::int64_t>(x))
        ^ static_cast<std::uint64_t>(block.position.z) * 116129781ULL
        ^ static_cast<std::uint64_t>(block.position.y);
    seed = seed * seed * 42317861ULL + seed * 11ULL;
    seed = static_cast<std::uint64_t>(std::bit_cast<std::int64_t>(seed) >> 16);
    Block result = block.block;
    auto data = block.data;
    int lastGroup = -1, matchedGroup = -1;
    LegacyRandom random(seed);
    for (const auto& processor : processors) {
        if (processor.ruleGroup != 0 && matchedGroup == processor.ruleGroup) continue;
        if (processor.ruleGroup == 0 || processor.ruleGroup != lastGroup) random.setSeed(seed);
        lastGroup = processor.ruleGroup;
        if (!processor.inputNames.empty()) {
            if (!data) continue;
            const auto name = data->state.substr(0, data->state.find('['));
            if (std::ranges::find(processor.inputNames, name) == processor.inputNames.end()) continue;
        }
        if (!processor.inputs.empty() && std::ranges::find(processor.inputs, result) == processor.inputs.end()) continue;
        if (!processor.locations.empty() && std::ranges::find(processor.locations, existing) == processor.locations.end()) continue;
        switch (processor.kind) {
        case TemplateProcessorKind::Ignore: return std::nullopt;
        case TemplateProcessorKind::Rule:
            if (processor.probability >= 1 || random.nextFloat() < processor.probability) {
                result = processor.output;
                if (processor.ruleGroup != 0) matchedGroup = processor.ruleGroup;
                data = processor.outputState.empty() ? nullptr : std::make_shared<BlockData>(BlockData{processor.outputState, {}});
            }
            break;
        case TemplateProcessorKind::Rot:
            if (random.nextFloat() > processor.probability) return std::nullopt;
            break;
        }
    }
    if (outputData) *outputData = std::move(data);
    return result;
}
} // namespace detail
} // namespace mcworld
