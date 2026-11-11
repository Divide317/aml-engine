# AML Pattern Engine: data model, CSR layout and C++ API

Status: design v0.1 (Nov 2026). The schema comes from IBM's NeurIPS 2023 paper and a public preview of `HI-Small_Trans.csv`. Section 8 lists the choices still waiting on one run of `tools/inspect_ibm_aml.py` over the real file.

---

## 1. The dataset

IBM's AMLworld generator models a virtual economy where some agents launder money. Every transaction carries a label, and an extra file lists which laundering transactions belong to which pattern.

| Split | Days | Accounts (nodes) | Transactions (edges) | Laundering | Rate |
|---|---|---|---|---|---|
| **HI-Small** | 10 | 515K | 5.08M | 3.6K | 1 in 981 |
| LI-Small | 10 | 705K | 6.92M | 4.0K | 1 in 1,942 |
| HI-Medium | 16 | 2.08M | 31.9M | 35K | 1 in 905 |
| HI-Large | 97 | 2.12M | 179.7M | 223K | 1 in 807 |

Each split has three files:
- `*_Trans.csv`: one row per transaction
- `*_accounts.csv`: account metadata (bank name, entity); the engine doesn't need it
- `*_Patterns.txt`: ground-truth pattern blocks

The paper's eight patterns are fan-out, fan-in, cycle, random, bipartite, stack, scatter-gather and gather-scatter.

Some READMEs say HI is "~5% illicit". That's wrong: it's about 0.1%. Use the paper's numbers.

### `*_Trans.csv` schema (read by position)

| # | Raw header | Example | Engine type | Notes |
|---|---|---|---|---|
| 0 | `Timestamp` | `2022/09/01 00:20` | `Minute` (i32, min since epoch, UTC) | Minute resolution, so many ties |
| 1 | `From Bank` | `3208` | part of `u64` key | Preview max is 245,335 (18 bits) |
| 2 | `Account` | `8000F4580` | part of `u64` key | 9 hex chars = 36 bits |
| 3 | `To Bank` | `1` | part of `u64` key | |
| 4 | `Account` (again) | `8000F5340` | part of `u64` key | Pandas renames it `Account.1` |
| 5 | `Amount Received` | `3697.34` | `double` | In receiving currency |
| 6 | `Receiving Currency` | `US Dollar` | `Code` (u8, dictionary) | |
| 7 | `Amount Paid` | `3697.34` | `double` | In payment currency; differs from col 5 when currencies differ |
| 8 | `Payment Currency` | `Euro` | `Code` | |
| 9 | `Payment Format` | `ACH` | `Code` | Cheque, Credit Card, ACH, Cash, Reinvestment, Wire, Bitcoin |
| 10 | `Is Laundering` | `0` | `u8`, stored in `GroundTruth` only | |

### Gotchas that shape the design
1. **Two columns are named `Account`.** Parse by position, never by header name. The loader checks that there are 11 fields and that the header matches the list exactly.
2. **The file isn't sorted by time.** The preview goes 00:20, 00:20, 00:00, 00:02. The builder sorts each row by time, and every windowed query depends on that.
3. **Self-loops are common.** Many `Reinvestment` rows have the same account on both ends. They're kept so totals match the CSV, but they're flagged `kSelfLoop` and every detector skips them.
4. **An account is (bank, account-hex), not the hex alone.** Other work keys on bank plus account to avoid collisions between banks, and the fixture includes one such collision.
5. **The pattern file covers only part of the laundering.** Most laundering transactions aren't assigned to any pattern. So:
   - **per-typology recall** uses only that pattern's rows in `Patterns.txt` as the denominator;
   - **precision** is measured against `Is Laundering`.

   The script reports how much of the laundering the pattern file covers.
6. **Minute resolution causes ties.** Same-minute hops are real, so time along a path only has to be non-decreasing. Section 5 shows how cycles are still reported exactly once.
7. **Cross-currency rows.** `Amount Paid` and `Amount Received` differ when the currencies differ. The script estimates exchange rates from these rows. If the spread is about 0, the rates are fixed and converting everything to USD is exact.
8. **`Patterns.txt` may zero-pad bank IDs** (`010` vs `10`). Always join on `int(bank)`.

---

## 2. Which size to start with

**HI-Small.** The whole graph fits in about 235 MB (section 4), and it's the split most papers report on, so your numbers can be compared with theirs.

- **NetworkX benchmarks:** use time slices of HI-Small (first 1, 2, 5 and 10 days) via `LoadOptions::time_filter`, so you get a scaling curve. A `MultiDiGraph` with 5M edges in pure Python is likely to run out of memory on a laptop. Measure this; don't assume it.
- **Scale test (engine only):** HI-Medium, 32M edges, about 1.4 GB.
- **Imbalance check (later):** LI-Small, to see whether your precision survives half the laundering rate.
- **HI-Large:** about 7.9 GB in this layout. That's the "what changes at 100×" discussion (section 9), not a laptop target.

