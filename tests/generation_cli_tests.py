"""Exercise the headless CLI and its exported data using an independent NBT reader."""
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("import_worldgen", Path(__file__).parents[1] / "tools/import_worldgen.py")
importer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(importer)
executable = sys.argv.pop(1)


class CliTests(unittest.TestCase):
    def run_cli(self, *args, success=True):
        result = subprocess.run([executable, *map(str, args)], capture_output=True, text=True, timeout=120)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("terrain_gen:", result.stderr)
        return result

    def test_help_and_validation(self):
        self.assertIn("--export", self.run_cli("--help").stdout)
        for args in [
            ["--chunks", "0"], ["--chunks", "17"], ["--threads", "0"], ["--threads", "65"],
            ["--threads", "-1"], ["--chunk-x", "2147483647"], ["--chunk-z", "-2147483648"],
            ["--seed", "9223372036854775808"], ["--seed", "1junk"], ["--seed"],
            ["--export", ""], ["--voxel"], ["--headless"], ["--unknown", "1"],
        ]:
            with self.subTest(args=args):
                self.run_cli(*args, success=False)

    def test_exports(self):
        with tempfile.TemporaryDirectory(prefix="mcworld-cli-") as directory:
            for terrain_only in (True, False):
                args = ["--seed", "12345", "--chunk-x", "-1", "--chunk-z", "2", "--chunks", "2",
                        "--templates", ""]
                if terrain_only:
                    args += ["--terrain-only"]
                first = Path(directory) / "first export.schem"
                second = Path(directory) / "second.schem"
                output = self.run_cli(*args, "--threads", 1, "--export", first).stdout
                self.run_cli(*args, "--threads", 2, "--export", second)
                self.assertEqual(first.read_bytes(), second.read_bytes(), "worker count must not change exports")
                self.assertIn("output_chunks=4", output)
                if terrain_only:
                    self.assertIn("terrain_chunks=4 decoration_chunks=0", output)
                self.assertNotIn("mesh_triangles=", output)
                self.assertNotIn("voxel_faces=", output)
                nbt = importer.NbtReader(first.read_bytes()).root()
                self.assertEqual(nbt["Version"], 2)
                self.assertEqual((nbt["Width"], nbt["Length"]), (32, 32))
                self.assertEqual(nbt["Offset"], [-32, -64, 16])
                self.assertGreater(nbt["Height"], 0)
                self.assertLessEqual(nbt["Height"], 384)
                palette = {value: name for name, value in nbt["Palette"].items()}
                counts = {"solid": 0, "water": 0, "lava": 0, "air": 0}
                value = shift = 0
                for signed_byte in nbt["BlockData"]:
                    byte = signed_byte & 255
                    value |= (byte & 127) << shift
                    if byte & 128:
                        shift += 7
                        self.assertLess(shift, 35)
                        continue
                    state = palette[value]
                    kind = state.removeprefix("minecraft:")
                    counts[kind if kind in counts else "solid"] += 1
                    value = shift = 0
                self.assertEqual(shift, 0)
                self.assertEqual(sum(counts.values()), 32 * nbt["Height"] * 32)
                stats = re.search(r"solid_blocks=(\d+) water_blocks=(\d+) lava_blocks=(\d+)", output)
                self.assertIsNotNone(stats)
                self.assertEqual(tuple(map(int, stats.groups())),
                                 (counts["solid"], counts["water"], counts["lava"]))

    def test_statistics_and_io_errors(self):
        args = ["--chunks", "1", "--terrain-only"]
        result = self.run_cli(*args, "--templates", "/nonexistent/ignored.mcwc")
        self.assertIn("solid_blocks=", result.stdout)
        self.assertNotIn("exported_schematic=", result.stdout)
        with tempfile.TemporaryDirectory(prefix="mcworld-cli-") as directory:
            self.run_cli(*args, "--export", Path(directory) / "missing" / "area.schem", success=False)
            self.run_cli("--chunks", "1", "--templates", Path(directory) / "missing.mcwc", success=False)


if __name__ == "__main__":
    unittest.main()
