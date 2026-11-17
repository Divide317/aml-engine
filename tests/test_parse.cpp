// Tests for the CSV parsing stage (src/parse.hpp), checked against the Python
// oracle's arrays in fixtures/tiny_expected.hpp.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "fixtures/tiny_expected.hpp"
#include "parse.hpp"

namespace {

const std::filesystem::path kTinyCsv = std::filesystem::path(AML_FIXTURE_DIR) / "tiny_Trans.csv";

constexpr std::string_view kHeader =
    "Timestamp,From Bank,Account,To Bank,Account,Amount Received,Receiving Currency,"
    "Amount Paid,Payment Currency,Payment Format,Is Laundering\n";
constexpr std::string_view kGoodRow =
    "2022/09/01 00:20,10,8000A0000,11,8000B0000,100.00,US Dollar,100.00,US Dollar,ACH,0\n";

// Write `content` to a file in the system temp directory and return its path.
std::filesystem::path write_temp(const std::string& name, std::string_view content) {
    const auto path = std::filesystem::temp_directory_path() / ("aml_test_" + name);
    std::ofstream(path, std::ios::binary) << content;
    return path;
}

// Parse `content` and return the line number of the ParseError it must raise.
std::size_t error_line(const std::string& name, const std::string& content) {
    const auto path = write_temp(name, content);
    try {
        (void)aml::detail::parse_ibm_csv(path);
    } catch (const aml::ParseError& e) {
        return e.line();
    }
    FAIL("expected a ParseError for " << name);
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Timestamps
// ---------------------------------------------------------------------------
TEST_CASE("parse_timestamp converts to minutes since the epoch", "[parse]") {
    using aml::detail::parse_timestamp;
    REQUIRE(parse_timestamp("1970/01/01 00:00") == 0);
    REQUIRE(parse_timestamp("1970/01/02 00:01") == 1441);
    // 2022-09-01 is day 19236 since the epoch: 19236 * 1440 + 20.
    REQUIRE(parse_timestamp("2022/09/01 00:20") == 27'699'860);
    REQUIRE(parse_timestamp("2024/02/29 23:59").has_value());  // leap day
}

TEST_CASE("parse_timestamp rejects anything not in the exact format", "[parse]") {
    using aml::detail::parse_timestamp;
    REQUIRE_FALSE(parse_timestamp(""));
    REQUIRE_FALSE(parse_timestamp("2022-09-01 00:20"));     // wrong separators
    REQUIRE_FALSE(parse_timestamp("2022/09/01 00:20:00"));  // seconds
    REQUIRE_FALSE(parse_timestamp("2022/9/1 00:20"));       // not zero-padded
    REQUIRE_FALSE(parse_timestamp("2022/02/30 00:20"));     // no such date
    REQUIRE_FALSE(parse_timestamp("2023/02/29 00:20"));     // not a leap year
    REQUIRE_FALSE(parse_timestamp("2022/09/01 24:00"));
    REQUIRE_FALSE(parse_timestamp("2022/09/01 00:60"));
    REQUIRE_FALSE(parse_timestamp("2022/09/01 0a:20"));
}

// ---------------------------------------------------------------------------
// The tiny fixture
// ---------------------------------------------------------------------------
TEST_CASE("tiny fixture: counts and node keys match the oracle", "[parse][tiny]") {
    const auto p = aml::detail::parse_ibm_csv(kTinyCsv);

    REQUIRE(p.rows_read == tiny::num_edges);
    REQUIRE(p.edges.size() == tiny::num_edges);
    REQUIRE(p.label_by_row.size() == tiny::num_edges);
    REQUIRE(p.self_loops == tiny::self_loop_eids.size());

    // Node ids are first-seen order (source before destination), so the key
    // array must equal the oracle's element for element.
    REQUIRE(p.node_key.size() == tiny::num_nodes);
    REQUIRE(std::equal(p.node_key.begin(), p.node_key.end(), tiny::node_key.begin()));
}

TEST_CASE("tiny fixture: every row has the oracle's src, dst, time and label", "[parse][tiny]") {
    const auto p = aml::detail::parse_ibm_csv(kTinyCsv);
    REQUIRE(p.edges.size() == tiny::num_edges);

    // The oracle stores edges in CSR order. Walk it node by node: edge e
    // belongs to source u, and src_row[e] says which CSV row it came from.
    for (std::uint32_t u = 0; u < tiny::num_nodes; ++u) {
        for (auto e = tiny::out_offsets[u]; e < tiny::out_offsets[u + 1]; ++e) {
            const std::uint32_t row = tiny::src_row[e];
            INFO("csv row " << row);
            REQUIRE(p.edges[row].src_row == row);  // unfiltered: edges are in file order
            REQUIRE(p.edges[row].src == u);
            REQUIRE(p.edges[row].dst == tiny::out_dst[e]);
            REQUIRE(p.edges[row].t == tiny::out_t[e]);
            REQUIRE(p.label_by_row[row] == tiny::is_laundering[e]);
        }
    }
}

TEST_CASE("tiny fixture: the same account string at two banks is two nodes", "[parse][tiny]") {
    const auto p = aml::detail::parse_ibm_csv(kTinyCsv);
    // CSV row 4: bank 30 / 8000F0000 pays bank 31 / 8000F0000.
    const aml::RawEdge& e = p.edges[4];
    REQUIRE(e.src != e.dst);
    REQUIRE(aml::key_account(p.node_key[e.src]) == aml::key_account(p.node_key[e.dst]));
    REQUIRE(aml::key_bank(p.node_key[e.src]) == 30);
    REQUIRE(aml::key_bank(p.node_key[e.dst]) == 31);
}

TEST_CASE("tiny fixture: amounts, currencies and formats", "[parse][tiny]") {
    const auto p = aml::detail::parse_ibm_csv(kTinyCsv);

    // Dictionaries are in first-seen order.
    REQUIRE(p.currency_names == std::vector<std::string>{"US Dollar", "Euro"});
    REQUIRE(p.format_names == std::vector<std::string>{"Reinvestment", "ACH", "Cash", "Wire", "Credit Card", "Cheque"});

    // CSV row 3 is the cross-currency hop: 4950 USD paid, 4554 EUR received.
    const aml::RawEdge& e = p.edges[3];
    REQUIRE(e.amount_paid == 4950.0);
    REQUIRE(e.amount_received == 4554.0);
    REQUIRE(p.currency_names[e.pay_ccy] == "US Dollar");
    REQUIRE(p.currency_names[e.recv_ccy] == "Euro");
    REQUIRE(p.format_names[e.format] == "Wire");
}

// ---------------------------------------------------------------------------
// Load options
// ---------------------------------------------------------------------------
TEST_CASE("max_rows stops after N data rows", "[parse][options]") {
    aml::LoadOptions opts;
    opts.max_rows = 5;
    const auto p = aml::detail::parse_ibm_csv(kTinyCsv, opts);
    REQUIRE(p.rows_read == 5);
    REQUIRE(p.edges.size() == 5);
    REQUIRE(p.edges.back().src_row == 4);
}

TEST_CASE("time_filter keeps only rows inside the range and keeps ids dense", "[parse][options]") {
    // First ten minutes of the fixture's first day, both ends included.
    const aml::Minute t0 = *aml::detail::parse_timestamp("2022/09/01 00:00");
    aml::LoadOptions opts;
    opts.time_filter = aml::TimeRange{t0, t0 + 10};
    const auto p = aml::detail::parse_ibm_csv(kTinyCsv, opts);

    const auto expected = std::count_if(tiny::out_t.begin(), tiny::out_t.end(),
                                        [&](aml::Minute t) { return opts.time_filter->contains(t); });
    REQUIRE(expected > 0);
    REQUIRE(p.edges.size() == static_cast<std::size_t>(expected));

    REQUIRE(p.rows_read == tiny::num_edges);            // every row was still read...
    REQUIRE(p.label_by_row.size() == tiny::num_edges);  // ...and its label recorded
    REQUIRE(p.node_key.size() < tiny::num_nodes);       // but only kept rows made nodes
    for (const aml::RawEdge& e : p.edges) {
        REQUIRE(opts.time_filter->contains(e.t));
        REQUIRE(e.src < p.node_key.size());
        REQUIRE(e.dst < p.node_key.size());
    }
}

// ---------------------------------------------------------------------------
// Tolerated input variations
// ---------------------------------------------------------------------------
TEST_CASE("Windows line endings and a blank last line are accepted", "[parse]") {
    std::string content(kHeader);
    content += kGoodRow;
    content += kGoodRow;
    std::string crlf;
    for (char c : content) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    crlf += "\r\n";  // blank last line

    const auto p = aml::detail::parse_ibm_csv(write_temp("crlf.csv", crlf));
    REQUIRE(p.edges.size() == 2);
    REQUIRE(p.format_names == std::vector<std::string>{"ACH"});  // no stray '\r' on the last column
    REQUIRE(p.label_by_row == std::vector<std::uint8_t>{0, 0});
}

// ---------------------------------------------------------------------------
// Malformed input: every error names the 1-based file line
// ---------------------------------------------------------------------------
TEST_CASE("malformed input raises ParseError with the right line", "[parse][errors]") {
    const std::string header(kHeader);
    const std::string good(kGoodRow);

    SECTION("missing file") {
        REQUIRE_THROWS_AS(aml::detail::parse_ibm_csv("/no/such/file.csv"), std::runtime_error);
    }
    SECTION("empty file") { REQUIRE(error_line("empty.csv", "") == 1); }
    SECTION("header with a renamed column (what pandas writes)") {
        std::string h = header;
        h.replace(h.find("To Bank,Account"), 15, "To Bank,Account.1");
        REQUIRE(error_line("header.csv", h + good) == 1);
    }
    SECTION("too few fields") {
        REQUIRE(error_line("few.csv", header + good + "2022/09/01 00:20,10,8000A0000\n") == 3);
    }
    SECTION("too many fields") {
        std::string row = good;
        row.insert(row.size() - 1, ",extra");
        REQUIRE(error_line("many.csv", header + row) == 2);
    }
    SECTION("bad timestamp") {
        REQUIRE(error_line("ts.csv", header + good +
                                         "2022-09-01 00:20,10,8000A0000,11,8000B0000,1.00,US Dollar,1.00,US Dollar,ACH,0\n") == 3);
    }
    SECTION("account that is not hex") {
        REQUIRE(error_line("hex.csv",
                           header + "2022/09/01 00:20,10,8000G0000,11,8000B0000,1.00,US Dollar,1.00,US Dollar,ACH,0\n") == 2);
    }
    SECTION("account wider than 36 bits") {
        REQUIRE(error_line("wide.csv",
                           header + "2022/09/01 00:20,10,18000A0000,11,8000B0000,1.00,US Dollar,1.00,US Dollar,ACH,0\n") == 2);
    }
    SECTION("negative bank id") {
        REQUIRE(error_line("bank.csv",
                           header + "2022/09/01 00:20,-1,8000A0000,11,8000B0000,1.00,US Dollar,1.00,US Dollar,ACH,0\n") == 2);
    }
    SECTION("amount with trailing junk") {
        REQUIRE(error_line("amt.csv",
                           header + "2022/09/01 00:20,10,8000A0000,11,8000B0000,1.00x,US Dollar,1.00,US Dollar,ACH,0\n") == 2);
    }
    SECTION("label that is not 0 or 1") {
        REQUIRE(error_line("label.csv",
                           header + "2022/09/01 00:20,10,8000A0000,11,8000B0000,1.00,US Dollar,1.00,US Dollar,ACH,2\n") == 2);
    }
}

TEST_CASE("zero-padded bank ids give the same account as unpadded ones", "[parse]") {
    // The real HI-Small file writes banks as "010" and "03208"; Patterns.txt
    // pads them too. A bank is a number, so "010" and "10" must be one account.
    std::string content(kHeader);
    content += "2022/09/01 00:20,010,8000A0000,11,8000B0000,1.00,US Dollar,1.00,US Dollar,ACH,0\n";
    content += "2022/09/01 00:21,10,8000A0000,0011,8000B0000,1.00,US Dollar,1.00,US Dollar,ACH,0\n";

    const auto p = aml::detail::parse_ibm_csv(write_temp("padded.csv", content));
    REQUIRE(p.node_key.size() == 2);  // two accounts, not four
    REQUIRE(p.edges[0].src == p.edges[1].src);
    REQUIRE(p.edges[0].dst == p.edges[1].dst);
    REQUIRE(aml::key_bank(p.node_key[p.edges[0].src]) == 10);
    REQUIRE(aml::key_bank(p.node_key[p.edges[0].dst]) == 11);
}