---

## 3. The graph model

- **Node** = one account, keyed by `pack_account(bank, hex) = bank << 36 | hex`.
- **Edge** = one transaction. It's a **multigraph**: repeated A→B transactions stay separate edges because each has its own time and amount.
- **Node ids** = first-seen order: scan rows top to bottom and intern the source before the destination. Alternative orderings (`NodeOrder::DegreeDescending`, `Bfs`) exist only for the cache-locality experiment.
- **Canonical `EdgeId`** = position in the out-CSR. Out-rows are sorted by `(t, dst, csv_row)`, so a node's out-edges are a contiguous id range. That's why `out_edges(u)` returns an `iota_view` instead of a container.
- **In-CSR rows** are sorted by `(t, src, eid)`. Each in-entry stores `src`, `t` and `eid`. `t` is a copy, so a windowed fan-in scan reads one contiguous run instead of jumping through `eid` into `out_t`.
- **Labels live in `GroundTruth`, not the graph.** No detector takes it as an argument, so label leakage is impossible by construction.

These conventions are pinned by `tests/make_expected.py`, a deliberately naive Python oracle. Your C++ builder must reproduce its arrays exactly.

---

## 4. Memory layout (struct-of-arrays)

```
            per node (N+1)          per edge (E), indexed by canonical EdgeId
OUT  hot    out_offsets u64    →    out_dst u32 | out_t i32
IN   hot    in_offsets  u64    →    in_src u32  | in_t i32 | in_eid u32
ATTR warm                           amount_paid f64 | amount_received f64 | pay_ccy u8 | recv_ccy u8 | format u8 | flags u8
ATTR cold                           src_row u32
TRUTH (separate object)             is_laundering u8 | typology u8
NODE        node_key u64
```

| Block | Bytes/edge | HI-Small | HI-Medium | HI-Large |
|---|---|---|---|---|
| Out topology (`dst`, `t`) | 8 | 38.7 MB | 243 MB | 1.37 GB |
| In topology (`src`, `t`, `eid`) | 12 | 58.1 MB | 365 MB | 2.06 GB |
| Attributes | 20 | 96.9 MB | 608 MB | 3.43 GB |
| `src_row` + labels | 6 | 29.1 MB | 183 MB | 1.03 GB |
| Offsets + node keys (per node) | — | 11.8 MB | 47.5 MB | 48.4 MB |
| **Total** | **≈ 48** | **≈ 235 MB** | **≈ 1.45 GB** | **≈ 7.9 GB** |

(Numbers come from `csr_memory()` in the inspection script, which mirrors `graph.hpp`.)

Why this layout:
- **The cycle search touches only `out_offsets`, `out_dst` and `out_t`,** 8 bytes per edge read in sequence. In an array-of-structs layout the same loop would drag 40+ bytes of amounts and codes through the cache for every edge. Your AoS vs SoA benchmark measures exactly this difference.
- **`double` for amounts.** `float` can't represent cents above about $167,772. The script counts how many rows cross that limit.
- **64-bit offsets** cost only N×8 bytes and remove the 4-billion-edge ceiling.
- **32-bit node and edge ids** halve the topology size compared with 64-bit. HI-Large's 180M edges are well inside `u32`.

---

## 5. Exact definitions (what the tests check)

**Temporal cycle.** A path e0 → e1 → … → e(k−1) that returns to src(e0), where:
- it's simple (no node repeats) and contains no self-loops;
- 2 ≤ k ≤ `max_len`;
- t(e_i) ≤ t(e_{i+1}), or < if `strict_time` is set;
- t(e_{k−1}) − t(e0) ≤ `window`.

Each cycle is reported **once**, rotated so that e0 is its `(t, eid)`-minimum edge. That rule makes the enumeration unique even when hops share a minute: you start a search only from edges that could be the minimum, and you prune any extension that would undercut the start edge.

**Fan-out / fan-in.** A center node with at least `min_distinct` distinct counterparties inside a sliding time window. Because rows are time-sorted, this is a two-pointer scan per node, O(deg) each.

**Scatter-gather.** A source sends to at least `min_intermediaries` distinct middle accounts, which all forward to one sink. Each second hop happens no earlier than its first hop, and everything fits inside `window`.

**Gather-scatter.** A fan-in to a center followed by a fan-out from it, within `window`.

**Risk propagation.** r ← (1−α)·seed + α·Wᵀr, a pull-style sparse matrix-vector product over the in-CSR. W[u][v] is the share of u's outgoing money that went to v. This is the only "matrix math" in the engine, and it's sparse.

