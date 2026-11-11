// aml/graph.hpp: API surface for the AML Pattern Engine (C++20).
//
// This header is the contract: types, layout and signatures. Small accessors
// are defined inline; loaders and detectors are declared and are yours to
// implement in src/. See DESIGN.md for the reasoning behind every choice here.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace aml {

// ---------------------------------------------------------------------------
// Scalar types
// ---------------------------------------------------------------------------
using NodeId = std::uint32_t;  // dense account index, 0..N-1 (HI-Large has ~2.1M)
using EdgeId = std::uint32_t;  // canonical edge = position in the out-CSR (HI-Large ~180M)
using Offset = std::uint64_t;  // CSR offsets: N+1 of them, so 64-bit costs almost nothing
using Minute = std::int32_t;   // minutes since Unix epoch, UTC (the data has minute resolution)
using Code   = std::uint8_t;   // dictionary-encoded currency / payment format

inline constexpr NodeId kNoNode = ~NodeId{0};

// Account key = (bank, account-hex) packed into 64 bits: bank << 36 | account.
// The same account string appears under different banks, so the bank is part of the key.
inline constexpr int kAccountBits = 36;  // 9 hex chars
[[nodiscard]] constexpr std::uint64_t pack_account(std::uint32_t bank, std::uint64_t account) noexcept {
    return (std::uint64_t{bank} << kAccountBits) | account;
}
[[nodiscard]] constexpr std::uint32_t key_bank(std::uint64_t key) noexcept {
    return static_cast<std::uint32_t>(key >> kAccountBits);
}
[[nodiscard]] constexpr std::uint64_t key_account(std::uint64_t key) noexcept {
    return key & ((std::uint64_t{1} << kAccountBits) - 1);
}

// Inclusive time interval [lo, hi].
struct TimeRange {
    Minute lo;
    Minute hi;
    [[nodiscard]] constexpr bool contains(Minute t) const noexcept { return lo <= t && t <= hi; }
};

// Per-edge structural flags, computed once at build time.
enum EdgeFlag : std::uint8_t {
    kSelfLoop      = 1u << 0,  // src == dst (mostly "Reinvestment"); detectors skip these
    kCrossCurrency = 1u << 1,  // pay_ccy != recv_ccy
};

// Detected-typology bits, one byte per edge (see DetectionMask).
enum Typology : std::uint8_t {
    kCycle         = 1u << 0,
    kFanIn         = 1u << 1,
    kFanOut        = 1u << 2,
    kScatterGather = 1u << 3,
    kGatherScatter = 1u << 4,
    kRiskScore     = 1u << 5,  // flagged by risk propagation threshold
};

// Flat "list of lists" result: item i owns items[offsets[i] .. offsets[i+1]).
// Same shape as CSR, cheap to hand to NumPy without a Python object per item.
template <class T>
struct Ragged {
    std::vector<Offset> offsets{0};
    std::vector<T> items;
    bool truncated = false;  // a max_results cap was hit

    [[nodiscard]] std::size_t size() const noexcept { return offsets.size() - 1; }
    [[nodiscard]] std::span<const T> operator[](std::size_t i) const noexcept {
        return {items.data() + offsets[i], items.data() + offsets[i + 1]};
    }
};

// ---------------------------------------------------------------------------
// Raw edge: what a parser produces and what the builder consumes.
// ---------------------------------------------------------------------------
struct RawEdge {
    NodeId src;
    NodeId dst;
    Minute t;
    double amount_paid;      // in pay_ccy
    double amount_received;  // in recv_ccy
    Code pay_ccy;
    Code recv_ccy;
    Code format;
    std::uint32_t src_row;   // 0-based data row in the CSV, for joins and debugging
};

class TemporalCSR;

// ---------------------------------------------------------------------------
// In-edge slice: struct-of-arrays view of one node's in-edges (time-sorted).
// ---------------------------------------------------------------------------
struct InSlice {
    std::span<const NodeId> src;
    std::span<const Minute> t;
    std::span<const EdgeId> eid;  // canonical edge id, for attributes and flags
    [[nodiscard]] std::size_t size() const noexcept { return src.size(); }
};

