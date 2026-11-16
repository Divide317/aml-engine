# AML Pattern Engine

A C++20 graph engine, with Python bindings, for detecting money-laundering patterns in transaction data.

**Status: design stage.** The design, the C++ API and the test fixtures are written. The engine itself is not built yet, so there is nothing to run and there are no results to report.

## What it will do

Transactions form a directed graph over time: accounts are nodes and each payment is an edge with a timestamp and an amount. Laundering leaves recognisable shapes in that graph. The engine will find them:

- **Cycles:** money that leaves an account and returns to it through a chain of others within a time window.
- **Fan-in and fan-out:** many accounts paying into one, or one account paying out to many.
- **Scatter-gather and gather-scatter:** funds split across intermediaries and then recombined.

It targets IBM's synthetic AMLworld dataset (Altman et al., NeurIPS 2023), starting with the HI-Small split: 5.08 million transactions between 515 thousand accounts over 10 days, of which about 1 in 981 is laundering.

## What is in the repository today

| Path | What it is |
|---|---|
| [`DESIGN.md`](DESIGN.md) | The data model, memory layout, exact pattern definitions and the reasoning behind each choice |
| [`include/aml/graph.hpp`](include/aml/graph.hpp) | The C++ API: types, layout and function signatures, with nothing implemented behind it yet |
| `tests/fixtures/` | A tiny hand-checkable transaction file with its expected answers |
| `tests/make_expected.py` | Produces the expected answers with NetworkX, as an independent reference |
| `tools/inspect_ibm_aml.py` | Inspects the real dataset to settle the open questions listed in the design |

## Approach

- The graph is stored in compressed sparse row form, one edge per transaction, with each account's outgoing edges sorted by time. That makes "edges from this account inside this time window" a binary search.
- Arrays are laid out by field (struct-of-arrays) so traversals touch only the columns they need.
- Ground-truth labels live in a separate structure that no detector accepts, so a detector cannot read the answers.
- Results will be tuned on days 1 to 7 and reported on days 8 to 10, with the hardware, compiler flags and dataset size recorded.

## Roadmap

1. Build system, CSV loader and graph builder; reproduce the tiny fixture's expected answers exactly.
2. Strongly connected components and traversals, with Python bindings, checked against NetworkX.
3. Temporal cycle detection, with precision and recall measured against the dataset's labels.
4. Fan-in, fan-out and scatter-gather detectors.
5. Benchmarks against NetworkX on 1, 2, 5 and 10-day slices.

## Principles

Every reported number comes from a script that actually ran, with the hardware, compiler flags and dataset size recorded alongside it.
