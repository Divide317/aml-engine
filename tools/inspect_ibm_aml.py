#!/usr/bin/env python3
"""
Inspect an IBM AML transactions file (e.g. HI-Small_Trans.csv) and, optionally,
its *_Patterns.txt, and report every fact the C++ CSR design depends on.

Stdlib only, single streaming pass over the CSV. HI-Small (~5M rows) needs
roughly 1-2 GB RAM with --pairs on (the default); pass --no-pairs to skip the
multi-edge count if memory is tight.

Usage:
    python3 tools/inspect_ibm_aml.py data/raw/HI-Small_Trans.csv \
        --patterns data/raw/HI-Small_Patterns.txt --json reports/hi_small_schema.json
"""
from __future__ import annotations

import argparse
import csv
import json
import random
import re
import statistics
import sys
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone

# Positional schema. The raw header repeats "Account"; pandas renames the second
# one to "Account.1", which is why notebooks show that name. Parse by position.
EXPECTED_HEADER = [
    "Timestamp", "From Bank", "Account", "To Bank", "Account",
    "Amount Received", "Receiving Currency", "Amount Paid",
    "Payment Currency", "Payment Format", "Is Laundering",
]
(TS, FROM_BANK, FROM_ACCT, TO_BANK, TO_ACCT, AMT_RECV, RECV_CCY,
 AMT_PAID, PAY_CCY, FMT, LABEL) = range(11)

TS_FORMAT = "%Y/%m/%d %H:%M"
HEX_RE = re.compile(r"^[0-9A-Fa-f]+$")
RESERVOIR = 2000  # samples kept per currency pair for exchange-rate medians


def ts_to_min(s: str) -> int:
    """Minutes since Unix epoch, treating the naive timestamp as UTC."""
    dt = datetime.strptime(s, TS_FORMAT).replace(tzinfo=timezone.utc)
    return int(dt.timestamp()) // 60


def min_to_str(m: int) -> str:
    return datetime.fromtimestamp(m * 60, tz=timezone.utc).strftime(TS_FORMAT)


def pct(sorted_vals, q):
    if not sorted_vals:
        return None
    i = min(len(sorted_vals) - 1, max(0, int(round(q * (len(sorted_vals) - 1)))))
    return sorted_vals[i]


def summarize(vals):
    if not vals:
        return {}
    s = sorted(vals)
    return {"n": len(s), "min": s[0], "p50": pct(s, .5), "p90": pct(s, .9),
            "p95": pct(s, .95), "p99": pct(s, .99), "max": s[-1]}


def decimals(s: str) -> int:
    return len(s.split(".", 1)[1]) if "." in s else 0


def tx_key(row):
    """Join key between Trans.csv and Patterns.txt rows (bank IDs normalised to int)."""
    return (row[TS].strip(), int(row[FROM_BANK]), row[FROM_ACCT].strip().upper(),
            int(row[TO_BANK]), row[TO_ACCT].strip().upper(),
            round(float(row[AMT_PAID]), 2), row[PAY_CCY].strip())


# --------------------------------------------------------------------------- #
# Patterns.txt
# --------------------------------------------------------------------------- #
def parse_patterns(path):
    """Blocks look like:
         BEGIN LAUNDERING ATTEMPT - FAN-OUT:  Max 16-degree Fan-Out
         <csv row in Trans.csv column order>
         ...
         END LAUNDERING ATTEMPT - FAN-OUT
    The parser is tolerant: any non-BEGIN/END line with >= 10 CSV fields
    inside a block is treated as a transaction."""
    blocks, cur, padded_banks, stray = [], None, 0, 0
    with open(path, newline="") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            up = line.upper()
            if up.startswith("BEGIN LAUNDERING ATTEMPT"):
                rest = line.split("-", 1)[1] if "-" in line else ""
                typ, _, detail = rest.partition(":")
                cur = {"type": typ.strip().upper(), "detail": detail.strip(), "rows": []}
                continue
            if up.startswith("END LAUNDERING ATTEMPT"):
                if cur is not None:
                    blocks.append(cur)
                cur = None
                continue
            row = next(csv.reader([line]))
            if len(row) < 10:
                stray += 1
                continue
            if row[FROM_BANK].startswith("0") or row[TO_BANK].startswith("0"):
                padded_banks += 1
            if cur is None:
                stray += 1
            else:
                cur["rows"].append(row)
    return blocks, padded_banks, stray


