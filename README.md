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
   `dcache_fill_process_cacheline`. This is the only path that mutates the
   FA shadow from the miss side (insert-at-MRU, evicting LRU if over
   capacity). It also defensively `ever_seen.insert`s the line — a no-op in
   the normal path because `classify_probe` already did so. Gated by
   `!off_path` and `type != MRT_DPRF` / `MRT_WB` / `MRT_WB_NODIRTY`, so
   only on-path demand fills move the FA shadow.

3. **`miss_classifier_observe_hit(proc_id, addr)`**
   Called at each on-path `DCACHE_HIT_ONPATH` site. Promotes the line to
   MRU in the FA shadow so its LRU order tracks the real access stream.
   Also defensively inserts into `ever_seen` / the FA shadow if the line
   isn't there (repair path; a no-op once steady state is reached).

## Classification logic

On a probe of address `a` (line-aligned using the real dcache's line size):

```
first_touch = ever_seen.insert(a).second   # record first-touch atomically
if first_touch : tag = compulsory          # first time this line is demanded
elif not FA.probe(a) : tag = capacity      # same-sized FA LRU would also miss
else                 : tag = conflict      # FA would hit → set-mapping artefact
```

`classify_probe` records first-touch into `ever_seen`, which is critical:
without it, a never-before-installed line that receives N pre-fill demand
misses (MSHR coalesce, retry, replay) would count as compulsory N times
instead of once. The FA shadow itself is only mutated by `install` on
fill-success and by `observe_hit` on real-cache hits, so its LRU order
mirrors the real access stream.

## Gating

* `!req->off_path` — off-path fills skipped (no install; counter gated
  separately to `!op->off_path` at the miss / hit sites).
* `req->type != MRT_DPRF` — hardware-prefetch fills skipped.
  Prefetchers are disabled globally in Lab2 runs via `--pref_framework_on 0`
  etc., so MRT_DPRF does not occur in practice.
* `req->type != MRT_WB && req->type != MRT_WB_NODIRTY` — writebacks skipped.
* At the 3 miss sites the classifier fires for `DCACHE_MISS_ONPATH` events
  including `MEM_LD`, `MEM_PF`/`MEM_WH`, and `MEM_ST`. `MEM_PF`
  (software prefetch) is included because the lab uses memtrace without
  software-prefetch generation, so this path is inert; the gate mirrors
  Scarab's own `DCACHE_MISS_ONPATH` counter for the 1:1 partition.

## Sanity checks

* **Partition invariant.** All 161 post-Lab2 `memory.stat.0.csv` files
  satisfy `DCACHE_MISS_3C_COMPULSORY + CAPACITY + CONFLICT
  == DCACHE_MISS_ONPATH` to the unit on raw counters
  (verified: OK=161, BAD=0, worst_diff=0). `summary.csv` is printed with
  `%.6g`, so aggregated rows there can show ±1..10 rounding out of 10⁶..10⁷
  — display artefact only.
* **4 KiB 64-way (fully associative).** Conflict rate ≈ 0.001 across 23
  workloads, essentially zero as expected for a real-cache FA config.
* **Compulsory rate.** ≈0.009 across all 7 configs, nearly unchanged
  (first-touch is a property of the access stream; access counts differ
  only ~2 % across configs at fixed 20 M-inst ROI).

## Build

Inside the `cse220_ubuntu` Docker container:

```bash
cd ~/scarab/src
make opt
```

`miss_classifier.cc` is auto-picked up by the existing CMake glob
(`scarab_dirs` in `src/CMakeLists.txt` already includes `.`).
