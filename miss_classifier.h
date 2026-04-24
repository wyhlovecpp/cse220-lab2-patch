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
 * Event model (designed so the 3C counters partition DCACHE_MISS_ONPATH):
 *
 *   - classify_probe() : called once per DCACHE_MISS_ONPATH event, at the
 *                        emission site.  Returns the 3C tag and records
 *                        first-touch in ever_seen (so a line is classified
 *                        compulsory exactly once — the first time it is
 *                        ever demanded — even if several pre-fill
 *                        secondary misses arrive before the fill lands).
 *                        Does NOT install the line in the FA shadow.
 *   - install()        : called when the real dcache actually installs the
 *                        line (dcache_fill_line's SUCCESS return path).
 *                        Inserts at MRU in the FA shadow, evicting LRU if
 *                        over capacity.
 *   - observe_hit()    : called on on-path dcache HITS.  Promotes the line
 *                        to MRU in the FA shadow so the shadow's LRU order
 *                        matches the real access stream.
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
