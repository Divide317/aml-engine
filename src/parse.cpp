#include "parse.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_map>

namespace aml::detail {
namespace {

constexpr std::size_t kNumFields = 11;

// The header must match exactly. If IBM ever reorders or renames a column we
// want a loud failure, not silently wrong data.
constexpr std::string_view kHeader =
    "Timestamp,From Bank,Account,To Bank,Account,Amount Received,Receiving Currency,"
    "Amount Paid,Payment Currency,Payment Format,Is Laundering";

// Column positions. Both account columns are called "Account" in the file.
enum Col : std::size_t {
    kTimestamp,
    kFromBank,
    kFromAccount,
    kToBank,
    kToAccount,
    kAmountReceived,
    kRecvCurrency,
    kAmountPaid,
    kPayCurrency,
    kFormat,
    kLabel,
};

// The bank takes whatever the 36 account bits leave free in a 64-bit key.
constexpr int kBankBits = 64 - kAccountBits;

using Fields = std::array<std::string_view, kNumFields>;

// Split on commas into exactly kNumFields views into `line` (no copying).
// The dataset has no quoted fields, so a plain split is correct.
[[nodiscard]] bool split_fields(std::string_view line, Fields& out) noexcept {
    std::size_t n = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (n == kNumFields) return false;  // too many fields
        out[n++] = line.substr(start, comma - start);  // npos - start clamps to the end
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return n == kNumFields;
}

// Parse the whole of `s` as an unsigned integer in `base`. std::from_chars is
// locale-independent, does not allocate, and rejects signs and whitespace.
template <class T>
[[nodiscard]] bool parse_uint(std::string_view s, T& out, int base = 10) noexcept {
    const char* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, out, base);
    return ec == std::errc{} && ptr == end;
}

[[nodiscard]] bool parse_double(std::string_view s, double& out) noexcept {
    const char* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, out);
    return ec == std::errc{} && ptr == end;
}

// Dictionary-encode a string: return its code, adding it if new. There are
// about 15 currencies and 7 formats, so a linear search beats a hash map.
[[nodiscard]] Code intern(std::vector<std::string>& names, std::string_view s, std::size_t line_no) {
    const auto it = std::find(names.begin(), names.end(), s);
    if (it != names.end()) return static_cast<Code>(it - names.begin());
    if (names.size() > std::numeric_limits<Code>::max()) {
        throw ParseError(line_no, "more than 256 distinct currency/format names");
    }
    names.emplace_back(s);
    return static_cast<Code>(names.size() - 1);
}

// (bank, account-hex) text -> packed 64-bit key.
[[nodiscard]] std::uint64_t parse_account_key(std::string_view bank_s, std::string_view account_s,
                                              std::size_t line_no) {
    std::uint32_t bank = 0;
    if (!parse_uint(bank_s, bank) || bank >= (std::uint32_t{1} << kBankBits)) {
        throw ParseError(line_no, "bad bank id '" + std::string(bank_s) + "'");
    }
    std::uint64_t account = 0;
    if (!parse_uint(account_s, account, 16) || account >= (std::uint64_t{1} << kAccountBits)) {
        throw ParseError(line_no, "bad account id '" + std::string(account_s) + "' (need hex, at most 9 digits)");
    }
    return pack_account(bank, account);
}

void strip_carriage_return(std::string& line) noexcept {
    if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate Windows line endings
}

}  // namespace

std::optional<Minute> parse_timestamp(std::string_view s) noexcept {
    // Fixed layout:  YYYY/MM/DD HH:MM
    //                0123456789012345
    if (s.size() != 16 || s[4] != '/' || s[7] != '/' || s[10] != ' ' || s[13] != ':') return std::nullopt;

    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (!parse_uint(s.substr(0, 4), year) || !parse_uint(s.substr(5, 2), month) ||
        !parse_uint(s.substr(8, 2), day) || !parse_uint(s.substr(11, 2), hour) ||
        !parse_uint(s.substr(14, 2), minute)) {
        return std::nullopt;
    }
    if (hour > 23 || minute > 59) return std::nullopt;

    // <chrono> does the calendar arithmetic (leap years, month lengths).
    const std::chrono::year_month_day ymd{std::chrono::year{static_cast<int>(year)}, std::chrono::month{month},
                                          std::chrono::day{day}};
    if (!ymd.ok()) return std::nullopt;  // e.g. 2022/02/30
    const std::int64_t days = std::chrono::sys_days{ymd}.time_since_epoch().count();

    const std::int64_t minutes = days * 1440 + hour * 60 + minute;
    if (minutes < std::numeric_limits<Minute>::min() || minutes > std::numeric_limits<Minute>::max()) {
        return std::nullopt;
    }
    return static_cast<Minute>(minutes);
}