def pattern_report(blocks):
    by_type = defaultdict(lambda: {"blocks": 0, "tx": [], "accounts": [], "span_min": [],
                                   "same_minute_hops": 0, "hops": 0})
    keys = Counter()
    for b in blocks:
        r = by_type[b["type"]]
        r["blocks"] += 1
        rows = b["rows"]
        r["tx"].append(len(rows))
        accts = set()
        times = []
        for row in rows:
            accts.add((int(row[FROM_BANK]), row[FROM_ACCT].upper()))
            accts.add((int(row[TO_BANK]), row[TO_ACCT].upper()))
            times.append(ts_to_min(row[TS].strip()))
            keys[tx_key(row)] += 1
        r["accounts"].append(len(accts))
        if times:
            r["span_min"].append(max(times) - min(times))
            ts_sorted = sorted(times)
            r["hops"] += max(0, len(ts_sorted) - 1)
            r["same_minute_hops"] += sum(1 for a, c in zip(ts_sorted, ts_sorted[1:]) if a == c)
    out = {}
    for typ, r in sorted(by_type.items()):
        out[typ] = {
            "blocks": r["blocks"],
            "tx_per_block": summarize(r["tx"]),
            "accounts_per_block": summarize(r["accounts"]),
            "time_span_minutes": summarize(r["span_min"]),
            "same_minute_fraction": (r["same_minute_hops"] / r["hops"]) if r["hops"] else None,
        }
    return out, keys


