#pragma once

#include "mcworld/generation.hpp"

#include <map>
#include <string>

namespace mcworld {

enum class TemplateDirection : std::uint8_t { Down, Up, North, South, West, East };
enum class TemplateRotation : std::uint8_t { None, Clockwise90, Clockwise180, CounterClockwise90 };

struct TemplateConnector {
    BlockPosition position; // template-local
    TemplateDirection front{TemplateDirection::North};
    TemplateDirection top{TemplateDirection::Up};
    std::string name, target, pool;
    bool rollable{};
    int selectionPriority{}, placementPriority{};
    Block finalBlock{Block::Air};
};

struct StructureTemplate {
    BlockPosition size;
    std::vector<StructureBlock> blocks; // template-local positions
    std::vector<TemplateConnector> connectors;
    int groundLevelDelta{1};
};

struct TemplatePoolElement {
    std::string templateId; // empty denotes EmptyPoolElement, a successful terminator
    int weight{1};
    PieceProjection projection{PieceProjection::Rigid};
    std::vector<TemplateProcessor> processors;
};

struct StructureTemplatePool {
    std::vector<TemplatePoolElement> elements;
    std::string fallback; // empty denotes minecraft:empty
};

struct TemplateStartPool {
    std::string pool;
    int maxDepth{6};
    int maxDistance{80};
    int startY{};
    bool projectToSurface{true};
};

// Catalog data is supplied by the caller; no Minecraft template assets are
// bundled. Validate once before use and keep it immutable for the generator's
// lifetime. Unsupported/missing IDs are errors, not substitute box structures.
struct StructureTemplateCatalog {
    std::map<std::string, StructureTemplate> templates;
    std::map<std::string, StructureTemplatePool> pools;
    std::map<StructureVariant, TemplateStartPool> starts;
    void validate() const;
};

} // namespace mcworld
