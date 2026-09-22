#!/usr/bin/env python3
"""Import local Minecraft 26.3 structure resources; requires only Python's stdlib.

The output is a generated binary catalog, not source code. Keep game assets in
an ignored build directory. Unsupported execution semantics are listed in the
catalog and printed, never silently represented as supported functionality.
"""
import argparse
import gzip
import io
import json
from pathlib import Path
import struct
import zipfile


class NbtReader:
    def __init__(self, data):
        self.stream = io.BytesIO(gzip.decompress(data) if data[:2] == b"\x1f\x8b" else data)

    def read(self, size):
        if size < 0:
            raise ValueError("Negative NBT length")
        data = self.stream.read(size)
        if len(data) != size:
            raise ValueError("Truncated NBT")
        return data

    def number(self, fmt):
        return struct.unpack(">" + fmt, self.read(struct.calcsize(fmt)))[0]

    def string(self):
        # Template names/properties are UTF-8; Java modified UTF-8 encodes NUL
        # specially and supplementary characters as surrogate pairs.
        raw = self.read(self.number("H")).replace(b"\xc0\x80", b"\0")
        return raw.decode("utf-8", "surrogatepass").encode("utf-16", "surrogatepass").decode("utf-16")

    def payload(self, kind, depth=0):
        if depth > 512:
            raise ValueError("NBT nesting exceeds 512")
        formats = {1: "b", 2: "h", 3: "i", 4: "q", 5: "f", 6: "d"}
        if kind in formats:
            return self.number(formats[kind])
        if kind == 8:
            return self.string()
        if kind in (7, 11, 12):
            count = self.number("i")
            if count < 0 or count > 16_000_000:
                raise ValueError("Invalid NBT array size")
            return [self.number({7: "b", 11: "i", 12: "q"}[kind]) for _ in range(count)]
        if kind == 9:
            child, count = self.number("B"), self.number("i")
            if count < 0 or count > 16_000_000:
                raise ValueError("Invalid NBT list size")
            return [self.payload(child, depth + 1) for _ in range(count)]
        if kind == 10:
            result = {}
            while (child := self.number("B")) != 0:
                name = self.string()
                result[name] = self.payload(child, depth + 1)
            return result
        raise ValueError(f"Unsupported NBT tag type {kind}")

    def root(self):
        if self.number("B") != 10:
            raise ValueError("Template NBT root must be a compound")
        self.string()
        value = self.payload(10)
        if self.stream.read(1):
            raise ValueError("Trailing NBT bytes")
        return value


def identifier(name):
    return name if ":" in name else "minecraft:" + name


def state_string(state):
    if isinstance(state, str):
        return state
    result = state.get("id", state.get("Name"))
    if result is None:
        raise ValueError("Block state is missing its identifier")
    properties = state.get("properties", state.get("Properties", {}))
    if properties:
        result += "[" + ",".join(f"{k}={v}" for k, v in sorted(properties.items())) + "]"
    return result


class Resources:
    def __init__(self, path):
        self.path = Path(path)
        self.archive = zipfile.ZipFile(path) if self.path.is_file() else None

    def read(self, name):
        return self.archive.read(name) if self.archive else (self.path / name).read_bytes()

    def json(self, kind, name):
        namespace, value = identifier(name).split(":", 1)
        return json.loads(self.read(f"data/{namespace}/{kind}/{value}.json"))

    def template(self, name):
        namespace, value = identifier(name).split(":", 1)
        return NbtReader(self.read(f"data/{namespace}/structure/{value}.nbt")).root()


