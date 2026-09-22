#pragma once
#include <string>

namespace syms {
namespace eng {

// Engineering-notation value parsing / formatting.
// "4.7k" -> 4700, "100n" -> 1e-7, "1M" -> 1e6, "1meg" -> 1e6, "10kOhm" -> 1e4.
bool parse_value(const std::string& text, double& out);

// 1.59e5 -> "1.59e5" (no plus sign, no exponent padding)
std::string format_si(double v, int sig = 3);

// 4700 -> "4.7k", 1e-7 -> "100n", 0.047 -> "47m"
std::string format_eng(double v, int sig = 3);

std::string format_db(double db, int decimals = 1);

// magnitude conversions: factor = 10^(db/20)
double db_to_factor(double db);
double factor_to_db(double factor);

} // namespace eng
} // namespace syms
