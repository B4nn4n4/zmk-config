"""Local routing regression tests. Requires PyYAML; no GitHub access needed."""

import fnmatch
from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[1]
BOARDS = {"hatsu", "zen", "tractyl"}


def load_yaml(path):
    # BaseLoader preserves the GitHub Actions key `on` instead of treating it as
    # a YAML 1.1 boolean, and keeps all scalar values as strings.
    return yaml.load((ROOT / path).read_text(), Loader=yaml.BaseLoader)


class BuildWorkflowsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.filters = load_yaml(".github/build-filters.yml")
        cls.workflow = load_yaml(".github/workflows/build-all.yml")

    def selected_boards(self, *paths):
        # These filters use only literals, * and ** (no picomatch extensions).
        matches = {
            group
            for group, patterns in self.filters.items()
            if any(fnmatch.fnmatchcase(path, pattern)
                   for path in paths for pattern in patterns)
        }
        return BOARDS if "shared" in matches else BOARDS & matches

    def test_board_only_changes(self):
        cases = {
            "hatsu": ["config/hatsu_left.keymap", "config/hatsu_right.conf",
                      "drivers/led/aw20216s.c", "src/battery_indicator.c",
                      "boards/angry_miao/hatsu_left/board.yml", "build-hatsu.yaml"],
            "zen": ["config/corneish_zen.keymap", "config/corneish_zen_left.conf",
                    "build-zen.yaml"],
            "tractyl": ["config/tracktyl.keymap", "config/tracktyl_left.keymap",
                        "config/tracktyl_right.conf", "build-tractyl.yaml",
                        "config/boards/shields/tracktyl/tracktyl.dtsi"],
        }
        for board, paths in cases.items():
            for path in paths:
                with self.subTest(path=path):
                    self.assertEqual(self.selected_boards(path), {board})

    def test_shared_changes(self):
        for path in ["config/layouts/alphas.h", "config/behaviors/homerow.dtsi",
                     "config/combos/umlauts.dtsi", "config/west.yml",
                     "src/charge_state.c", "src/usb_battery.c", "Kconfig", "CMakeLists.txt",
                     "zephyr/module.yml", "build.yaml", ".github/build-filters.yml",
                     ".github/workflows/build-zen.yml"]:
            with self.subTest(path=path):
                self.assertEqual(self.selected_boards(path), BOARDS)

    def test_mixed_shared_and_board_changes(self):
        self.assertEqual(self.selected_boards(
            "config/layouts/alphas.h", "config/hatsu_left.keymap",
            "config/corneish_zen.keymap", "config/tracktyl.keymap"), BOARDS)

    def test_multiple_boards_without_shared_changes(self):
        self.assertEqual(self.selected_boards(
            "config/hatsu_left.keymap", "config/tracktyl.keymap"),
            {"hatsu", "tractyl"})

    def test_unrelated_and_manual_only_inputs(self):
        for path in ["README.md", "docs/keymap-hatsu.svg", "host/example.sh",
                     "build-reset.yaml", "build-hatsu-debug.yaml"]:
            with self.subTest(path=path):
                self.assertEqual(self.selected_boards(path), set())

    def test_single_automatic_entry_point(self):
        automatic = []
        for path in (ROOT / ".github/workflows").glob("*.yml"):
            events = load_yaml(path.relative_to(ROOT))["on"]
            if "push" in events or "pull_request" in events:
                automatic.append(path.name)
        self.assertEqual(automatic, ["build-all.yml"])
        self.assertEqual(set(self.workflow["on"]),
                         {"push", "pull_request", "workflow_dispatch"})

    def test_exactly_one_job_per_board(self):
        jobs = self.workflow["jobs"]
        self.assertEqual(set(jobs), BOARDS | {"changes"})
        for board in BOARDS:
            with self.subTest(board=board):
                job = jobs[board]
                self.assertEqual(job["needs"], "changes")
                self.assertEqual(job["if"],
                                 "needs.changes.outputs.shared == 'true' || "
                                 f"needs.changes.outputs.{board} == 'true'")
                path = f".github/workflows/build-{board}.yml"
                self.assertEqual(job["uses"], f"./{path}")
                child = load_yaml(path)
                self.assertEqual(set(child["on"]),
                                 {"workflow_call", "workflow_dispatch"})
                self.assertEqual(child["jobs"]["build"]["with"]["build_matrix_path"],
                                 f"build-{board}.yaml")

    def test_manual_all_and_push_comparison(self):
        changes = self.workflow["jobs"]["changes"]
        self.assertEqual(changes["outputs"]["shared"],
                         "${{ github.event_name == 'workflow_dispatch' || "
                         "steps.filter.outputs.shared == 'true' }}")
        filter_step = next(step for step in changes["steps"]
                           if step.get("id") == "filter")
        self.assertEqual(filter_step["with"]["base"], "${{ github.ref }}")
        self.assertEqual(filter_step["with"]["filters"], ".github/build-filters.yml")


if __name__ == "__main__":
    unittest.main()
