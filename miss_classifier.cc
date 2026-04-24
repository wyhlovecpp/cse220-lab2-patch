/*
 * CSE220 Lab2 — 3C miss classifier implementation (see miss_classifier.h).
 *
 * Per proc_id shadow state:
 *   ever_seen : unordered_set<line_addr>
 *   fa        : fully-associative LRU cache (list + hash map, O(1) access)
 *
 * Semantics (Hill & Smith, adapted for the out-of-order/MSHR case):
 *   classify_probe(addr) — decide the 3C tag AND record first-touch for
 *                          this line in ever_seen (so a line is classified
 *                          compulsory exactly once, even if several
 *                          pre-fill secondary misses land before the fill
 *                          completes).  Does NOT install the line in the
 *                          FA shadow.
 *   install(addr)        — called when the real dcache installs the line
 *                          (dcache_fill_line SUCCESS).  Inserts at MRU in
 *                          the FA shadow, evicting LRU if necessary.
 *                          Also defensively inserts into ever_seen so the
 *                          shadow stays consistent if the probe-side was
 *                          skipped for any reason.
 *   observe_hit(addr)    — LRU-promote the line in the FA shadow.  Called
 *                          on on-path real-cache hits so the shadow LRU
 *                          order tracks the real access stream.
 */

#include "miss_classifier.h"

#include <cstdint>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct FaCache {
  std::list<uint64_t> lru;  /* MRU at front, LRU at back */
  std::unordered_map<uint64_t, std::list<uint64_t>::iterator> map;
  size_t capacity_lines = 0;

  /* probe: returns true iff the line is currently resident, no mutation. */
  bool probe(uint64_t line) const { return map.find(line) != map.end(); }

  /* install-at-MRU; evict LRU if over capacity.  Undefined behaviour if the
   * line is already resident — call promote() instead in that case. */
  void install(uint64_t line) {
    lru.push_front(line);
    map[line] = lru.begin();
    if (lru.size() > capacity_lines) {
      uint64_t victim = lru.back();
      map.erase(victim);
      lru.pop_back();
    }
  }

  /* promote to MRU (no-op if not resident). */
  void promote(uint64_t line) {
    auto it = map.find(line);
    if (it != map.end()) {
      lru.splice(lru.begin(), lru, it->second);
    }
  }
};

struct ProcState {
  bool initialized = false;
  unsigned int line_shift = 6;
  FaCache fa;
  std::unordered_set<uint64_t> ever_seen;
};

std::vector<ProcState> g_states;

ProcState& state(unsigned int proc_id) {
  if (proc_id >= g_states.size()) {
    g_states.resize(proc_id + 1);
  }
  return g_states[proc_id];
}

unsigned int ilog2(unsigned int v) {
  unsigned int r = 0;
  while ((1u << r) < v) {
    ++r;
  }
  return r;
}

}  // namespace

extern "C" void miss_classifier_init(unsigned int proc_id,
                                     unsigned int num_lines,
                                     unsigned int line_size) {
  ProcState& s = state(proc_id);
  /* Always fully reset: the dcache geometry may have changed across runs of
   * the same binary, and stale shadow state would produce wrong
   * compulsory/capacity classification. */
  s.fa.lru.clear();
  s.fa.map.clear();
  s.ever_seen.clear();
  s.line_shift = ilog2(line_size > 0 ? line_size : 64);
  s.fa.capacity_lines = num_lines;
  s.initialized = true;
}

extern "C" int miss_classifier_classify_probe(unsigned int proc_id,
                                              unsigned long long addr) {
  ProcState& s = state(proc_id);
  /* Caller is required to call miss_classifier_init() before any probe;
   * dcache_stage.c's init_dcache_stage() does this. */
  uint64_t line = static_cast<uint64_t>(addr) >> s.line_shift;

  /* Mark first-touch immediately at the first miss event for this line.
   * Without this, multiple pre-fill misses (MSHR-merged secondaries, same
   * line re-missing before the fill completes) would each be classified
   * compulsory — which contradicts the Hill & Smith definition, where
   * compulsory = first reference to a never-before-seen line. The FA
   * shadow itself is only mutated on fill-success (install) or on
   * real-cache hits (observe_hit); first-touch bookkeeping is cheaper
   * to co-locate here with the classification decision. */
  const bool first_touch = s.ever_seen.insert(line).second;
  if (first_touch) {
    return 0;  /* compulsory */
  }
  const bool fa_hit = s.fa.probe(line);
  if (!fa_hit) {
    return 1;  /* capacity */
  }
  return 2;    /* conflict */
}

extern "C" void miss_classifier_install(unsigned int proc_id,
                                        unsigned long long addr) {
  ProcState& s = state(proc_id);
  uint64_t line = static_cast<uint64_t>(addr) >> s.line_shift;
  s.ever_seen.insert(line);
  /* If somehow already resident (should not happen on a demand fill for a
   * real miss, but could happen if the same line is re-filled without an
   * intervening eviction), promote instead of double-installing. */
  if (s.fa.probe(line)) {
    s.fa.promote(line);
  } else {
    s.fa.install(line);
  }
}

extern "C" void miss_classifier_observe_hit(unsigned int proc_id,
                                            unsigned long long addr) {
  ProcState& s = state(proc_id);
  uint64_t line = static_cast<uint64_t>(addr) >> s.line_shift;
  /* A real-cache hit means the line is definitely in ever_seen; make sure
   * the shadow agrees.  (It can legitimately miss the FA shadow for a very
   * brief post-warmup transient where a line was installed while the
   * shadow was reset.  Insert on first-observed-hit in that case.) */
  s.ever_seen.insert(line);
  if (s.fa.probe(line)) {
    s.fa.promote(line);
  } else {
    s.fa.install(line);
  }
}
