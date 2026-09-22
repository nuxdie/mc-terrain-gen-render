import gzip
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("import_worldgen", Path(__file__).parents[1] / "tools/import_worldgen.py")
importer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(importer)
executable = sys.argv.pop(1) if len(sys.argv) > 1 else None


def string(value):
    encoded = value.encode("utf-8")
    return struct.pack(">H", len(encoded)) + encoded


def tag(kind, name, data):
    return bytes([kind]) + string(name) + data


def compound(*values):
    return b"".join(values) + b"\0"


def integer_list(values):
    return b"\x03" + struct.pack(">i", len(values)) + b"".join(struct.pack(">i", x) for x in values)


def template():
    palette = compound(tag(8, "id", string("minecraft:oak_stairs")),
                       tag(10, "properties", compound(tag(8, "facing", string("north")), tag(8, "half", string("bottom")))))
    entity = compound(tag(8, "id", string("minecraft:chest")), tag(8, "LootTable", string("minecraft:chests/village/village_plains_house")),
                      tag(4, "LootTableSeed", struct.pack(">q", -912345678901)))
    block = compound(tag(9, "pos", integer_list([1, 0, 1])), tag(3, "state", struct.pack(">i", 0)), tag(10, "nbt", entity))
    return tag(10, "", compound(tag(9, "size", integer_list([3, 4, 3])),
                               tag(9, "palette", b"\x0a" + struct.pack(">i", 1) + palette),
                               tag(9, "blocks", b"\x0a" + struct.pack(">i", 1) + block),
                               tag(9, "entities", b"\x0a\0\0\0\0")))


class ImportTests(unittest.TestCase):
    def test_nbt_compressed_and_raw(self):
        raw = template()
        self.assertEqual(importer.NbtReader(raw).root(), importer.NbtReader(gzip.compress(raw)).root())
        parsed = importer.NbtReader(raw).root()
        self.assertEqual(parsed["blocks"][0]["nbt"]["LootTableSeed"], -912345678901)
        self.assertEqual(importer.state_string(parsed["palette"][0]), "minecraft:oak_stairs[facing=north,half=bottom]")

    def test_nbt_invalid(self):
        with self.assertRaises(ValueError):
            importer.NbtReader(template()[:-1]).root()
        with self.assertRaises(ValueError):
            importer.NbtReader(template() + b"bad").root()
        with self.assertRaises(ValueError):
            importer.NbtReader(tag(10, "", compound(tag(9, "bad", b"\x03\xff\xff\xff\xff")))).root()

    def test_real_format_roundtrip(self):
        with tempfile.TemporaryDirectory(prefix="mcworld-import-") as directory:
            source = Path(directory) / "game.jar"
            out = Path(directory) / "test.mcwc"
            config = {"start_pool": "minecraft:test/start", "size": 0, "max_distance_from_center": 32,
                      "start_height": {"absolute": 80}, "use_expansion_hack": True}
            pool = {"fallback": "minecraft:empty", "elements": [{"weight": 1, "element": {
                "element_type": "minecraft:single_pool_element", "location": "minecraft:test/house",
                "projection": "rigid", "processors": {"processors": []}}}]}
            with zipfile.ZipFile(source, "w") as archive:
                archive.writestr("data/minecraft/worldgen/structure/village_plains.json", json.dumps(config))
                archive.writestr("data/minecraft/worldgen/template_pool/test/start.json", json.dumps(pool))
                archive.writestr("data/minecraft/structure/test/house.nbt", gzip.compress(template()))
            catalog = importer.Importer(importer.Resources(source))
            catalog.STARTS = {"village_plains": 1}
            catalog.run()
            catalog.write(out)
            self.assertEqual(len(catalog.templates), 1)
            self.assertEqual(catalog.states[0][0], "minecraft:oak_stairs[facing=north,half=bottom]")
            self.assertIn("LootTable", catalog.states[0][1])
            if executable:
                subprocess.run([executable, "--catalog-fixture", str(out)], check=True)
                broken = Path(directory) / "broken.mcwc"
                broken.write_bytes(out.read_bytes()[:-8])
                subprocess.run([executable, "--bad-catalog", str(broken)], check=True)


if __name__ == "__main__":
    unittest.main()