using EdgeRange = std::ranges::iota_view<EdgeId, EdgeId>;

// ---------------------------------------------------------------------------
// TemporalCSR: immutable directed temporal multigraph.
//
// Invariants (checked by validate()):
//   * out_offsets/in_offsets are non-decreasing, size N+1, back() == E
//   * within each out-row, (t, dst) is non-decreasing; within each in-row, (t, src)
//   * in_eid is a permutation of 0..E-1 and out_dst[in_eid[p]] == the in-row's node
//   * every edge attribute array has size E
// Immutable after build, so it is safe to read from many threads at once.
// ---------------------------------------------------------------------------
class TemporalCSR {
public:
    TemporalCSR() = default;

    [[nodiscard]] NodeId num_nodes() const noexcept { return static_cast<NodeId>(node_key_.size()); }
    [[nodiscard]] EdgeId num_edges() const noexcept { return static_cast<EdgeId>(out_dst_.size()); }

    // --- Out-edges: canonical ids are contiguous per source node -------------------
    [[nodiscard]] EdgeRange out_edges(NodeId u) const noexcept {
        return EdgeRange{static_cast<EdgeId>(out_offsets_[u]), static_cast<EdgeId>(out_offsets_[u + 1])};
    }
    // Out-edges of u with t in w. O(log deg(u)) via binary search on the time-sorted row.
    [[nodiscard]] EdgeRange out_edges(NodeId u, TimeRange w) const noexcept {
        const auto row_begin = out_t_.begin() + static_cast<std::ptrdiff_t>(out_offsets_[u]);
        const auto row_end   = out_t_.begin() + static_cast<std::ptrdiff_t>(out_offsets_[u + 1]);
        const auto lo = std::lower_bound(row_begin, row_end, w.lo);
        const auto hi = std::upper_bound(lo, row_end, w.hi);
        return EdgeRange{static_cast<EdgeId>(lo - out_t_.begin()), static_cast<EdgeId>(hi - out_t_.begin())};
    }
    [[nodiscard]] std::size_t out_degree(NodeId u) const noexcept {
        return static_cast<std::size_t>(out_offsets_[u + 1] - out_offsets_[u]);
    }

    // --- In-edges --------------------------------------------------------------------
    [[nodiscard]] InSlice in_edges(NodeId v) const noexcept {
        return in_slice(static_cast<std::size_t>(in_offsets_[v]), static_cast<std::size_t>(in_offsets_[v + 1]));
    }
    [[nodiscard]] InSlice in_edges(NodeId v, TimeRange w) const noexcept {
        const auto row_begin = in_t_.begin() + static_cast<std::ptrdiff_t>(in_offsets_[v]);
        const auto row_end   = in_t_.begin() + static_cast<std::ptrdiff_t>(in_offsets_[v + 1]);
        const auto lo = std::lower_bound(row_begin, row_end, w.lo);
        const auto hi = std::upper_bound(lo, row_end, w.hi);
        return in_slice(static_cast<std::size_t>(lo - in_t_.begin()), static_cast<std::size_t>(hi - in_t_.begin()));
    }
    [[nodiscard]] std::size_t in_degree(NodeId v) const noexcept {
        return static_cast<std::size_t>(in_offsets_[v + 1] - in_offsets_[v]);
    }

    // --- Per-edge access (by canonical EdgeId) ---------------------------------------
    [[nodiscard]] NodeId dst(EdgeId e) const noexcept { return out_dst_[e]; }
    [[nodiscard]] Minute time(EdgeId e) const noexcept { return out_t_[e]; }
    [[nodiscard]] std::uint8_t flags(EdgeId e) const noexcept { return flags_[e]; }
    [[nodiscard]] bool is_self_loop(EdgeId e) const noexcept { return (flags_[e] & kSelfLoop) != 0; }
    // Slow path, O(log N): edge-centric code should iterate nodes, then out_edges(u).
    [[nodiscard]] NodeId src(EdgeId e) const noexcept {
        const auto it = std::upper_bound(out_offsets_.begin(), out_offsets_.end(), Offset{e});
        return static_cast<NodeId>(it - out_offsets_.begin() - 1);
    }

