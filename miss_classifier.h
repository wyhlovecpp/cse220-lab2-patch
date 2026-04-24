/*
 * CSE220 Lab2 — 3C (compulsory / capacity / conflict) miss classifier.
 *
 * Implemented as a C++ module so dcache_stage.c can call the C-linkage API
 * below.  The classifier uses Hill & Smith's shadow-cache technique:
 *
 *   - compulsory : the line address has never been seen before.
 *   - capacity   : seen before AND a fully-associative LRU cache of the same
 *                  number of lines as the real dcache would also miss.
 *   - conflict   : seen before AND the FA shadow would hit (i.e. the real
 *                  miss is caused purely by the set-associative mapping).
 *
 * Event model — required to make the 3C counters partition DCACHE_MISS_ONPATH
 * 1:1:
 *
 *   - classify_probe() : emit one 3C STAT_EVENT *per* DCACHE_MISS_ONPATH
 *                        event.  This is probe-only: it never mutates the
 *                        shadow state.
 *   - install()        : called when the real dcache actually installs the
 *                        line (dcache_fill_line's SUCCESS return path).
 *                        Adds the line to ever_seen and installs it in the
 *                        FA shadow (LRU insert, possibly evicting LRU).
 *   - observe_hit()    : called on on-path dcache HITS.  LRU-promotes the
 *                        line in the FA shadow.
 *
 * Rationale: the FA shadow must track "what a same-sized FA cache would hold
 * right now."  A secondary pending miss (a second demand miss to a line
 * whose fill has not yet completed) should classify *identically* to the
 * first pending miss — because the real cache still misses and the FA
 * shadow also still misses.  Only installing on fill-success guarantees
 * that.  Probing without state updates on miss guarantees that multiple
 * pending misses to the same line are counted together with the first
 * miss in the same 3C bucket — so that compulsory + capacity + conflict
 * sum to DCACHE_MISS_ONPATH exactly.
 */
#ifndef __MISS_CLASSIFIER_H__
#define __MISS_CLASSIFIER_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the shadow state for this proc_id from the real dcache's
 * geometry.  Called once from init_dcache_stage(). */
void miss_classifier_init(unsigned int proc_id,
                          unsigned int num_lines,
                          unsigned int line_size);

/* Probe-only classification; does NOT mutate shadow state.  Called once per
 * DCACHE_MISS_ONPATH event (i.e. every real on-path demand miss that
 * successfully allocated a mem_req).  Returns tag: 0=compulsory,
 * 1=capacity, 2=conflict. */
int miss_classifier_classify_probe(unsigned int proc_id,
                                   unsigned long long addr);

/* Install a line into the FA shadow (and into ever_seen).  Called when the
 * real dcache actually installs the line (after dcache_fill_process_cacheline
 * succeeds).  LRU-inserts at MRU, evicts LRU if over capacity. */
void miss_classifier_install(unsigned int proc_id,
                             unsigned long long addr);

/* LRU-promote a line in the FA shadow.  Called on every on-path real-cache
 * hit so the shadow's LRU order matches the access stream. */
void miss_classifier_observe_hit(unsigned int proc_id,
                                 unsigned long long addr);

#ifdef __cplusplus
}
#endif

#endif /* __MISS_CLASSIFIER_H__ */
