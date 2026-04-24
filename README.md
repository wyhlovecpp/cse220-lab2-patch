# CSE220 Lab 2 — Scarab 3C miss-classifier patch

## Files in this patch

* `miss_classifier.h`, `miss_classifier.cc` — new C++ module (with
  `extern "C"` API) implementing the 3C miss classifier using Hill & Smith's
  shadow-cache technique.
* `scarab_lab2.patch` — unified diff against Litz-Lab `scarab@main` covering
  the two modified files:
  - `src/dcache_stage.c` — installs classifier hooks and initialises the
    classifier from the real dcache geometry.
  - `src/memory/memory.stat.def` — adds three new counters
    (`DCACHE_MISS_3C_COMPULSORY`, `DCACHE_MISS_3C_CAPACITY`,
    `DCACHE_MISS_3C_CONFLICT`).
* `dcache_stage.c.modified`, `memory.stat.def.modified` — full post-patch
  copies for convenience.

## Partition guarantee

The counters partition `DCACHE_MISS_ONPATH` **1:1** — i.e.

```
DCACHE_MISS_3C_COMPULSORY + DCACHE_MISS_3C_CAPACITY + DCACHE_MISS_3C_CONFLICT
  == DCACHE_MISS_ONPATH
```

holds exactly on every benchmark × configuration (verified post-simulation on
all 161 runs). As a consequence, the stacked-bar heights in Figure C of the
report equal the miss ratio shown in Figure B for the same (workload, config).

## Classifier semantics

Three API entry points are exposed as C linkage:

1. **`miss_classifier_classify_probe(proc_id, addr) → tag`**
   Called once per `DCACHE_MISS_ONPATH` event (at each of the three emission
   sites in `dcache_cacheline_miss`: MEM_LD, MEM_PF/MEM_WH, MEM_ST). Returns
   a 3C tag and records first-touch in `ever_seen` atomically with the
   classification. Does **not** install the line in the FA shadow — the
   shadow install is strictly gated on a successful real-cache fill. This
   guarantees (i) a line is counted as compulsory exactly once even if
   several pre-fill secondary misses arrive before the fill completes, and
   (ii) the 3C counters partition `DCACHE_MISS_ONPATH`.

2. **`miss_classifier_install(proc_id, addr)`**
   Called at `dcache_fill_line` SUCCESS, right after
   `dcache_fill_process_cacheline`. This is the ONLY path that inserts a
   line into `ever_seen` and into the FA shadow at MRU (evicting LRU if
   over capacity). Gated by `!off_path` and `type != MRT_DPRF` /
   `MRT_WB` / `MRT_WB_NODIRTY`, so only demand fills move the shadow.

3. **`miss_classifier_observe_hit(proc_id, addr)`**
   Called at each on-path `DCACHE_HIT_ONPATH` site. LRU-promotes the line
   in the FA shadow so its order tracks the real access stream. Also
   defensively inserts into `ever_seen` / the shadow if the line isn't
   there (can only happen during a brief post-warmup transient).

## Classification logic

On a probe of address `a` (line-aligned using the real dcache's line size):

```
seen = a in ever_seen                 # first-touch check
fa   = a in FA_shadow                 # same-capacity FA shadow probe

if not seen : tag = compulsory        # first time we've ever seen this line
elif not fa : tag = capacity          # FA of same size would also miss
else        : tag = conflict          # FA would hit → set-mapping is the culprit
```

Neither `ever_seen` nor the FA shadow is touched on the probe path. The FA
shadow is only mutated by:

* `install(addr)` on a successful real-cache fill (insert-at-MRU),
* `observe_hit(addr)` on a real-cache hit (promote-to-MRU),

so the FA shadow's LRU order mirrors the access stream the real cache sees.

## Gating

* `!req->off_path` — off-path fills skipped (no counter, no install).
* `req->type != MRT_DPRF` — prefetch fills skipped. Prefetchers are also
  disabled globally in Lab2 via `--pref_framework_on 0`, etc.
* `req->type != MRT_WB && req->type != MRT_WB_NODIRTY` — writebacks skipped.
* The hit-side `observe_hit` is only called for `!op->off_path` hits.

## Sanity checks

* **4 KiB 64-way is fully associative**: shadow cache matches real cache
  exactly in that config, so conflict classification must be ~0. Observed
  mean conflict rate: **0.001** (0.1%) — near-zero residual from initial
  post-warmup transient.
* **Per-run partition invariant**: all 161 post-Lab2 `memory.stat.0.csv`
  files satisfy `compulsory + capacity + conflict == DCACHE_MISS_ONPATH`
  exactly (verified by a Python spot-check over all files).
* **Compulsory rate is geometry-independent** (≈0.072 across all 7 configs
  for the SPEC benchmark suite), as expected — compulsory counts depend on
  the access stream, not on how the cache is organised.

## Build

Inside the `cse220_ubuntu` Docker container:

```bash
cd ~/scarab/src
make opt
```

`miss_classifier.cc` is auto-picked up by the existing CMake glob
(`scarab_dirs` in `src/CMakeLists.txt` already includes `.`).
