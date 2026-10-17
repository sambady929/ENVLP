# CLN-on-wasm32 probe

```bash
bash tools/wasm-probes/run.sh
```

Builds `cln_probe.cpp` against `libcln.a` only — no GiNaC, no ENVLP code — so a
failure can only be CLN or the wasm build of it. It links a fresh module per
check, because a wasm trap poisons the instance.

Exit status is 0 only when CLN is usable on wasm32. Current output:

```
  --- CLN configuration ---
  pointer_size  = 32   (want 32)      OK
  word_alignment= 4    (want 4)       OK
  value_shift   = 2    (want 2)       OK
  value_len     = 30   (want 30)      OK
  intDsize      = 32   (want 32)      OK
  --- GMP (reference) ---
  limb_bits     = 32   nail_bits = 0  bits_per_limb = 32     OK
  a*a exact     = 1    (want 1)       OK   <- GMP is fine
  --- CLN ---
  a*a exact     = X    (want 1)       traps in NUDS_to_I (via cl_I("..."))
  len(12345)    = 14   (want 14)      OK
  len(1<<k)     (want k+1):
    0:1 ... 28:29  29:29  30:0        correct to k=28, then breaks
  ash           = X    (want 100)     traps
  parse         = X    (want 61)      traps
  rational      = 42   (want 42)      OK
```

Interpretation:

* CLN's own configuration is right, and **GMP is exact** — so the big-number
  backend is not the problem.
* Everything works until a value needs `cl_value_len` bits (30, the immediate
  payload width from `include/cln/object.h`). `1<<29` comes back as 29 bits and
  `1<<30` as **0** (an empty bignum).
* Integer parsing traps in `NUDS_to_I()`, which writes one digit past its buffer
  *by design* — the caller is supposed to leave a spare limb.

Suspects, all in CLN's digit layer: the 1-digit bignum in
`src/integer/conv/cl_I_from_L.cc` (`allocate_bignum(1)` + `arrayLSref`), the
`need` estimate and `num_stack_alloc` slack in
`src/integer/conv/cl_I_from_digits.cc`, and the `lspref`/`mspref` pointer
conventions in `src/base/digitseq/cl_DS.h` (two mirrored variants selected by
`CL_DS_BIG_ENDIAN_P`, which is correctly undefined here).

Cleared with evidence: `HAVE_DD`/`HAVE_FAST_LONGLONG` (measurements identical
before and after patching it in), the fit test and `minus_bit()` (the mask really
is `-1UL << n`), `mulu32` (the plain `uint64` form), byte order, the exception
ABI, stale objects, stack and memory sizing, and `-O2` (`-O0` removes the traps
but the values are still wrong).

See `web/README.md`, "Known blocker", for the full write-up.
