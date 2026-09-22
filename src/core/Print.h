#pragma once
#include <ginac/ginac.h>

// CLN's As() macro corrupts wxWidgets' wxAny::As<T>(); undo it.
#ifdef As
#undef As
#endif

#include <string>
#include <vector>

namespace syms {

// UTF-8 pretty printer tuned for transfer functions.
// Uses '*' for multiplication and unicode superscripts for small powers.

// Generic expression, no outer parentheses at top level.
std::string pretty(const GiNaC::ex& e);

// Polynomial in s printed with descending powers of s:
//   "1 + s*R1*C1", "-gm1*Rd + s*..."
// `s` must be the s symbol registered in the ParamTable.
std::string pretty_in_s(const GiNaC::ex& e, const GiNaC::ex& s);

// Like pretty(), but parenthesized when the expression is a sum.
std::string pretty_factor(const GiNaC::ex& e);

// "a*b*c" with each factor parenthesized when needed.
std::string pretty_product(const std::vector<GiNaC::ex>& factors);

// "num / (den)" -- denominator parenthesized when it is a sum.
std::string pretty_ratio(const GiNaC::ex& num, const GiNaC::ex& den,
                         const GiNaC::ex& s);

} // namespace syms
