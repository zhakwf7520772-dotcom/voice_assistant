#!/usr/bin/env python3
"""Parse RMT RX logs without treating headers, separators or EOF as data bits.

Python standard library only. The structural decoder discovers timing clusters
and block lengths; candidate state fields are evaluated afterwards, independently
against user annotations. No predicted state is used to label the input data.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
from statistics import median

CAPTURE_RE = re.compile(r"\[IR\] Signal received; capture=(\d+)")
SYMBOL_RE = re.compile(
    r"Symbol\[(\d+)\]: level0=(\d) duration0=(\d+)us "
    r"level1=(\d) duration1=(\d+)us(?: raw=0x([0-9a-fA-F]+))?"
)
TIME_RE = re.compile(r"since boot\): (\d+)us")
COUNT_RE = re.compile(r"\[IR\] Symbols: (\d+)")
END_RE = re.compile(r"Captured duration: (\d+)us; end capture=(\d+)")


def parse_log(text: str) -> tuple[list[dict], list[str]]:
    captures, issues = [], []
    current = None
    session, previous_id, previous_time = 0, None, None
    for line_number, line in enumerate(text.splitlines(), 1):
        match = CAPTURE_RE.search(line)
        if match:
            capture_id = int(match[1])
            if previous_id is None or capture_id <= previous_id:
                session += 1
                previous_time = None
            elif capture_id != previous_id + 1:
                issues.append(f"session {session}: capture IDs jump {previous_id} -> {capture_id}")
            previous_id = capture_id
            current = {"session": session, "capture": capture_id, "source_line": line_number,
                       "symbols": [], "issues": [], "ended": False}
            captures.append(current)
            continue
        if current is None:
            if SYMBOL_RE.search(line):
                issues.append(f"line {line_number}: symbol without capture header")
            continue
        if match := TIME_RE.search(line):
            timestamp = int(match[1])
            if previous_time is not None and timestamp < previous_time:
                session += 1
                current["session"] = session
                issues.append(f"line {line_number}: completion clock reset")
            current["completed_us"] = timestamp
            previous_time = timestamp
        if match := COUNT_RE.search(line):
            current["reported_count"] = int(match[1])
        if match := SYMBOL_RE.search(line):
            index, level0, duration0, level1, duration1 = map(int, match.groups()[:5])
            current["symbols"].append({"index": index, "level0": level0, "duration0": duration0,
                                       "level1": level1, "duration1": duration1,
                                       "raw": int(match[6], 16) if match[6] else None})
        if match := END_RE.search(line):
            current["reported_duration_us"] = int(match[1])
            current["ended"] = int(match[2]) == current["capture"]
        if "Buffer capacity reached" in line:
            current["issues"].append("buffer capacity reached: possible truncation")
        if "[IR][DIAG]" in line:
            # Preserve diagnostics without including Wi-Fi or other private logs.
            current["last_diagnostic"] = line[line.index("[IR][DIAG]"):]

    for capture in captures:
        symbols = capture["symbols"]
        if not capture["ended"]:
            capture["issues"].append("missing end capture")
        if [s["index"] for s in symbols] != list(range(len(symbols))):
            capture["issues"].append("missing, duplicate or out-of-order symbol indices")
        if capture.get("reported_count") != len(symbols):
            capture["issues"].append("symbol count mismatch")
        capture["duration_us"] = sum(s["duration0"] + s["duration1"] for s in symbols)
        if capture.get("reported_duration_us") != capture["duration_us"]:
            capture["issues"].append("duration sum mismatch")
        if "completed_us" not in capture:
            capture["issues"].append("missing completion timestamp")
        for symbol in symbols:
            if symbol["level0"] not in (0, 1) or symbol["level1"] not in (0, 1):
                capture["issues"].append("invalid logic level")
            if not (0 <= symbol["duration0"] <= 32767 and 0 <= symbol["duration1"] <= 32767):
                capture["issues"].append("duration outside 15-bit RMT range")
            expected = (symbol["duration0"] | symbol["level0"] << 15 |
                        symbol["duration1"] << 16 | symbol["level1"] << 31)
            if symbol["raw"] is not None and symbol["raw"] != expected:
                capture["issues"].append(f"raw mismatch at symbol {symbol['index']}")
        capture["issues"] = sorted(set(capture["issues"]))
    return captures, issues


def timing_model(captures: list[dict]) -> dict:
    # Separate data-space populations from multi-millisecond header/gap spaces.
    spaces = [s["duration1"] for c in captures for s in c["symbols"]
              if s["level0"] == 0 and s["level1"] == 1 and 150 <= s["duration1"] < 3000]
    if len(spaces) < 2:
        raise ValueError("not enough pulse spaces to infer timing")
    centers = [float(min(spaces)), float(max(spaces))]
    for _ in range(20):
        groups = [[x for x in spaces if abs(x - centers[i]) <= abs(x - centers[1 - i])]
                  for i in range(2)]
        if any(not group for group in groups):
            raise ValueError("only one timing population; cannot infer binary encoding")
        updated = [float(median(group)) for group in groups]
        if updated == centers:
            break
        centers = updated
    short, long = sorted(centers)
    if long < short * 1.8:
        raise ValueError("short/long timing populations are not well separated")
    return {"short_space_us": short, "long_space_us": long,
            "threshold_us": (short + long) / 2, "gap_threshold_us": 10000}


def bits_to_bytes(bits: str) -> list[int]:
    if len(bits) % 8:
        raise ValueError("byte packing requires whole bytes")
    return [sum(int(bits[offset + bit]) << bit for bit in range(8))
            for offset in range(0, len(bits), 8)]


def candidate_checksum(payload: list[int], seed: int = 10) -> int:
    """Kelvinator-family block candidate, cross-checked against decoded data.

    The final byte's LOW nibble is excluded by this candidate. It is zero in
    this dataset, so these observations cannot prove whether it contributes.
    """
    if len(payload) != 8:
        raise ValueError("checksum candidate requires an eight-byte block")
    return (seed + sum(value & 15 for value in payload[:4]) +
            sum(value >> 4 for value in payload[4:7])) & 15


def decode_capture(capture: dict, model: dict) -> None:
    capture["blocks"] = []
    capture["header"] = None
    capture["gap_symbols"] = []
    current_bits, current_indices = [], []

    def flush() -> None:
        if current_bits:
            capture["blocks"].append({"bits": "".join(current_bits),
                                      "symbol_indices": current_indices.copy()})
            current_bits.clear()
            current_indices.clear()

    for symbol in capture["symbols"]:
        i, mark, space = symbol["index"], symbol["duration0"], symbol["duration1"]
        if symbol["level0"] != 0 or symbol["level1"] != 1:
            flush()
            capture["issues"].append(f"unexpected polarity at symbol {i}")
            continue
        if mark == 0 or space == 0:
            flush()  # EOF/terminal mark is retained in raw data, not decoded as a bit.
            if i != len(capture["symbols"]) - 1:
                capture["issues"].append(f"zero-duration marker inside capture at symbol {i}")
            continue
        if mark >= 4000 and space >= 2000:
            flush()
            if i != 0 or capture["header"] is not None:
                capture["issues"].append(f"unexpected header position {i}")
            capture["header"] = {"symbol": i, "mark_us": mark, "space_us": space}
            continue
        if space >= model["gap_threshold_us"]:
            flush()
            capture["gap_symbols"].append({"symbol": i, "mark_us": mark, "space_us": space})
            continue
        bit = "1" if space > model["threshold_us"] else "0"
        center = model["long_space_us"] if bit == "1" else model["short_space_us"]
        if not (150 <= mark <= 1200 and center * 0.5 <= space <= center * 1.5):
            capture["issues"].append(f"unclassified timing at symbol {i}")
        current_bits.append(bit)
        current_indices.append(i)
    flush()

    # Byte-aligned prefix + explicitly retained non-byte-aligned separator.
    # This uses measured block lengths, not fixed symbol offsets.
    payload, separators = [], []
    for block in capture["blocks"]:
        full_bits = len(block["bits"]) // 8 * 8
        block["payload_bits"] = block["bits"][:full_bits]
        block["separator_bits"] = block["bits"][full_bits:]
        block["bytes_lsb_first"] = bits_to_bytes(block["payload_bits"])
        payload.extend(block["bytes_lsb_first"])
        separators.append(block["separator_bits"])
    capture["payload"] = payload
    capture["payload_bits"] = "".join(b["payload_bits"] for b in capture["blocks"])
    capture["hex"] = " ".join(f"{value:02X}" for value in payload)
    capture["structure"] = [len(b["bits"]) for b in capture["blocks"]]
    # The following checks apply to the observed two-block family only. Noise or
    # another protocol is kept in the report and cannot enter the replay catalog.
    capture["family_candidate"] = bool(
        capture["header"] and capture["structure"] == [35, 32] and
        separators == ["010", ""] and len(payload) == 8 and
        len(capture["gap_symbols"]) == 1
    )
    capture["checksum_valid"] = (candidate_checksum(payload) == payload[-1] >> 4
                                  if capture["family_candidate"] else None)
    capture["issues"] = sorted(set(capture["issues"]))


def group_operations(captures: list[dict], max_gap_us: int = 80000) -> list[dict]:
    operations = []
    for capture in captures:
        gap = None
        if operations:
            previous = operations[-1]["captures"][-1]
            if previous["session"] == capture["session"]:
                gap = (capture.get("completed_us", 0) - previous.get("completed_us", 0) -
                       capture["duration_us"])
        # Group by measured signal-end to next signal-start gap. Completion
        # latency cancels only for captures with the same RX idle threshold.
        join = bool(operations and gap is not None and 0 <= gap <= max_gap_us and
                    capture["capture"] == operations[-1]["captures"][-1]["capture"] + 1)
        if not join:
            session = capture["session"]
            index = 1 + sum(o["session"] == session for o in operations)
            operations.append({"session": session, "operation": index, "captures": [],
                               "inter_capture_gaps_us": [], "annotation": None})
        elif gap is not None:
            operations[-1]["inter_capture_gaps_us"].append(gap)
        operations[-1]["captures"].append(capture)
    for operation in operations:
        complete = all(c["family_candidate"] and not c["issues"] for c in operation["captures"])
        operation["payload_bits"] = ("".join(c["payload_bits"] for c in operation["captures"])
                                     if complete else None)
        operation["payload_hex"] = (" ".join(c["hex"] for c in operation["captures"])
                                    if complete else None)
    return operations


def annotate(operations: list[dict], annotations: dict) -> list[str]:
    issues = []
    for session in annotations.get("sessions", []):
        matching = [o for o in operations if o["session"] == session["session"]]
        states = session["operations"]
        if len(matching) != len(states):
            issues.append(f"session {session['session']}: {len(matching)} operations, "
                          f"{len(states)} annotations; excess operations remain unlabelled")
        for operation, state in zip(matching, states):
            operation["annotation"] = state
    return issues


def safe_operation(operation: dict) -> bool:
    captures = operation["captures"]
    return (len(captures) == 2 and len(operation["inter_capture_gaps_us"]) == 1 and
            all(not c["issues"] and c["family_candidate"] and c["checksum_valid"] for c in captures) and
            0 < operation["inter_capture_gaps_us"][0] <= 200000)


def verify_fields(operations: list[dict]) -> dict:
    errors, checked, repeats = [], 0, defaultdict(set)
    for operation in operations:
        state = operation["annotation"]
        if not state or not safe_operation(operation):
            continue
        a, b = [c["payload"] for c in operation["captures"]]
        checks = {"power": bool(a[0] & 8) == state["power"] == bool(b[0] & 8),
                  "temperature": (a[1] & 15) + 16 == state["temp"] == (b[1] & 15) + 16,
                  "fan": (b[6] >> 4) & 7 == state["fan"] and
                         ((a[0] >> 4) & 3) == min(state["fan"], 3) == ((b[0] >> 4) & 3),
                  "mode_constant": (a[0] & 7) == (b[0] & 7) == state["mode_code"]}
        checked += 1
        for field, ok in checks.items():
            if not ok:
                errors.append({"session": operation["session"], "operation": operation["operation"],
                               "field": field})
        key = json.dumps(state, sort_keys=True)
        repeats[key].add(tuple(a + b))
    return {"annotated_operations_checked": checked, "errors": errors,
            "same_state_payload_consistent": all(len(values) == 1 for values in repeats.values()),
            "unique_annotated_states": len(repeats)}


def difference_positions(a: list[int], b: list[int]) -> list[int]:
    return [byte * 8 + bit + 1 for byte, (x, y) in enumerate(zip(a, b))
            for bit in range(8) if (x ^ y) & (1 << bit)]


def write_report(path: Path, captures: list[dict], operations: list[dict], model: dict,
                 issues: list[str], verification: dict) -> None:
    complete = [c for c in captures if not c["issues"] and c["family_candidate"]]
    seeds = [seed for seed in range(16) if complete and
             all(candidate_checksum(c["payload"], seed) == c["payload"][-1] >> 4 for c in complete)]
    lines = ["# IR log analysis", "", f"Captures: {len(captures)}; sessions: {len({c['session'] for c in captures})}.",
             f"Observed valid 8-byte captures: {len(complete)}. Count distribution: {dict(Counter(c['reported_count'] for c in captures))}.",
             f"Inferred short/long spaces: {model['short_space_us']:g}/{model['long_space_us']:g} us; decision threshold: {model['threshold_us']:g} us.",
             "", "## Measured structure", "",
             "Each recognized capture contains a header, a 35-bit timing block, an internal gap, a 32-bit timing block, and a terminal mark.",
             "The 35-bit block separates into 32 payload bits and a 3-bit separator `010`. Each capture therefore has 64 payload bits (8 bytes), not 70 data bits.",
             "Bytes below use LSB-first packing; printed bit strings retain transmission order. RMT zero-based symbol indices and one-based payload bit indices are different.",
             "Two adjacent captures form one observed 16-byte operation. They carry different payloads.",
             "", "## Field validation against annotations", "", json.dumps(verification, ensure_ascii=False), "",
             "- Power candidate: byte 0 bit 3 (payload bit 4; RMT Symbol[4]).",
             "- Temperature candidate: byte 1 low nibble + 16 (payload bits 9..12); only 25..27 C tested.",
             "- Basic fan candidate: byte 0 bits 4..5 (payload bits 5..6), equal to min(fan, 3). Bit 4 of the transmission is POWER, not fan.",
             "- Extended fan candidate: second capture byte 6 bits 4..6 (payload bits 53..55; RMT Symbols[57..59]; second internal block bits 21..23).",
             "- Mode byte 0 bits 0..2 remain 1 in annotated samples. Cooling was held fixed, so other mode mappings are unverified.",
             "", "## Checksum candidate", "",
             "For each 8-byte capture: `C = (seed + sum(B[0..3] & 0xF) + sum(B[4..6] >> 4)) & 0xF`; compare with `B[7] >> 4`.",
             f"Seeds fitting all structurally valid captures: {seeds}. Checksum passes: {sum(c['checksum_valid'] is True for c in complete)}/{len(complete)}.",
             "The last byte's low nibble is always zero here. These samples cannot establish its role, timer behavior, other modes, or every temperature/fan combination.",
             "This candidate was cross-checked with the primary IRremoteESP8266 Kelvinator implementation after extracting raw data: [checksum and framing](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/src/ir_Kelvinator.cpp), [field layout](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/src/ir_Kelvinator.h).",
             "The observed two-capture / five-speed structure matches that protocol family; the remote model has not been established.",
             "", "## Operations", "", "| Session/op | Captures | Annotation | Gap us | First / second bytes |", "|---|---|---|---|---|"]
    for operation in operations:
        names = ",".join(str(c["capture"]) for c in operation["captures"])
        state = operation["annotation"]
        label = (f"power={state['power']} temp={state['temp']} fan={state['fan']}" if state else "unlabelled")
        payloads = " / ".join(c["hex"] or "undecodable" for c in operation["captures"])
        lines.append(f"| {operation['session']}/{operation['operation']} | {names} | {label} | {operation['inter_capture_gaps_us']} | {payloads} |")
    lines += ["", "## Observed fixed bits by capture phase", "",
              "These masks describe this dataset, not every mode or remote function. A 1 in the mask means the bit did not change.",
              "", "| Phase | Byte (zero-based) | Fixed mask | Fixed value |", "|---|---|---|---|"]
    for phase in range(2):
        payloads = [o["captures"][phase]["payload"] for o in operations if safe_operation(o)]
        for byte in range(8):
            values = [p[byte] for p in payloads]
            both, either = 255, 0
            for value in values:
                both &= value
                either |= value
            mask = 255 ^ (both ^ either)
            lines.append(f"| {phase + 1} | {byte} | {mask:02X} | {both & mask:02X} |")
    lines += ["", "## Full capture data", ""]
    for capture in captures:
        lines += [f"### Session {capture['session']} capture {capture['capture']}", "",
                  f"Symbols: {capture['reported_count']}; duration: {capture['duration_us']} us; structure: {capture['structure']}; checksum: {capture['checksum_valid']}.",
                  f"Payload: `{capture['hex']}`.", f"Payload transmission bits: `{capture['payload_bits']}`.",
                  f"Blocks (including separators): `{' | '.join(b['bits'] for b in capture['blocks'])}`.",
                  f"Issues: {capture['issues']}.", ""]
    lines += ["## Consecutive operation differences (one-based payload bits)", ""]
    for previous, current in zip(operations, operations[1:]):
        if previous["session"] == current["session"] and safe_operation(previous) and safe_operation(current):
            differences = [difference_positions(a["payload"], b["payload"])
                           for a, b in zip(previous["captures"], current["captures"])]
            lines.append(f"- Session {current['session']}, op {previous['operation']} -> {current['operation']}: {differences}")
    lines += ["", "## Input issues", ""] + [f"- {issue}" for issue in issues]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def export_catalog(directory: Path, operations: list[dict]) -> int:
    selected = {}
    for operation in operations:
        state = operation["annotation"]
        if state and safe_operation(operation):
            key = (state["power"], state["temp"], state["fan"], state["mode_code"])
            selected.setdefault(key, operation)
    if not selected:
        raise ValueError("no complete, checksum-valid annotated operations available for replay")
    directory.mkdir(parents=True, exist_ok=True)
    header = '''// Generated by tools/ir_log_analyzer.py. Captured data, not synthesized state.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ir_transmitter.h"

typedef struct {
    bool power;
    uint8_t temperature_c;
    uint8_t fan;
    uint8_t mode_code;
    const ir_raw_command_t *command;
} gree_raw_entry_t;

extern const gree_raw_entry_t gree_raw_catalog[];
extern const size_t gree_raw_catalog_count;
'''
    source = ['// Generated from complete annotated captures; do not edit by hand.',
              '#include "gree_raw_catalog.h"', '']
    entries = []
    for number, (key, operation) in enumerate(sorted(selected.items())):
        power, temp, fan, mode = key
        source.append(f"// session {operation['session']}, operation {operation['operation']}; power={power}, temp={temp}, fan={fan}")
        for phase, capture in enumerate(operation["captures"]):
            source.append(f"static const rmt_symbol_word_t raw_{number}_{phase}[] = {{")
            for symbol in capture["symbols"]:
                word = (symbol["duration0"] | symbol["level0"] << 15 |
                        symbol["duration1"] << 16 | symbol["level1"] << 31)
                source.append(f"    {{ .val = 0x{word:08x}U }},")
            source.append("};")
        source.append(f"static const ir_raw_frame_t frames_{number}[] = {{")
        for phase, capture in enumerate(operation["captures"]):
            gap = operation["inter_capture_gaps_us"][phase] if phase < len(operation["captures"]) - 1 else 0
            source.append(f"    {{ raw_{number}_{phase}, {len(capture['symbols'])}, {gap}U }},")
        source += ["};", f"static const ir_raw_command_t command_{number} = {{ frames_{number}, 2 }};", ""]
        entries.append(f"    {{ {'true' if power else 'false'}, {temp}, {fan}, {mode}, &command_{number} }},")
    source += ["const gree_raw_entry_t gree_raw_catalog[] = {", *entries, "};",
               "const size_t gree_raw_catalog_count = sizeof(gree_raw_catalog) / sizeof(gree_raw_catalog[0]);", ""]
    (directory / "gree_raw_catalog.h").write_text(header, encoding="utf-8")
    (directory / "gree_raw_catalog.c").write_text("\n".join(source), encoding="utf-8")
    return len(selected)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--annotations", type=Path)
    parser.add_argument("--out", type=Path, default=Path("ir_analysis"), help="output filename prefix")
    parser.add_argument("--group-gap-us", type=int, default=80000)
    parser.add_argument("--export-c", type=Path,
                        help="catalog output directory; use main/ir/gree for this project")
    args = parser.parse_args()
    text = args.log.read_text(encoding="utf-8-sig", errors="replace")
    captures, issues = parse_log(text)
    try:
        model = timing_model(captures)
    except ValueError as error:
        parser.error(str(error))
    for capture in captures:
        decode_capture(capture, model)
    operations = group_operations(captures, args.group_gap_us)
    annotations = json.loads(args.annotations.read_text(encoding="utf-8-sig")) if args.annotations else {}
    issues += annotate(operations, annotations)
    verification = verify_fields(operations)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    result = {"source_sha256": hashlib.sha256(text.encode()).hexdigest(), "timing_model": model,
              "issues": issues, "field_verification": verification, "operations": operations}
    args.out.with_suffix(".json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_report(args.out.with_suffix(".md"), captures, operations, model, issues, verification)
    if args.export_c:
        if verification["errors"] or not verification["same_state_payload_consistent"]:
            parser.error("annotated state validation failed; refusing to export replay catalog")
        count = export_catalog(args.export_c, operations)
        print(f"Exported {count} measured state combinations")
    print(f"Parsed {len(captures)} captures / {len(operations)} operations / {len({c['session'] for c in captures})} sessions")
    print(f"Annotated operations checked: {verification['annotated_operations_checked']}; errors: {len(verification['errors'])}")
    print(f"Reports: {args.out.with_suffix('.md')} and {args.out.with_suffix('.json')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