    // --- Raw arrays: zero-copy views for SpMV loops and the pybind11 layer ----------
    [[nodiscard]] std::span<const Offset> out_offsets() const noexcept { return out_offsets_; }
    [[nodiscard]] std::span<const NodeId> out_dst() const noexcept { return out_dst_; }
    [[nodiscard]] std::span<const Minute> out_t() const noexcept { return out_t_; }
    [[nodiscard]] std::span<const Offset> in_offsets() const noexcept { return in_offsets_; }
    [[nodiscard]] std::span<const NodeId> in_src() const noexcept { return in_src_; }
    [[nodiscard]] std::span<const Minute> in_t() const noexcept { return in_t_; }
    [[nodiscard]] std::span<const EdgeId> in_eid() const noexcept { return in_eid_; }

    [[nodiscard]] std::span<const double> amount_paid() const noexcept { return amount_paid_; }
    [[nodiscard]] std::span<const double> amount_received() const noexcept { return amount_received_; }
    [[nodiscard]] std::span<const Code> pay_ccy() const noexcept { return pay_ccy_; }
    [[nodiscard]] std::span<const Code> recv_ccy() const noexcept { return recv_ccy_; }
    [[nodiscard]] std::span<const Code> format() const noexcept { return format_; }
    [[nodiscard]] std::span<const std::uint8_t> edge_flags() const noexcept { return flags_; }
    [[nodiscard]] std::span<const std::uint32_t> src_row() const noexcept { return src_row_; }

    // --- Node / dictionary metadata ------------------------------------------------
    [[nodiscard]] std::span<const std::uint64_t> node_key() const noexcept { return node_key_; }
    [[nodiscard]] NodeId find_node(std::uint64_t packed_key) const;  // kNoNode if absent
    [[nodiscard]] const std::vector<std::string>& currency_names() const noexcept { return currency_names_; }
    [[nodiscard]] const std::vector<std::string>& format_names() const noexcept { return format_names_; }

    // --- Diagnostics ----------------------------------------------------------------
    [[nodiscard]] std::size_t memory_bytes() const noexcept;  // sum of capacity() * sizeof over all arrays
    void validate() const;                                    // throws std::logic_error on a broken invariant

private:
    [[nodiscard]] InSlice in_slice(std::size_t b, std::size_t e) const noexcept {
        return {std::span<const NodeId>(in_src_).subspan(b, e - b),
                std::span<const Minute>(in_t_).subspan(b, e - b),
                std::span<const EdgeId>(in_eid_).subspan(b, e - b)};
    }

    // Topology, hot. A cycle search touches only out_offsets_, out_dst_, out_t_ (8 B/edge).
    std::vector<Offset> out_offsets_{0};
    std::vector<NodeId> out_dst_;
    std::vector<Minute> out_t_;
    std::vector<Offset> in_offsets_{0};
    std::vector<NodeId> in_src_;
    std::vector<Minute> in_t_;   // duplicated from out_t_ so windowed in-scans stay sequential
    std::vector<EdgeId> in_eid_;

    // Edge attributes, indexed by canonical EdgeId (struct-of-arrays).
    std::vector<double> amount_paid_;
    std::vector<double> amount_received_;
    std::vector<Code> pay_ccy_;
    std::vector<Code> recv_ccy_;
    std::vector<Code> format_;
    std::vector<std::uint8_t> flags_;
    std::vector<std::uint32_t> src_row_;

    // Nodes and dictionaries.
    std::vector<std::uint64_t> node_key_;
    std::vector<std::uint64_t> key_sorted_;  // sorted keys, for find_node
    std::vector<NodeId> key_sorted_id_;
    std::vector<std::string> currency_names_;
    std::vector<std::string> format_names_;

    friend class CSRBuilder;
};

