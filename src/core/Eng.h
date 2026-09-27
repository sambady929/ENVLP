#pragma once
#include <string>

namespace syms {
namespace eng {

// Engineering-notation value parsing / formatting.
// "4.7k" -> 4700, "100n" -> 1e-7, "1M" -> 1e6, "1meg" -> 1e6, "10kOhm" -> 1e4.
bool parse_value(const std::string& text, double& out);

// Exact decimal -> rational (numerator, denominator) when `text` is a plain
// decimal or scientific literal (e.g. "0.8" -> 4/5, "1.8e-3" -> 9/5000).
// Returns false if the text carries an SI suffix or is otherwise not a plain
// number. Used to keep DC/AC source values exact in symbolic output instead of
// the binary floating-point rounding a double parse would introduce.
bool parse_exact_decimal(const std::string& text, long long& num,
                         long long& den);

// 1.59e5 -> "1.59e5" (no plus sign, no exponent padding)
std::string format_si(double v, int sig = 3);

// 4700 -> "4.7k", 1e-7 -> "100n", 0.047 -> "47m"
std::string format_eng(double v, int sig = 3);

// A frequency in Hz with a proper unit: 1.59e8 -> "159 MHz", 1e6 -> "1 MHz",
// 0 -> "0 Hz". The prefix is separated from "Hz" by a space.
std::string format_hz(double hz, int sig = 3);

// A frequency in rad/s: 1.59e8 -> "159 Mrad/s".
std::string format_rads(double w, int sig = 3);

std::string format_db(double db, int decimals = 1);

// A percentage as a plain decimal (no scientific notation):
// 56.68 -> "56.68", 0.0084 -> "0.0084". `sig` significant figures.
std::string format_percent(double pct, int sig = 3);

// magnitude conversions: factor = 10^(db/20)
double db_to_factor(double db);
double factor_to_db(double factor);

} // namespace eng
} // namespace syms