# --------------------------------------------------------------------------- #
# Main pass over Trans.csv
# --------------------------------------------------------------------------- #
def inspect(path, pattern_keys=None, want_pairs=True, max_rows=None):
    t0 = time.time()
    node_id = {}                 # (bank, acct) -> dense id
    out_deg, in_deg = [], []
    hex_to_banks = defaultdict(set)
    acct_len = Counter()
    non_hex = 0
    max_bank = 0
    max_acct_bits = 0
    pairs = set() if want_pairs else None

    n = labels = self_loops = self_loop_labels = inversions = 0
    prev_t = None
    t_min = t_max = None
    per_day = Counter()
    per_minute = Counter()
    fmt_count, fmt_label = Counter(), Counter()
    pay_ccy, recv_ccy = Counter(), Counter()
    self_loop_fmt = Counter()
    cross_ccy = 0
    same_ccy_amount_mismatch = 0
    rate_samples = defaultdict(list)
    rate_seen = Counter()
    amt_max = 0.0
    amt_min = float("inf")
    max_decimals = 0
    over_f32_exact = 0           # amounts >= 2^24 / 100: float32 can't hold cents
    matched_pattern_rows = 0
    laundering_in_patterns = 0
    pk = Counter(pattern_keys) if pattern_keys else None
    rng = random.Random(0)

    def intern(bank, acct):
        nonlocal max_acct_bits, non_hex
        key = (bank, acct)
        i = node_id.get(key)
        if i is None:
            i = len(node_id)
            node_id[key] = i
            out_deg.append(0)
            in_deg.append(0)
            acct_len[len(acct)] += 1
            hex_to_banks[acct].add(bank)
            if HEX_RE.match(acct):
                max_acct_bits = max(max_acct_bits, int(acct, 16).bit_length())
            else:
                non_hex += 1
        return i

    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        header_ok = [h.strip() for h in header] == EXPECTED_HEADER
        for row in reader:
            if max_rows and n >= max_rows:
                break
            if len(row) != 11:
                raise ValueError(f"row {n + 2}: expected 11 fields, got {len(row)}: {row}")
            n += 1
            t = ts_to_min(row[TS])
            if prev_t is not None and t < prev_t:
                inversions += 1
            prev_t = t
            t_min = t if t_min is None else min(t_min, t)
            t_max = t if t_max is None else max(t_max, t)
            per_day[row[TS][:10]] += 1
            per_minute[t] += 1

            fb, tb = int(row[FROM_BANK]), int(row[TO_BANK])
            max_bank = max(max_bank, fb, tb)
            fa, ta = row[FROM_ACCT].strip().upper(), row[TO_ACCT].strip().upper()
            s, d = intern(fb, fa), intern(tb, ta)
            out_deg[s] += 1
            in_deg[d] += 1
            if pairs is not None:
                pairs.add((s << 32) | d)

            y = int(row[LABEL])
            labels += y
            fmt_count[row[FMT]] += 1
            fmt_label[row[FMT]] += y
            pay_ccy[row[PAY_CCY]] += 1
            recv_ccy[row[RECV_CCY]] += 1
            if s == d:
                self_loops += 1
                self_loop_labels += y
                self_loop_fmt[row[FMT]] += 1

            paid, recv = float(row[AMT_PAID]), float(row[AMT_RECV])
            amt_max = max(amt_max, paid, recv)
            amt_min = min(amt_min, paid, recv)
            max_decimals = max(max_decimals, decimals(row[AMT_PAID]), decimals(row[AMT_RECV]))
            if paid >= 167772.16 or recv >= 167772.16:
                over_f32_exact += 1
            if row[PAY_CCY] != row[RECV_CCY]:
                cross_ccy += 1
                if paid > 0:
                    pair = f"{row[PAY_CCY]} -> {row[RECV_CCY]}"
                    rate_seen[pair] += 1
                    samples = rate_samples[pair]
                    if len(samples) < RESERVOIR:
                        samples.append(recv / paid)
                    else:
                        j = rng.randrange(rate_seen[pair])
                        if j < RESERVOIR:
                            samples[j] = recv / paid
            elif abs(paid - recv) > 1e-9:
                same_ccy_amount_mismatch += 1

            if pk is not None:
                k = tx_key(row)
                if pk.get(k, 0) > 0:
                    pk[k] -= 1
                    matched_pattern_rows += 1
                    laundering_in_patterns += y

    N, E = len(node_id), n
    od, idg = sorted(out_deg), sorted(in_deg)
    collisions = sum(1 for banks in hex_to_banks.values() if len(banks) > 1)
    rates = {p: {"rows": rate_seen[p], "median": statistics.median(v),
                 "min": min(v), "max": max(v),
                 "rel_spread": (max(v) - min(v)) / statistics.median(v) if statistics.median(v) else None}
             for p, v in sorted(rate_samples.items())}

    report = {
        "file": path,
        "seconds": round(time.time() - t0, 1),
        "header_raw": header,
        "header_matches_expected": header_ok,
        "rows": E,
        "accounts": N,
        "labels": {"laundering": labels, "rate": labels / E if E else 0,
                   "one_per_n": round(E / labels, 1) if labels else None},
        "time": {
            "min": min_to_str(t_min), "max": min_to_str(t_max),
            "span_days": round((t_max - t_min) / 1440, 2),
            "rows_out_of_order": inversions,
            "file_is_time_sorted": inversions == 0,
            "distinct_minutes": len(per_minute),
            "max_rows_in_one_minute": max(per_minute.values()),
            "rows_per_day": dict(sorted(per_day.items())),
        },
        "account_ids": {
            "string_length_hist": dict(sorted(acct_len.items())),
            "non_hex_ids": non_hex,
            "max_account_bits": max_acct_bits,
            "max_bank_id": max_bank,
            "max_bank_bits": max_bank.bit_length(),
            "same_account_string_under_multiple_banks": collisions,
            "packable_into_u64": non_hex == 0 and max_acct_bits <= 36 and max_bank.bit_length() <= 28,
        },
        "edges": {
            "self_loops": self_loops,
            "self_loop_fraction": self_loops / E if E else 0,
            "self_loops_laundering": self_loop_labels,
            "self_loops_by_format": dict(self_loop_fmt.most_common()),
            "distinct_src_dst_pairs": len(pairs) if pairs is not None else None,
            "multi_edge_ratio": (E / len(pairs)) if pairs else None,
        },
        "degree": {
            "out": {"max": od[-1], "p50": pct(od, .5), "p99": pct(od, .99), "zero": od.count(0)},
            "in": {"max": idg[-1], "p50": pct(idg, .5), "p99": pct(idg, .99), "zero": idg.count(0)},
            "fits_u32": od[-1] < 2**32 and idg[-1] < 2**32,
        },
        "categoricals": {
            "payment_format": {k: {"rows": v, "laundering": fmt_label[k],
                                   "rate": fmt_label[k] / v} for k, v in fmt_count.most_common()},
            "payment_currency": dict(pay_ccy.most_common()),
            "receiving_currency": dict(recv_ccy.most_common()),
            "n_currencies": len(set(pay_ccy) | set(recv_ccy)),
            "n_formats": len(fmt_count),
        },
        "amounts": {
            "min": amt_min, "max": amt_max, "max_decimal_places": max_decimals,
            "rows_where_float32_loses_cents": over_f32_exact,
            "cross_currency_rows": cross_ccy,
            "same_currency_but_paid_ne_received": same_ccy_amount_mismatch,
            "fx_rates_recv_over_paid": rates,
        },
    }
    if pk is not None:
        report["pattern_join"] = {
            "pattern_rows_matched_in_trans": matched_pattern_rows,
            "laundering_rows_covered_by_patterns": laundering_in_patterns,
            "fraction_of_laundering_covered": laundering_in_patterns / labels if labels else None,
            "pattern_rows_unmatched": sum(v for v in pk.values() if v > 0),
        }
    report["csr_memory_estimate_MB"] = csr_memory(N, E)
    return report