// Anything a detector needs from a graph. TemporalCSR satisfies it today; a
// streaming delta graph (Path B) has to satisfy it too.
template <class G>
concept TemporalGraph = requires(const G& g, NodeId u, EdgeId e, TimeRange w) {
    { g.num_nodes() } -> std::convertible_to<NodeId>;
    { g.num_edges() } -> std::convertible_to<EdgeId>;
    { g.out_edges(u, w) } -> std::ranges::input_range;
    { g.in_edges(u, w) };
    { g.dst(e) } -> std::same_as<NodeId>;
    { g.time(e) } -> std::same_as<Minute>;
    { g.is_self_loop(e) } -> std::same_as<bool>;
};
static_assert(TemporalGraph<TemporalCSR>);

// ---------------------------------------------------------------------------
// Ground truth. Kept out of TemporalCSR on purpose: no detector signature takes
// it, so labels cannot leak into detection.
// ---------------------------------------------------------------------------
struct GroundTruth {
    std::vector<std::uint8_t> is_laundering;  // [EdgeId] from the CSV's last column
    std::vector<std::uint8_t> typology;       // [EdgeId] index into typology_names, 0 = none
    std::vector<std::string> typology_names{"NONE"};
    std::size_t pattern_rows_unmatched = 0;   // Patterns.txt rows with no matching edge
};

// ---------------------------------------------------------------------------
// Building and loading
// ---------------------------------------------------------------------------
enum class NodeOrder : std::uint8_t {
    FirstSeen,         // default: CSV order, src before dst
    DegreeDescending,  // hubs first; experiment for cache behaviour
    Bfs,               // BFS from highest-degree node; locality experiment
};

struct BuildOptions {
    bool keep_self_loops = true;  // kept and flagged, so totals still match the CSV
    NodeOrder node_order = NodeOrder::FirstSeen;
};

struct LoadOptions {
    BuildOptions build{};
    std::optional<TimeRange> time_filter;  // load a time slice, e.g. for scaling benchmarks
    std::size_t max_rows = 0;              // 0 = all rows
};

struct LoadStats {
    std::size_t rows_read = 0;
    std::size_t rows_kept = 0;
    std::size_t self_loops = 0;
    double parse_seconds = 0;
    double build_seconds = 0;
};

struct Loaded {
    TemporalCSR graph;
    GroundTruth truth;
    LoadStats stats;
};

class ParseError : public std::runtime_error {
public:
    ParseError(std::size_t line, const std::string& what)
        : std::runtime_error("line " + std::to_string(line) + ": " + what), line_(line) {}
    [[nodiscard]] std::size_t line() const noexcept { return line_; }
private:
    std::size_t line_;
};

// Two-pass counting-sort build: O(N + E) plus a per-row sort by (t, dst).
// Node ids in `edges` must already be dense in [0, num_nodes).
class CSRBuilder {
public:
    [[nodiscard]] static TemporalCSR build(std::vector<RawEdge> edges,
                                           std::vector<std::uint64_t> node_key,
                                           std::vector<std::string> currency_names,
                                           std::vector<std::string> format_names,
                                           const BuildOptions& opts = {});
};

// Parse an IBM *_Trans.csv (11 columns, read by position) into a graph + labels.
[[nodiscard]] Loaded load_ibm_csv(const std::filesystem::path& trans_csv, const LoadOptions& opts = {});

// Join a *_Patterns.txt onto an already loaded graph, filling truth.typology.
void load_ibm_patterns(const std::filesystem::path& patterns_txt, const TemporalCSR& g, GroundTruth& truth);

// ---------------------------------------------------------------------------
// Detectors. None of them take GroundTruth.
// ---------------------------------------------------------------------------

// Strongly connected components, iterative Tarjan (explicit stack, no recursion).
// Ignores time and self-loops. Useful as a pre-filter: every cycle lies inside one SCC.
struct SccResult {
    std::vector<std::uint32_t> component;  // [NodeId] component id
    std::uint32_t num_components = 0;
};
[[nodiscard]] SccResult strongly_connected_components(const TemporalCSR& g);

