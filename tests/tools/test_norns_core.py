"""Runs the Lua host test of the norns wingie_monitor params (tests/norns/)."""

import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LUA = shutil.which("lua5.3") or shutil.which("lua5.4") or shutil.which("lua")


@unittest.skipIf(LUA is None, "lua is not installed")
class NornsCoreParamsTest(unittest.TestCase):
    def test_core_params(self):
        result = subprocess.run([LUA, "tests/norns/core_params_test.lua"], cwd=ROOT,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("core_params_test passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
