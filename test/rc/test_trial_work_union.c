#include "rc.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
 enum { N=2048 };
 RcContext *c=rc_context_new();assert(c);rc_set_collection_interval(c,0);
 RcValue **roots=calloc(N,sizeof(*roots));assert(roots);
 /* A rooted self-cycle forces the full trial collector rather than its
  * proven-acyclic fast path for every collection below. */
 RcValue *cycle=rc_vector(c,1);assert(cycle&&rc_vector_set(c,cycle,0,cycle));
 RcStats baseline=rc_stats(c);size_t header=0;
 for(size_t i=0;i<N;i++) {
  RcStats before=rc_stats(c);
  roots[i]=rc_cons(c,rc_nil(),i?roots[i-1]:rc_nil());assert(roots[i]);
  size_t bytes=rc_stats(c).live_bytes-before.live_bytes;
  if(!i)header=bytes;else assert(bytes==header);
 }
#if UINTPTR_MAX == UINT64_MAX
 assert(header==112); /* 64-bit physical header contract (128 before #7). */
#else
 assert(header>0);
#endif
 RcStats before=rc_stats(c);
 assert(before.live_objects==N+1&&before.live_bytes==baseline.live_bytes+N*header);
 assert(before.retain_calls-baseline.retain_calls==(rc_stats_counts_ownership()?N-1:0));
 /* Every pair has its own residual C root; its trial is consumed while the
  * other roots still await discovery. Their work links form a second chain. */
 for(int repeat=0;repeat<8;repeat++) {
  rc_collect(c);RcStats after=rc_stats(c);
  assert(after.live_objects==before.live_objects&&after.live_bytes==before.live_bytes);
  assert(after.total_allocations==before.total_allocations&&after.total_frees==before.total_frees);
  assert(after.retain_calls==before.retain_calls&&after.release_calls==before.release_calls);
  assert(after.cycle_objects_freed==before.cycle_objects_freed);
  for(size_t i=0;i<N;i++) {
   assert(rc_refcount(roots[i])==(i+1<N?2u:1u));
   assert(rc_tail_borrowed(roots[i])==(i?roots[i-1]:rc_nil()));
  }
 }
 /* The final release drains a deep work list after old trial data was
  * overwritten by the collector, preserving exact reference traffic. */
 size_t releases=rc_stats(c).release_calls;
 for(size_t i=0;i<N;i++)rc_release(roots[i]);
 RcStats after=rc_stats(c);
 assert(after.live_objects==1&&after.live_bytes==baseline.live_bytes);
 assert(after.total_frees-before.total_frees==N);
 assert(after.release_calls-releases==(rc_stats_counts_ownership()?2*N-1:0));
 rc_release(cycle);rc_collect(c);after=rc_stats(c);
 assert(after.live_objects==0&&after.live_bytes==0);
 assert(after.cycle_objects_freed==1&&after.total_allocations==after.total_frees);
 free(roots);rc_context_free(c);
 puts("trial/work union: 2048 simultaneous roots, full collections, release work and exact stats PASS");
 return 0;
}
