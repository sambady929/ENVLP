// Pass/fail probe for CLN on wasm32.
//
// The browser build cannot use CLN until this reports OK. It links against
// libcln.a only (no GiNaC, no ENVLP code), so a failure is CLN's or the build
// of it, never ours.
//
// Build and run with ./run.sh (see the README next to this file).
//
// Every check is exported separately, because a wasm trap poisons the instance:
// the runner makes a fresh module per call.
#include <cln/cln.h>
#include <cln/object.h>
#include <gmp.h>

// --- configuration CLN sees ------------------------------------------------
extern "C" int p_pointer_size(void) { return cl_pointer_size; }
extern "C" int p_word_alignment(void) { return cl_word_alignment; }
extern "C" int p_value_shift(void) { return cl_value_shift; }
extern "C" int p_value_len(void) { return cl_value_len; }
extern "C" int p_intDsize(void) { return intDsize; }
extern "C" int p_intLsize(void) { return intLsize; }

// --- GMP, the reference implementation (expects 32/0/32/32) ----------------
extern "C" int p_gmp_limb_bits(void) { return GMP_LIMB_BITS; }
extern "C" int p_gmp_nail_bits(void) { return GMP_NAIL_BITS; }
extern "C" int p_gmp_bits_per_limb(void) { return (int)mp_bits_per_limb; }

// GMP: is 1000000007^2 exact?  (expects 1)
extern "C" int p_gmp_mul_exact(void) {
    mpz_t a, e;
    mpz_init_set_str(a, "1000000007", 10);
    mpz_init_set_str(e, "1000000014000000049", 10);
    mpz_mul(a, a, a);
    int ok = (mpz_cmp(a, e) == 0);
    mpz_clear(a); mpz_clear(e);
    return ok;
}

// --- CLN: exactness -------------------------------------------------------
// CLN: is 1000000007^2 exact?  (expects 1)
extern "C" int p_cln_mul_exact(void) {
    cln::cl_I a = cln::cl_I(1000000007);
    return ((a * a) == cln::cl_I("1000000014000000049")) ? 1 : 0;
}

// CLN: integer_length(cl_I(1<<k)) -- expects k+1 for every k.
extern "C" int p_cln_len_k(int k) {
    unsigned int v = 1u << k;
    return (int)cln::integer_length(cln::cl_I((int)v));
}

// CLN: a small value, for contrast (expects 14 for 12345).
extern "C" int p_cln_small_len(void) {
    return (int)cln::integer_length(cln::cl_I(12345));
}

// CLN: shift a 60-bit value left by 40 (expects no trap).
extern "C" int p_cln_ash(void) {
    cln::cl_I b = cln::cl_I(1000000007) * cln::cl_I(1000000007);
    return (int)cln::integer_length(cln::ash(b, 40));
}

// CLN: parse a long decimal (expects no trap).
extern "C" int p_cln_parse(void) {
    cln::cl_I a = cln::cl_I("1000000014000000049");
    return (int)cln::integer_length(a);
}

// CLN: rational arithmetic (expects 42).
extern "C" int p_cln_rational(void) {
    cln::cl_RA r = cln::cl_RA(1) / cln::cl_RA(3);
    cln::cl_RA s = r * r;
    return (s == cln::cl_RA(1) / cln::cl_RA(9)) ? 42 : 7;
}

// --- boundary cases for every integer constructor --------------------------
// These are the regression tests for the fix: the int / unsigned int
// constructors used to pack the value into a fixnum without a range check.

extern "C" int p_i_neg(void)   { return (cln::cl_I(-1000000007) == cln::cl_I("-1000000007")) ? 1 : 0; }
extern "C" int p_i_max(void)   { return (cln::cl_I(2147483647) == cln::cl_I("2147483647")) ? 1 : 0; }
extern "C" int p_i_min(void)   { return (cln::cl_I((-2147483647 - 1)) == cln::cl_I("-2147483648")) ? 1 : 0; }
extern "C" int p_ui_big(void)  { return (cln::cl_I((unsigned int)0x80000000u) == cln::cl_I("2147483648")) ? 1 : 0; }
extern "C" int p_ui_max(void)  { return (cln::cl_I((unsigned int)0xFFFFFFFFu) == cln::cl_I("4294967295")) ? 1 : 0; }
extern "C" int p_l_max(void)   { return (cln::cl_I((long)2147483647) == cln::cl_I("2147483647")) ? 1 : 0; }
extern "C" int p_l_min(void)   { return (cln::cl_I((long)(-2147483647 - 1)) == cln::cl_I("-2147483648")) ? 1 : 0; }
extern "C" int p_ul_max(void)  { return (cln::cl_I((unsigned long)0xFFFFFFFFul) == cln::cl_I("4294967295")) ? 1 : 0; }
extern "C" int p_ll(void)      { return (cln::cl_I((long long)1000000007) == cln::cl_I("1000000007")) ? 1 : 0; }
extern "C" int p_ull_big(void) {
    return (cln::cl_I((unsigned long long)18446744073709551615ull) ==
            cln::cl_I("18446744073709551615")) ? 1 : 0;
}
// assignment operators go through the same macros
extern "C" int p_assign(void) {
    cln::cl_I x;
    x = 1000000007;
    const cln::cl_I want("1000000007");
    return (x == want) ? 1 : 0;
}
extern "C" int p_assign_neg(void) {
    cln::cl_I x;
    x = -1000000007;
    const cln::cl_I want("-1000000007");
    return (x == want) ? 1 : 0;
}