class Importer:
    DIRECTIONS = {"down": 0, "up": 1, "north": 2, "south": 3, "west": 4, "east": 5}
    STARTS = {"village_plains": 1, "village_desert": 2, "village_savanna": 3,
              "village_snowy": 4, "village_taiga": 5, "ancient_city": 15}

    def __init__(self, resources):
        self.resources = resources
        self.templates, self.pools, self.starts = {}, {}, []
        self.states, self.state_ids = [], {}
        self.unsupported = set()
        self.tags = {}

    def state_id(self, state, nbt=None):
        data = (state_string(state), json.dumps(nbt, sort_keys=True, separators=(",", ":"), ensure_ascii=False) if nbt else "")
        if data not in self.state_ids:
            self.state_ids[data] = len(self.states)
            self.states.append(data)
        return self.state_ids[data]

    def tag(self, name, visiting=()):
        if name in visiting:
            raise ValueError("Recursive block tag: " + name)
        if name not in self.tags:
            result = []
            for value in self.resources.json("tags/block", name)["values"]:
                value = value["id"] if isinstance(value, dict) else value
                result.extend(self.tag(value[1:], visiting + (name,)) if value.startswith("#") else [value])
            self.tags[name] = sorted(set(result))
        return self.tags[name]

    def load_template(self, name, legacy=False):
        key = name + ("#legacy" if legacy else "")
        if key in self.templates:
            return key
        try:
            data = self.resources.template(name)
        except (KeyError, FileNotFoundError):
            # TemplateManager.getOrCreate returns an empty template for missing
            # resources. Keep its no-connectors behavior and candidate RNG draws.
            self.unsupported.add("missing template resource: " + name)
            self.templates[key] = ([1, 1, 1], [], [], "")
            return key
        palette = data.get("palette")
        if palette is None:
            palette = data["palettes"][0]
            if len(data["palettes"]) > 1:
                self.unsupported.add("multiple template palettes: " + name)
        blocks, connectors = [], []
        for entry in data["blocks"]:
            state = palette[entry["state"]]
            pos, nbt = entry["pos"], entry.get("nbt", {})
            block = state.get("id", state.get("Name"))
            if block in ("minecraft:structure_block", "minecraft:structure_void") or (legacy and block == "minecraft:air"):
                continue
            if block == "minecraft:jigsaw":
                front, top = state.get("properties", state.get("Properties", {})).get("orientation", "north_up").split("_")
                pool = identifier(nbt.get("pool", "empty"))
                pool = "" if pool == "minecraft:empty" else pool
                connectors.append((pos, self.DIRECTIONS[front], self.DIRECTIONS[top],
                    nbt.get("name", "minecraft:empty"), nbt.get("target", "minecraft:empty"), pool,
                    nbt.get("joint", "aligned" if front not in ("up", "down") else "rollable") == "rollable",
                    nbt.get("selection_priority", 0), nbt.get("placement_priority", 0), nbt.get("final_state", "minecraft:air")))
            else:
                blocks.append((pos, self.state_id(state, nbt)))
        # Java palettes place full cubes first, then other blocks and NBT blocks;
        # retain the template's stored order until full shape classification exists.
        self.templates[key] = (data["size"], blocks, connectors, "")
        if data.get("entities"):
            self.unsupported.add("template entities: " + name)
        for joint in connectors:
            if joint[5]:
                self.load_pool(joint[5])
        return key

    def processors(self, value):
        if isinstance(value, str):
            value = self.resources.json("worldgen/processor_list", value)
        result = []
        for group, processor in enumerate(value.get("processors", []), 1):
            kind = processor["processor_type"]
            if kind == "minecraft:block_rot":
                names = self.tag(processor["rottable_blocks"][1:]) if isinstance(processor.get("rottable_blocks"), str) else processor.get("rottable_blocks", [])
                result.append((2, names, "minecraft:air", processor["integrity"], 0))
            elif kind == "minecraft:rule":
                for rule in processor["rules"]:
                    pred = rule["input_predicate"]
                    if rule.get("position_predicate", {}).get("predicate_type", "minecraft:always_true") != "minecraft:always_true" or rule["location_predicate"]["predicate_type"] != "minecraft:always_true":
                        self.unsupported.add("processor predicate: " + json.dumps(rule, sort_keys=True))
                        continue
                    typ = pred["predicate_type"]
                    if typ in ("minecraft:block_match", "minecraft:random_block_match"):
                        names = [pred["block"]]
                    elif typ == "minecraft:tag_match":
                        names = self.tag(pred["tag"])
                    elif typ == "minecraft:always_true":
                        names = []
                    else:
                        self.unsupported.add("processor predicate: " + typ)
                        continue
                    result.append((1, names, state_string(rule["output_state"]), pred.get("probability", 1.0), group))
                    if "block_entity_modifier" in rule:
                        self.unsupported.add("processor block entity modifier")
            elif kind == "minecraft:protected_blocks":
                self.unsupported.add("protected_blocks processor")
            else:
                self.unsupported.add("processor: " + kind)
        return result

    def element(self, element):
        kind = element["element_type"]
        projection = 1 if element.get("projection", "rigid") == "rigid" else 2
        if kind == "minecraft:empty_pool_element":
            return "", projection, []
        if kind in ("minecraft:single_pool_element", "minecraft:legacy_single_pool_element"):
            key = self.load_template(element["location"], kind == "minecraft:legacy_single_pool_element")
            return key, projection, self.processors(element.get("processors", {"processors": []}))
        if kind == "minecraft:feature_pool_element":
            feature = element["feature"]
            if feature not in {"minecraft:oak", "minecraft:spruce", "minecraft:pine", "minecraft:acacia",
                               "minecraft:patch_cactus", "minecraft:patch_berry_bush", "minecraft:patch_taiga_grass", "minecraft:flower_plain", "minecraft:pile_hay",
                               "minecraft:pile_ice", "minecraft:pile_pumpkin", "minecraft:pile_melon", "minecraft:pile_snow"}:
                self.unsupported.add("feature pool algorithm: " + feature)
            key = "feature:" + feature
            self.templates[key] = ([1, 1, 1], [], [([0, 0, 0], 0, 3, "", "minecraft:empty", "", True, 0, 0, "minecraft:air")], feature)
            return key, projection, []
        if kind == "minecraft:list_pool_element":
            children = [self.element(child) for child in element["elements"]]
            if not children:
                raise ValueError("Empty list pool element")
            if any(child[2] != children[0][2] for child in children):
                raise ValueError("List pool element with differing processors is not supported")
            key = "list:" + "+".join(child[0] for child in children)
            templates = [self.templates[child[0]] for child in children]
            self.templates[key] = ([max(t[0][i] for t in templates) for i in range(3)],
                                   [block for t in templates for block in t[1]], templates[0][2], "")
            return key, projection, children[0][2]
        raise ValueError("Unsupported pool element: " + kind)

    def load_pool(self, name):
        if name in self.pools or not name or name == "minecraft:empty":
            return
        self.pools[name] = None  # connector graphs contain cycles
        data = self.resources.json("worldgen/template_pool", name)
        fallback = data["fallback"]
        fallback = "" if fallback == "minecraft:empty" else fallback
        elements = []
        for entry in data["elements"]:
            template, projection, processors = self.element(entry["element"])
            elements.append((template, entry["weight"], projection, processors))
        self.pools[name] = (fallback, elements)
        self.load_pool(fallback)

    def run(self):
        for name, variant in self.STARTS.items():
            data = self.resources.json("worldgen/structure", name)
            self.load_pool(data["start_pool"])
            self.starts.append((variant, data["start_pool"], data["size"], data["max_distance_from_center"],
                data["start_height"]["absolute"], "project_start_to_heightmap" in data,
                data.get("use_expansion_hack", False), data.get("start_jigsaw_name", "")))

    def write(self, path):
        with Path(path).open("wb") as out:
            def integer(value): out.write(struct.pack("<i", value))
            def text(value):
                data = value.encode("utf-8")
                integer(len(data)); out.write(data)
            def position(pos):
                for value in pos: integer(value)
            out.write(b"MCWTPL01")
            text("26.3")
            integer(len(self.unsupported))
            for value in sorted(self.unsupported): text(value)
            integer(len(self.states))
            for state, nbt in self.states: text(state); text(nbt)
            integer(len(self.templates))
            for name, (size, blocks, joints, feature) in sorted(self.templates.items()):
                text(name); position(size); text(feature)
                integer(len(blocks))
                for pos, state in blocks: position(pos); integer(state)
                integer(len(joints))
                for pos, front, top, name, target, pool, rollable, selection, priority, final in joints:
                    position(pos); integer(front); integer(top); text(name); text(target); text(pool)
                    integer(rollable); integer(selection); integer(priority); text(final)
            integer(len(self.pools))
            for name, (fallback, elements) in sorted(self.pools.items()):
                text(name); text(fallback); integer(len(elements))
                for template, weight, projection, processors in elements:
                    text(template); integer(weight); integer(projection); integer(len(processors))
                    for kind, names, output, chance, group in processors:
                        integer(kind); integer(len(names))
                        for name in names: text(name)
                        text(output); out.write(struct.pack("<f", chance)); integer(group)
            integer(len(self.starts))
            for variant, pool, depth, distance, y, project, expansion, anchor in self.starts:
                integer(variant); text(pool); integer(depth); integer(distance); integer(y)
                integer(project); integer(expansion); text(anchor)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="Local Minecraft 26.3 game JAR or extracted resource directory")
    parser.add_argument("output", help="Generated .mcwc file (use an ignored build directory)")
    args = parser.parse_args()
    resources = Resources(args.source)
    version = json.loads(resources.read("version.json"))
    if version["id"] != "26.3":
        raise ValueError("Expected Minecraft 26.3, got " + version["id"])
    importer = Importer(resources)
    importer.run()
    importer.write(args.output)
    print(f"Imported {len(importer.templates)} templates, {len(importer.pools)} pools, {len(importer.starts)} starts into {args.output}")
    print(f"Unsupported semantics recorded in catalog: {len(importer.unsupported)}")


if __name__ == "__main__":
    main()