**Evaluation (in Python, not C++).** Detectors set `Typology` bits in a `DetectionMask`, one byte per edge. Precision compares those flagged edges with `Is Laundering`. Per-typology recall compares them with that typology's rows in `Patterns.txt`.

### The fixture: `tests/fixtures/tiny_Trans.csv` (12 rows, 10 accounts)

The fixture contains:
- a laundering cycle A→B→C→D→A that switches from USD to EUR halfway;
- a laundering fan-out from X to Y1/Y2/Y3, with two edges in the same minute;
- a self-loop on A;
- a second, legitimate A→B transaction (a multi-edge);
- a legitimate B→A refund;
- a C→A payment 33 hours later;
- the same account string at two different banks.

The oracle's output:

```
out_offsets  [0, 3, 5, 8, 8, 10, 11, 11, 11, 12, 12]
out_dst      [1, 1, 0, 0, 4, 3, 7, 9, 8, 0, 6, 0]
src_row      [1, 9, 0, 7, 3, 2, 5, 8, 6, 11, 4, 10]
in_offsets   [0, 4, 6, 6, 7, 8, 8, 9, 10, 11, 12]
in_src       [0, 1, 8, 4, 0, 0, 2, 1, 5, 2, 4, 2]
in_eid       [2, 3, 11, 9, 0, 1, 5, 4, 10, 6, 8, 7]
SCC          [[0, 1, 4, 8]]   (A, B, C, D)
cycles @24h  [0,3] [0,4,8,11] [1,3] [1,4,8,11]
cycles @48h  adds [0,4,9] [1,4,9]
```

**Lesson from the fixture.** At a 24-hour window the detector flags 6 edges, but only 4 are laundering, so precision is 67% even on 12 rows. The false positives are the $10 refund (the 2-cycles `[0,3]` and `[1,3]`) and the $60 payment that happens to start a cycle.

`CycleParams::hop_ratio` addresses this. The hypothesis: laundered money moves on almost in full, minus fees, so each hop passes on 80–105% of what it received. On the fixture:

| Filter | Cycles kept | Edge precision |
|---|---|---|
| None | 4 | 4/6 = 67% |
| Lower bound only (≥ 0.8) | `[0,4,8,11]`, `[1,4,8,11]` | 4/5 = 80% (60 → 4950 passes) |
| Two-sided `{0.8, 1.05}` | `[0,4,8,11]` | 4/4 = 100% |

A lower bound alone isn't enough: a small payment followed by a big one also passes it. The real cycle passes both bounds: 5000 → 4950 USD, 4554 → 4500 EUR, 4500 → 4400 EUR. The check compares `prev.amount_received` with `next.amount_paid` only when their currencies match, so no exchange rate is needed.

This is a hypothesis about the generator, not a fact. Test it on days 1–7 and report precision and recall with and without the filter.

---

## 6. Build algorithm (you implement it in `src/build.cpp`)

1. **Parse.** One pass over the CSV, producing a `vector<RawEdge>`. Intern accounts through a `unordered_map<u64, NodeId>`. Dictionary-encode currencies and formats.
2. **Out-CSR by counting sort.** Count out-degrees, take an exclusive prefix sum into `out_offsets`, then scatter edges into position: O(N + E), no comparison sort. Then sort each row by `(t, dst, row)`: O(Σ dᵢ log dᵢ). Rows are short except for hubs.
3. **Permute the attributes** into canonical `EdgeId` order.
4. **In-CSR.** Same counting sort keyed on `dst`, then sort each row by `(t, src, eid)`.
5. **Sorted key index** for `find_node`, then `validate()`.

Be ready to explain why the counting sort is O(E) and why it's stable. "Why CSR, and what does an insert cost?" is interview question #1. The answer: an insert into a static CSR is O(E) because everything after it shifts, so streaming mode (Path B) puts new edges in a delta structure and periodically merges.

---

## 7. API surface

The full contract is `include/aml/graph.hpp`, which compiles cleanly with `-Wall -Wextra -Wpedantic -Wconversion -Werror` on g++ 13 and clang. The core calls:

```cpp
auto [g, truth, stats] = aml::load_ibm_csv("data/raw/HI-Small_Trans.csv");
aml::load_ibm_patterns("data/raw/HI-Small_Patterns.txt", g, truth);

for (aml::EdgeId e : g.out_edges(u, {t0, t0 + 1440})) { g.dst(e); g.time(e); }
aml::InSlice in = g.in_edges(v, {t0, t0 + 1440});   // in.src[i], in.t[i], in.eid[i]

auto scc    = aml::strongly_connected_components(g);
auto cycles = aml::temporal_cycles(g, {.window = 1440, .max_len = 6, .hop_ratio = aml::HopRatio{0.8, 1.05}});
auto fans   = aml::fan_patterns(g, {.dir = aml::Direction::Out, .window = 1440, .min_distinct = 5});
auto risk   = aml::propagate_risk(g, seed, {});
aml::DetectionMask mask(g.num_edges()); mask.mark(cycles, aml::kCycle);
```

