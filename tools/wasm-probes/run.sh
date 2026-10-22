#!/usr/bin/env bash
# Build the CLN-on-wasm pass/fail probe and run it under node.
#
#   bash tools/wasm-probes/run.sh
#
# Requires the wasm dependency prefix to exist (build-web/deps), which
# tools/build_web.sh produces, and node from the Emscripten SDK.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEPS="$ROOT/build-web/deps"
HERE="$ROOT/tools/wasm-probes"
OUT="$ROOT/build"

# The Emscripten SDK, found the same way tools/build_web.sh finds it: em++ has
# to be on PATH, and node comes with the SDK (the wasm module is run by node).
if ! command -v em++ >/dev/null 2>&1; then
    for sdk in "${EMSDK:-}" /c/emsdk /d/emsdk "$HOME/emsdk" /opt/emsdk \
               /usr/local/emsdk; do
        [ -n "$sdk" ] || continue
        [ -f "$sdk/upstream/emscripten/em++.py" ] || continue
        for n in "$sdk"/node/*_64bit "$sdk"/python/*_64bit; do
            if [ -d "$n" ]; then PATH="$n:$PATH"; fi
        done
        PATH="$sdk/upstream/emscripten:$PATH"
        export EM_CONFIG="${EM_CONFIG:-$sdk/.emscripten}"
        break
    done
fi
command -v em++ >/dev/null 2>&1 || {
    echo "em++ not on PATH -- install and activate the Emscripten SDK" >&2
    exit 1
}
command -v node >/dev/null 2>&1 || {
    echo "node not on PATH -- it ships with the Emscripten SDK" >&2
    exit 1
}

mkdir -p "$OUT"

if [ ! -f "$DEPS/lib/libcln.a" ]; then
    echo "missing $DEPS/lib/libcln.a -- run tools/build_web.sh first" >&2
    exit 1
fi

# node here may be a Windows binary, so it needs a Windows path for require().
OUTWIN=$(cd "$OUT" && pwd -W 2>/dev/null || echo "$OUT")

em++ -O0 -g -std=c++17 -I"$DEPS/include" "$HERE/cln_probe.cpp" \
  -L"$DEPS/lib" -lcln -lgmp \
  -sMODULARIZE=1 -sEXPORT_NAME=createClnProbe \
  -sEXPORTED_FUNCTIONS='["_p_pointer_size","_p_word_alignment","_p_value_shift","_p_value_len","_p_intDsize","_p_intLsize","_p_gmp_limb_bits","_p_gmp_nail_bits","_p_gmp_bits_per_limb","_p_gmp_mul_exact","_p_cln_mul_exact","_p_cln_len_k","_p_cln_small_len","_p_cln_ash","_p_cln_parse","_p_cln_rational","_p_i_neg","_p_i_max","_p_i_min","_p_ui_big","_p_ui_max","_p_l_max","_p_l_min","_p_ul_max","_p_ll","_p_ull_big","_p_assign","_p_assign_neg"]' \
  -sEXPORTED_RUNTIME_METHODS='["ccall"]' \
  -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node \
  -o "$OUT/cln_probe.js"

cat > "$OUT/cln_probe_run.js" <<EOF
const factory = require('$OUTWIN/cln_probe.js');
(async () => {
  // A wasm trap poisons the instance and throws, so every call gets a fresh
  // module and is wrapped.
  const call = async (n, k) => {
    try {
      const m = await factory();
      return (k === undefined) ? m.ccall(n, 'number', [], [])
                              : m.ccall(n, 'number', ['number'], [k]);
    } catch (e) { return 'X'; }
  };
  const c1 = (n) => call(n);
  const ck = (n, k) => call(n, k);
  let bad = 0;
  console.log('  --- CLN configuration ---');
  console.log('  pointer_size  = ' + await c1('p_pointer_size')  + '   (want 32)');
  console.log('  word_alignment= ' + await c1('p_word_alignment') + '   (want 4)');
  console.log('  value_shift   = ' + await c1('p_value_shift')   + '   (want 2)');
  console.log('  value_len     = ' + await c1('p_value_len')     + '   (want 30)');
  console.log('  intDsize      = ' + await c1('p_intDsize')      + '   (want 32)');
  console.log('  --- GMP (reference) ---');
  console.log('  limb_bits     = ' + await c1('p_gmp_limb_bits')      + '   (want 32)');
  console.log('  nail_bits     = ' + await c1('p_gmp_nail_bits')      + '   (want 0)');
  console.log('  bits_per_limb = ' + await c1('p_gmp_bits_per_limb')  + '   (want 32)');
  const gm = await c1('p_gmp_mul_exact');
  console.log('  a*a exact     = ' + gm + '   (want 1)'); if (gm !== 1) bad++;
  console.log('  --- CLN ---');
  const cm = await c1('p_cln_mul_exact');
  console.log('  a*a exact     = ' + cm + '   (want 1)'); if (cm !== 1) bad++;
  const sl = await c1('p_cln_small_len');
  console.log('  len(12345)    = ' + sl + '   (want 14)'); if (sl !== 14) bad++;
  const row = [];
  for (let k = 0; k <= 30; k++) {
    let r = 'X';
    try { r = await ck('p_cln_len_k', k); } catch (e) { r = 'X'; }
    if (r !== k + 1) bad++;
    row.push(k + ':' + r);
  }
  console.log('  len(1<<k)     (want k+1):');
  console.log('    ' + row.join('  '));
  for (const [n, want] of [['p_cln_ash', 100], ['p_cln_parse', 60], ['p_cln_rational', 42]]) {
    const r = await c1(n);
    console.log('  ' + n + ' = ' + r + '   (want ' + want + ')');
    if (r !== want) bad++;
  }
  console.log('  --- int/unsigned constructors (regression) ---');
  for (const [n, want, what] of [
      ['p_i_neg', 1, 'cl_I(-1000000007)'],
      ['p_i_max', 1, 'cl_I(2147483647)'],
      ['p_i_min', 1, 'cl_I(INT_MIN)'],
      ['p_ui_big', 1, 'cl_I(0x80000000u)'],
      ['p_ui_max', 1, 'cl_I(0xFFFFFFFFu)'],
      ['p_l_max', 1, 'cl_I(2147483647L)'],
      ['p_l_min', 1, 'cl_I(LONG_MIN)'],
      ['p_ul_max', 1, 'cl_I(0xFFFFFFFFUL)'],
      ['p_ll', 1, 'cl_I(1000000007LL)'],
      ['p_ull_big', 1, 'cl_I(ULLONG_MAX)'],
      ['p_assign', 1, 'x = 1000000007'],
      ['p_assign_neg', 1, 'x = -1000000007']]) {
    const r = await c1(n);
    console.log('  ' + what + ' -> ' + r + '   (want ' + want + ')');
    if (r !== want) bad++;
  }
  console.log('');
  console.log(bad === 0 ? '  RESULT: CLN OK on wasm32'
                        : '  RESULT: ' + bad + ' check(s) failed -- CLN is not usable on wasm32 yet');
  process.exit(bad === 0 ? 0 : 1);
})();
EOF
cd "$OUT" && node cln_probe_run.js
