# CLN-on-wasm32 probe

```bash
bash tools/wasm-probes/run.sh
```

Builds `cln_probe.cpp` against `libcln.a` only — no GiNaC, no ENVLP code — so a
failure can only be CLN or the wasm build of it. It links a fresh module per
check, because a wasm trap poisons the instance. Exit status is 0 only when CLN
is usable.

It is the regression test for the CLN patch that `tools/build_web.sh` applies
(search for `CLNFIX` there). Current output:

```
  --- CLN configuration ---
  pointer_size  = 32   word_alignment= 4   value_shift   = 2
  value_len     = 30   intDsize      = 32                  all as expected
  --- GMP (reference) ---
  limb_bits     = 32   nail_bits     = 0   bits_per_limb = 32
  a*a exact     = 1
  --- CLN ---
  a*a exact     = 1
  len(12345)    = 14
  len(1<<k)     (want k+1):
    0:1  1:2 ... 28:29  29:30  30:31
  ash           = 100
  parse         = 60
  rational      = 42

  RESULT: CLN OK on wasm32
```

## What it caught

`integer_length(cl_I(1<<k))` was correct only up to `k=28`; `1<<29` came back as
29 bits and `1<<30` as **0**. The cause was CLN's `int` constructor
(`CL_DEFINE_INT_CONSTRUCTOR` in `include/cln/number.h`), which packs the argument
into a fixnum with no range check — valid only when `cl_value_len >= 32`, but on a
32-bit target it is 30, so the payload is read as a signed 30-bit value and
anything with bit 29 set is corrupted (`1000000007` became −73741817).

The probe also showed which paths were *fine*: values built from `long` /
`long long`, parsed values, and rationals. That contrast is what located the bug —
the failing inputs were exactly the ones taking the `int` overload.

See `web/README.md`, "CLN on wasm32 (fixed)", for the full write-up.
