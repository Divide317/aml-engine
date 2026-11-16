// Internal header (not part of the public API): the CSV parsing stage.
//
// Loading is split in two so each half can be tested on its own:
//   parse_ibm_csv   text file  -> rows as RawEdge + node keys + dictionaries
//   CSRBuilder      RawEdge[]  -> TemporalCSR
// The public aml::load_ibm_csv (graph.hpp) will call one, then the other.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aml/graph.hpp"

namespace aml::detail {

struct ParsedCsv {
    std::vector<RawEdge> edges;                // kept rows, in file order
    std::vector<std::uint64_t> node_key;       // [NodeId] packed (bank, account), first-seen order
    std::vector<std::string> currency_names;   // [Code], first-seen order
    std::vector<std::string> format_names;     // [Code], first-seen order
    // "Is Laundering" for every data row read, indexed by CSV row (RawEdge::src_row).
    // Kept apart from RawEdge so the builder never sees a label.
    std::vector<std::uint8_t> label_by_row;
    std::size_t rows_read = 0;                 // data rows read (header and blank lines excluded)
    std::size_t self_loops = 0;                // kept rows with src == dst
};

// "YYYY/MM/DD HH:MM" -> minutes since the Unix epoch, reading the time as UTC.
// Returns nullopt if the text is not exactly in that format or not a real date.
[[nodiscard]] std::optional<Minute> parse_timestamp(std::string_view s) noexcept;

// Parse an IBM *_Trans.csv. Columns are read by position because the header
// names two of them "Account". Honours opts.time_filter and opts.max_rows.
// Throws ParseError (with the 1-based file line) on malformed input.
[[nodiscard]] ParsedCsv parse_ibm_csv(const std::filesystem::path& trans_csv, const LoadOptions& opts = {});

}  // namespace aml::detail
