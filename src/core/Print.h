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

// A term of a polynomial in s: the coefficient and the power of s.
struct STerm {
    GiNaC::ex coeff;
    int k;
};

// Split a sum into its terms, each with the power of s it carries and the
// rest as its coefficient -- *without* expanding, so an authored grouping
// like R1*(Cds + C1) stays grouped. A term that is not a product of s is a
// constant (k = 0).
std::vector<STerm> split_in_s(const GiNaC::ex& e, const GiNaC::ex& s);

// Like pretty(), but parenthesized when the expression is a sum.
std::string pretty_factor(const GiNaC::ex& e);

// "a*b*c" with each factor parenthesized when needed.
std::string pretty_product(const std::vector<GiNaC::ex>& factors);

// "num / (den)" -- denominator parenthesized when it is a sum.
std::string pretty_ratio(const GiNaC::ex& num, const GiNaC::ex& den,
                         const GiNaC::ex& s);

// Decompose sqrt(X) (X = r*p^2/q^2, r square-free) into an outside factor
// p/q and the remaining radicand r, so a radical can be shown as
// (p/q)*sqrt(r). Returns {outside, radicand}; radicand == 1 means X was a
// perfect square and there is no remaining root. Used by the printers to keep
// a square root to a single, clearly-parenthesised radical.
void split_radical(const GiNaC::ex& X, GiNaC::ex& outside,
                   GiNaC::ex& radicand);

} // namespace syms