ParsedCsv parse_ibm_csv(const std::filesystem::path& trans_csv, const LoadOptions& opts) {
    // Binary mode: we handle "\r\n" ourselves, the same way on every platform.
    std::ifstream in(trans_csv, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + trans_csv.string());

    std::string line;
    std::size_t line_no = 1;  // 1-based, as an editor shows it
    if (!std::getline(in, line)) throw ParseError(line_no, "file is empty");
    strip_carriage_return(line);
    if (line != kHeader) throw ParseError(line_no, "unexpected header (expected the 11 IBM AML columns)");

    ParsedCsv out;
    std::unordered_map<std::uint64_t, NodeId> id_of_key;

    // Dense node id for a key, assigned in first-seen order.
    const auto intern_node = [&](std::uint64_t key) {
        const auto [it, inserted] = id_of_key.try_emplace(key, static_cast<NodeId>(out.node_key.size()));
        if (inserted) out.node_key.push_back(key);
        return it->second;
    };

    Fields f;
    while (std::getline(in, line)) {
        ++line_no;
        strip_carriage_return(line);
        if (line.empty()) continue;  // e.g. a blank last line
        if (opts.max_rows != 0 && out.rows_read == opts.max_rows) break;

        const std::size_t row = out.rows_read++;  // 0-based data row
        if (row > std::numeric_limits<std::uint32_t>::max()) {
            throw ParseError(line_no, "more than 2^32 rows; src_row is 32-bit");
        }

        // Validate the whole row before deciding whether to keep it, so a
        // malformed row is reported even if the time filter would drop it.
        if (!split_fields(line, f)) throw ParseError(line_no, "expected 11 comma-separated fields");

        const std::optional<Minute> t = parse_timestamp(f[kTimestamp]);
        if (!t) throw ParseError(line_no, "bad timestamp '" + std::string(f[kTimestamp]) + "'");

        const std::uint64_t src_key = parse_account_key(f[kFromBank], f[kFromAccount], line_no);
        const std::uint64_t dst_key = parse_account_key(f[kToBank], f[kToAccount], line_no);

        double amount_received = 0;
        double amount_paid = 0;
        if (!parse_double(f[kAmountReceived], amount_received) || !parse_double(f[kAmountPaid], amount_paid)) {
            throw ParseError(line_no, "bad amount");
        }

        if (f[kLabel] != "0" && f[kLabel] != "1") throw ParseError(line_no, "Is Laundering must be 0 or 1");
        out.label_by_row.push_back(f[kLabel] == "1" ? 1 : 0);

        if (opts.time_filter && !opts.time_filter->contains(*t)) continue;

        // Only kept rows create nodes and dictionary entries, so a time slice
        // still gets dense ids. Source is interned before destination.
        RawEdge e{};
        e.src = intern_node(src_key);
        e.dst = intern_node(dst_key);
        e.t = *t;
        e.amount_paid = amount_paid;
        e.amount_received = amount_received;
        e.recv_ccy = intern(out.currency_names, f[kRecvCurrency], line_no);
        e.pay_ccy = intern(out.currency_names, f[kPayCurrency], line_no);
        e.format = intern(out.format_names, f[kFormat], line_no);
        e.src_row = static_cast<std::uint32_t>(row);

        if (e.src == e.dst) ++out.self_loops;
        out.edges.push_back(e);
    }
    return out;
}

}  // namespace aml::detail