// Temporal cycle e0 -> e1 -> ... -> e(k-1), back to src(e0):
//   simple (no repeated node), self-loops excluded, 2 <= k <= max_len,
//   t(e_i) <= t(e_{i+1})  (or < when strict_time),
//   t(e_{k-1}) - t(e0) <= window,
//   reported once, rotated so e0 is the (t, eid)-minimum edge of the cycle.
//
// HopRatio: for consecutive hops a -> b, require
//   lo <= b.amount_paid / a.amount_received <= hi
// (checked only when b.pay_ccy == a.recv_ccy, so no FX table is needed).
// Laundered money moves on nearly whole, minus fees: try {0.8, 1.05}.
struct HopRatio {
    double lo = 0.8;
    double hi = 1.05;
};
struct CycleParams {
    Minute window = 24 * 60;
    std::uint8_t max_len = 6;
    bool strict_time = false;
    bool restrict_to_scc = true;         // skip start edges whose endpoints are in different SCCs
    std::optional<HopRatio> hop_ratio;   // amount-conservation filter, off by default
    std::size_t max_results = 10'000'000;
};
[[nodiscard]] Ragged<EdgeId> temporal_cycles(const TemporalCSR& g, const CycleParams& p);

// Fan-out / fan-in: a center node touching >= min_distinct distinct counterparties
// within a sliding window of length `window`. Each event lists its member edges.
enum class Direction : std::uint8_t { Out, In };
struct FanParams {
    Direction dir = Direction::Out;
    Minute window = 24 * 60;
    std::uint32_t min_distinct = 5;
    std::size_t max_results = 10'000'000;
};
struct FanEvent {
    NodeId center;
    Minute t_first;
    Minute t_last;
    std::uint32_t distinct;
};
struct FanResult {
    std::vector<FanEvent> events;
    Ragged<EdgeId> edges;  // edges[i] belongs to events[i]
};
[[nodiscard]] FanResult fan_patterns(const TemporalCSR& g, const FanParams& p);

// Scatter-gather: source -> >= min_intermediaries distinct middles -> one sink,
// every second hop no earlier than its first hop, all within window.
// Gather-scatter: fan-in to a center followed by fan-out from it within window.
struct ScatterGatherParams {
    Minute window = 24 * 60;
    std::uint32_t min_intermediaries = 3;
    std::size_t max_results = 10'000'000;
};
struct FlowEvent {
    NodeId a;       // scatter-gather: source; gather-scatter: center
    NodeId b;       // scatter-gather: sink;   gather-scatter: kNoNode
    std::uint32_t width;
};
struct FlowResult {
    std::vector<FlowEvent> events;
    Ragged<EdgeId> edges;
};
[[nodiscard]] FlowResult scatter_gather(const TemporalCSR& g, const ScatterGatherParams& p);
[[nodiscard]] FlowResult gather_scatter(const TemporalCSR& g, const ScatterGatherParams& p);

// Personalised-PageRank-style risk propagation by sparse mat-vec over the in-CSR:
//   r <- (1 - alpha) * seed + alpha * W^T r,  W[u][v] = share of u's outflow sent to v.
enum class EdgeWeight : std::uint8_t { Uniform, AmountShare };
struct RiskParams {
    float alpha = 0.85f;
    std::uint32_t max_iters = 50;
    float tol = 1e-6f;  // L1 change per iteration
    EdgeWeight weight = EdgeWeight::AmountShare;
    std::optional<TimeRange> window;
};
[[nodiscard]] std::vector<float> propagate_risk(const TemporalCSR& g, std::span<const float> seed,
                                                const RiskParams& p);

// ---------------------------------------------------------------------------
// Detection mask: one byte of Typology bits per edge; the thing you evaluate.
// ---------------------------------------------------------------------------
struct DetectionMask {
    std::vector<std::uint8_t> bits;  // [EdgeId]
    explicit DetectionMask(EdgeId num_edges) : bits(num_edges, 0) {}
    void mark(const Ragged<EdgeId>& groups, Typology t) {
        for (EdgeId e : groups.items) bits[e] |= t;
    }
};

}  // namespace aml
