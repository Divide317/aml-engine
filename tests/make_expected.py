#!/usr/bin/env python3
"""
Test oracle for the C++ CSR builder. Deliberately naive (Python sort, no
counting sort) so it shares no logic with the engine. Writes the exact arrays
the C++ builder must produce for a given *_Trans.csv, plus SCC and temporal-cycle
answers cross-checked against NetworkX.

Conventions it pins down (see DESIGN.md section 3):
  * node id     = first-seen order, scanning rows top to bottom, src before dst
  * node key    = (bank << 36) | int(account_hex, 16)
  * edge id     = position in the out-CSR; rows sorted by (t, dst, csv_row)
  * in-CSR rows sorted by (t, src, eid)
  * temporal cycle = simple closed path, t non-decreasing along the path,
    last.t - first.t <= window, self-loops excluded, reported once with
    e0 = the (t, eid)-minimum edge of the cycle.

Usage: python3 -I tests/make_expected.py tests/fixtures/tiny_Trans.csv tests/fixtures/tiny_expected.json
"""
import csv
import json
import sys
from datetime import datetime, timezone

import networkx as nx


def minutes(s):
    return int(datetime.strptime(s, "%Y/%m/%d %H:%M").replace(tzinfo=timezone.utc).timestamp()) // 60


def load(path):
    ids, keys, rows = {}, [], []
    with open(path, newline="") as f:
        r = csv.reader(f)
        next(r)
        for i, row in enumerate(r):
            ends = []
            for bank, acct in ((row[1], row[2]), (row[3], row[4])):
                k = (int(bank) << 36) | int(acct, 16)
                if k not in ids:
                    ids[k] = len(keys)
                    keys.append(k)
                ends.append(ids[k])
            rows.append({"row": i, "src": ends[0], "dst": ends[1], "t": minutes(row[0]),
                         "label": int(row[10])})
    return keys, rows


def build(keys, rows):
    N = len(keys)
    out = sorted(rows, key=lambda e: (e["src"], e["t"], e["dst"], e["row"]))
    for eid, e in enumerate(out):
        e["eid"] = eid
    out_off = [0] * (N + 1)
    for e in out:
        out_off[e["src"] + 1] += 1
    for u in range(N):
        out_off[u + 1] += out_off[u]
    inn = sorted(out, key=lambda e: (e["dst"], e["t"], e["src"], e["eid"]))
    in_off = [0] * (N + 1)
    for e in inn:
        in_off[e["dst"] + 1] += 1
    for v in range(N):
        in_off[v + 1] += in_off[v]
    return out, inn, out_off, in_off


def temporal_cycles(out, out_off, window, max_len):
    """Brute force: DFS from every edge, as the definition reads."""
    found = []
    by_src = [out[out_off[u]:out_off[u + 1]] for u in range(len(out_off) - 1)]

    def dfs(start, path, visited):
        last = path[-1]
        for e in by_src[last["dst"]]:
            if e["src"] == e["dst"]:
                continue
            if e["t"] < last["t"] or e["t"] - start["t"] > window:
                continue
            if (e["t"], e["eid"]) <= (start["t"], start["eid"]):
                continue                       # start must be the cycle's minimum edge
            if e["dst"] == start["src"]:
                found.append([x["eid"] for x in path] + [e["eid"]])
            elif e["dst"] not in visited and len(path) + 1 < max_len:
                dfs(start, path + [e], visited | {e["dst"]})

    for e in out:
        if e["src"] != e["dst"]:
            dfs(e, [e], {e["src"], e["dst"]})
    return sorted(found)


def main(src, dst):
    keys, rows = load(src)
    out, inn, out_off, in_off = build(keys, rows)

    G = nx.MultiDiGraph()
    G.add_nodes_from(range(len(keys)))
    for e in out:
        if e["src"] != e["dst"]:
            G.add_edge(e["src"], e["dst"], key=e["eid"])
    comps = sorted(sorted(c) for c in nx.strongly_connected_components(G))

    # Cross-check the node-level cycle structure against NetworkX (time ignored).
    nx_cycles = {tuple(c) for c in nx.simple_cycles(nx.DiGraph(G), length_bound=6)}

    cycles = {}
    for w in (24 * 60, 48 * 60):
        cyc = temporal_cycles(out, out_off, window=w, max_len=6)
        for c in cyc:  # every temporal cycle must be a node-level cycle NetworkX also finds
            nodes = [out[i]["src"] for i in c]
            assert any(tuple(nodes[k:] + nodes[:k]) in nx_cycles for k in range(len(nodes))), nodes
        cycles[f"window_{w}"] = cyc

    expected = {
        "num_nodes": len(keys), "num_edges": len(out),
        "node_key": keys,
        "out_offsets": out_off,
        "out_dst": [e["dst"] for e in out],
        "out_t": [e["t"] for e in out],
        "src_row": [e["row"] for e in out],
        "in_offsets": in_off,
        "in_src": [e["src"] for e in inn],
        "in_t": [e["t"] for e in inn],
        "in_eid": [e["eid"] for e in inn],
        "self_loop_eids": [e["eid"] for e in out if e["src"] == e["dst"]],
        "is_laundering": [e["label"] for e in out],
        "scc_nontrivial": [c for c in comps if len(c) > 1],
        "temporal_cycles": cycles,
    }
    with open(dst, "w") as f:
        json.dump(expected, f, indent=1)
    for k in ("out_offsets", "out_dst", "src_row", "in_offsets", "in_src", "in_eid",
              "self_loop_eids", "scc_nontrivial", "temporal_cycles"):
        print(f"{k:16} {expected[k]}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