def csr_memory(N, E):
    """Byte layout from DESIGN.md section 3. Keep in sync with graph.hpp."""
    MB = 1 / 2**20
    offsets = 2 * (N + 1) * 8                  # out_offsets + in_offsets, u64
    out_hot = E * (4 + 4)                      # dst u32, t i32
    in_hot = E * (4 + 4 + 4)                   # src u32, t i32, eid u32
    attrs = E * (8 + 8 + 1 + 1 + 1 + 1)        # amt_paid f64, amt_recv f64, pay/recv ccy, format, flags
    row_ids = E * 4                            # original CSV row, u32
    labels = E * 2                             # GroundTruth (is_laundering + typology), kept apart
    keys = N * 8                               # packed account key per node
    total = offsets + out_hot + in_hot + attrs + row_ids + labels + keys
    return {k: round(v * MB, 1) for k, v in {
        "offsets": offsets, "out_topology": out_hot, "in_topology": in_hot,
        "edge_attrs": attrs, "row_ids": row_ids, "labels": labels,
        "node_keys": keys, "total": total}.items()}


def print_summary(r):
    a, t, e, ids = r["amounts"], r["time"], r["edges"], r["account_ids"]
    print(f"\n== {r['file']}  ({r['seconds']} s)")
    print(f"header matches expected positional schema: {r['header_matches_expected']}")
    print(f"rows (edges) E = {r['rows']:,}   accounts (nodes) N = {r['accounts']:,}")
    lab = r["labels"]
    print(f"laundering: {lab['laundering']:,}  (1 per {lab['one_per_n']})")
    print(f"time: {t['min']} -> {t['max']}  ({t['span_days']} days), "
          f"time-sorted file: {t['file_is_time_sorted']} ({t['rows_out_of_order']:,} inversions)")
    print(f"busiest minute: {t['max_rows_in_one_minute']:,} rows")
    print(f"account ids: lengths {ids['string_length_hist']}, non-hex {ids['non_hex_ids']}, "
          f"max bits {ids['max_account_bits']}, max bank {ids['max_bank_id']} "
          f"({ids['max_bank_bits']} bits), packable u64: {ids['packable_into_u64']}")
    print(f"same account string under >1 bank: {ids['same_account_string_under_multiple_banks']:,}")
    print(f"self-loops: {e['self_loops']:,} ({e['self_loop_fraction']:.1%}), "
          f"laundering among them: {e['self_loops_laundering']}, by format: {e['self_loops_by_format']}")
    if e["distinct_src_dst_pairs"] is not None:
        print(f"distinct (src,dst) pairs: {e['distinct_src_dst_pairs']:,}  "
              f"-> {e['multi_edge_ratio']:.2f} tx per pair")
    d = r["degree"]
    print(f"out-degree max {d['out']['max']:,} p99 {d['out']['p99']}  |  "
          f"in-degree max {d['in']['max']:,} p99 {d['in']['p99']}")
    c = r["categoricals"]
    print(f"{c['n_formats']} payment formats, {c['n_currencies']} currencies")
    for k, v in c["payment_format"].items():
        print(f"   {k:<14} {v['rows']:>10,}  laundering {v['laundering']:>6,}  ({v['rate']:.3%})")
    print(f"amounts: {a['min']} .. {a['max']:,}  max decimals {a['max_decimal_places']}  "
          f"float32-unsafe rows {a['rows_where_float32_loses_cents']:,}")
    print(f"cross-currency rows: {a['cross_currency_rows']:,}  "
          f"same-ccy paid!=recv: {a['same_currency_but_paid_ne_received']:,}")
    spreads = [v["rel_spread"] for v in a["fx_rates_recv_over_paid"].values() if v["rel_spread"] is not None]
    if spreads:
        print(f"fx rate relative spread across pairs: max {max(spreads):.4f} "
              f"(near 0 => fixed rates, USD normalisation is exact)")
    if "pattern_join" in r:
        pj = r["pattern_join"]
        print(f"patterns: {pj['pattern_rows_matched_in_trans']:,} rows matched, "
              f"{pj['pattern_rows_unmatched']:,} unmatched; laundering covered by a pattern: "
              f"{pj['fraction_of_laundering_covered']:.1%}")
    if "patterns" in r:
        print("typology            blocks  tx/block p50/max  accts p50/max  span(min) p50/p95/max  same-minute")
        for typ, p in r["patterns"].items():
            tx, ac, sp = p["tx_per_block"], p["accounts_per_block"], p["time_span_minutes"]
            smf = p["same_minute_fraction"]
            print(f"  {typ:<18} {p['blocks']:>6}  {tx.get('p50')!s:>5}/{tx.get('max')!s:<6}"
                  f"   {ac.get('p50')!s:>4}/{ac.get('max')!s:<5}  "
                  f"{sp.get('p50')!s:>6}/{sp.get('p95')!s:>6}/{sp.get('max')!s:<7}"
                  f"  {'' if smf is None else f'{smf:.1%}'}")
    print(f"CSR memory estimate (MB): {r['csr_memory_estimate_MB']}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trans_csv")
    ap.add_argument("--patterns", help="matching *_Patterns.txt")
    ap.add_argument("--json", help="write full report here")
    ap.add_argument("--no-pairs", action="store_true", help="skip distinct (src,dst) count to save RAM")
    ap.add_argument("--max-rows", type=int, help="only read the first N rows (smoke test)")
    args = ap.parse_args(argv)

    pattern_section, keys, extra = None, None, {}
    if args.patterns:
        blocks, padded, stray = parse_patterns(args.patterns)
        pattern_section, keys = pattern_report(blocks)
        extra = {"blocks": len(blocks), "rows_with_zero_padded_bank": padded, "stray_lines": stray}

    rep = inspect(args.trans_csv, keys, want_pairs=not args.no_pairs, max_rows=args.max_rows)
    if pattern_section is not None:
        rep["patterns"] = pattern_section
        rep["patterns_file"] = {"path": args.patterns, **extra}
    print_summary(rep)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(rep, f, indent=2, default=str)
        print(f"\nfull report -> {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