Design rules:
- **Results come back `Ragged<T>`**, flat `offsets + items` (the same shape as CSR), so pybind11 can hand them to NumPy without creating a Python object per cycle.
- **Detectors take `const TemporalCSR&`.** The `TemporalGraph` concept, `static_assert`ed in the header, is the contract a future streaming graph must meet so the same detectors can run on it.
- **The graph is immutable once built,** so it's safe to read from multiple threads. Parallelising over start nodes later needs no locks.

### Planned Python surface (pybind11, week 2)

```python
import aml_engine as ae
g, truth = ae.load_ibm_csv("HI-Small_Trans.csv")
g.out_offsets, g.out_dst, g.out_t      # zero-copy NumPy views; the graph stays alive while they exist
comp = g.scc()                         # np.uint32[N]
offs, eids = g.temporal_cycles(window=1440, max_len=6)
mask = g.detect(cycles=dict(window=1440), fan_out=dict(min_distinct=5))  # np.uint8[E]
```

Zero-copy works by returning `py::array_t` with the graph object as its base, so NumPy keeps the graph alive. Section 2 of your blog post on ctypes vs pybind11 can be built around this.

---

## 8. Decisions to make after running the inspection script

Run:

```
python3 tools/inspect_ibm_aml.py data/raw/HI-Small_Trans.csv \
    --patterns data/raw/HI-Small_Patterns.txt --json reports/hi_small_schema.json
```

It takes about 1 minute and under 1 GB of RAM (1M synthetic rows took 11.5 s), and it reads only Python's standard library.

| Question | Report field | Rule |
|---|---|---|
| Is the u64 key safe? | `account_ids.packable_into_u64` | If false, fall back to interning strings in a hash map |
| Default cycle `window` | `patterns.CYCLE.time_span_minutes.p95` | Round up to whole hours; also try 2× |
| Default `max_len` | `patterns.CYCLE.accounts_per_block.max` | Use that number; the paper reports none above 18 |
| `strict_time`? | `patterns.*.same_minute_fraction` | If it's above 0 (likely), keep non-strict |
| Fan `min_distinct`, `window` | `patterns.FAN-OUT/FAN-IN` spans and sizes | Start at block-size p10, window at span p95 |
| Exact USD normalisation? | `amounts.fx_rates_recv_over_paid.*.rel_spread` | If ≈ 0, use a fixed rate table |
| How much recall is possible? | `pattern_join.fraction_of_laundering_covered` | This is the ceiling for typology-based recall; say so in the README |
| Join sanity | `pattern_join.pattern_rows_unmatched`, `patterns_file.stray_lines` | Both should be 0; if not, the parser assumption is wrong |
| Self-loop handling | `edges.self_loops_laundering` | If 0, skipping self-loops costs no recall |
| Hub risk | `degree.out.max`, `degree.in.max` | Very large hubs need a degree cap in cycle DFS (document it) |

Then tune thresholds on **days 1–7** and report on **days 8–10**. Tuning on the same days you evaluate on overstates precision.

---

## 9. What I'd change at 100× scale

- **Split hot and cold data.** Topology alone is about 20 B/edge, or 3.5 GB for HI-Large. Memory-map the attribute arrays from a binary file instead of keeping them resident, and save the built CSR to disk so later runs skip CSV parsing.
- **Shrink the edge arrays.** Delta-encode `out_t` within rows (differences fit in u16) and compress neighbour ids with varints. This trades CPU for memory bandwidth.
- **Partition by time.** One CSR per day plus a merged index: windowed queries touch only the days inside their window, and new days append without a rebuild.
- **Partition by account** (hash) across machines for multiple hosts. Cycles that cross partitions need distributed BFS or a frontier exchange, which is where real systems (e.g. IBM's Graph Feature Preprocessor) spend their engineering.

---

## Sources
- Altman et al., *Realistic Synthetic Financial Transactions for Anti-Money Laundering Models*, NeurIPS 2023 D&B: https://arxiv.org/abs/2306.16424
- Kaggle dataset: https://www.kaggle.com/datasets/ealtman2019/ibm-transactions-for-anti-money-laundering-aml
- IBM Multi-GNN (reference preprocessing): https://github.com/IBM/Multi-GNN
- Node/edge counts per split (BlazingAML, Table 1): https://arxiv.org/abs/2604.12241
- Pattern coverage note: https://arxiv.org/abs/2503.24259
