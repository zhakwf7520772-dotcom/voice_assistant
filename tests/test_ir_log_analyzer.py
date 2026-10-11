import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ir_log_analyzer", ROOT / "tools/ir_log_analyzer.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


class AnalyzerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.captures, cls.issues = analyzer.parse_log((ROOT / "data/ir/captures.log").read_text(encoding="utf-8"))
        cls.model = analyzer.timing_model(cls.captures)
        for capture in cls.captures:
            analyzer.decode_capture(capture, cls.model)
        cls.operations = analyzer.group_operations(cls.captures)
        cls.annotations = json.loads((ROOT / "data/ir/annotations.json").read_text())
        cls.annotation_issues = analyzer.annotate(cls.operations, cls.annotations)

    def test_sessions_missing_capture_and_partial_waveforms(self):
        self.assertEqual(len(self.captures), 73)
        self.assertEqual([sum(c["session"] == s for c in self.captures) for s in (1, 2, 3)], [17, 28, 28])
        self.assertTrue(any("15 -> 17" in issue for issue in self.issues))
        incomplete = [c for c in self.captures if not c["family_candidate"]]
        self.assertEqual([c["capture"] for c in incomplete], [13, 14, 15])

    def test_structure_does_not_pack_separator_or_eof_as_payload(self):
        first = self.captures[0]
        self.assertEqual(first["structure"], [35, 32])
        self.assertEqual(first["blocks"][0]["separator_bits"], "010")
        self.assertEqual(len(first["payload_bits"]), 64)
        self.assertEqual(first["hex"], "19 09 20 50 00 40 00 00")
        self.assertEqual(first["gap_symbols"][0]["symbol"], 36)
        self.assertEqual(len(self.operations[0]["payload_bits"]), 128)
        self.assertEqual(len(self.operations[0]["payload_hex"].split()), 16)

    def test_annotations_are_independent_and_all_fields_match(self):
        result = analyzer.verify_fields(self.operations)
        self.assertEqual(result["annotated_operations_checked"], 33)
        self.assertEqual(result["errors"], [])
        self.assertTrue(result["same_state_payload_consistent"])
        self.assertEqual(result["unique_annotated_states"], 9)
        self.assertEqual(self.operations[-1]["annotation"], None)
        self.assertTrue(self.annotation_issues)

    def test_fan_index_is_in_second_internal_block(self):
        fan_operations = [o for o in self.operations if o["session"] == 3 and o["annotation"]]
        for operation in fan_operations:
            second = operation["captures"][1]
            expected = operation["annotation"]["fan"]
            self.assertEqual(second["payload"][6] >> 4 & 7, expected)
            # First block bits 21..23 are constant; they are not the fan field.
            self.assertEqual(second["blocks"][0]["bits"][20:23], "010")
            self.assertEqual(int(second["blocks"][1]["bits"][20:23][::-1], 2), expected)

    def test_checksum_fit_and_corruption_detection(self):
        complete = [c for c in self.captures if c["family_candidate"] and not c["issues"]]
        self.assertEqual(len(complete), 70)
        self.assertTrue(all(c["checksum_valid"] for c in complete))
        fitting = [seed for seed in range(16) if all(
            analyzer.candidate_checksum(c["payload"], seed) == c["payload"][-1] >> 4 for c in complete)]
        self.assertEqual(fitting, [10])
        corrupted = complete[0]["payload"].copy()
        corrupted[1] ^= 1
        self.assertNotEqual(analyzer.candidate_checksum(corrupted), corrupted[-1] >> 4)

    def test_timing_not_print_times_and_sessions_never_join(self):
        for operation in self.operations:
            self.assertEqual(len({c["session"] for c in operation["captures"]}), 1)
            if analyzer.safe_operation(operation):
                self.assertTrue(39000 < operation["inter_capture_gaps_us"][0] < 41000)

    def test_truncated_raw_or_missing_end_cannot_be_replayed(self):
        first = self.captures[0]
        symbol = first["symbols"][0]
        log = ("[IR] Signal received; capture=1\n"
               "[IR] Receive time (completion, since boot): 100000us\n"
               "[IR] Symbols: 70\n"
               f"Symbol[0]: level0=0 duration0={symbol['duration0']}us level1=1 duration1={symbol['duration1']}us raw=0x00000000\n")
        captures, _ = analyzer.parse_log(log)
        self.assertIn("missing end capture", captures[0]["issues"])
        self.assertIn("symbol count mismatch", captures[0]["issues"])
        self.assertIn("raw mismatch at symbol 0", captures[0]["issues"])

    def test_wrong_annotation_is_reported_and_only_safe_labels_export(self):
        operations = copy.deepcopy(self.operations)
        operations[0]["annotation"]["fan"] = 5
        result = analyzer.verify_fields(operations)
        self.assertTrue(any(error["field"] == "fan" for error in result["errors"]))
        with tempfile.TemporaryDirectory() as folder:
            count = analyzer.export_catalog(Path(folder), self.operations)
            self.assertEqual(count, 9)
            source = (Path(folder) / "gree_raw_catalog.c").read_text()
            self.assertNotIn("operation 14;", source)  # Unlabelled last fan event.
            self.assertIn(".val = 0x", source)


if __name__ == "__main__":
    unittest.main()
