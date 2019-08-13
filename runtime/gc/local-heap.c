/* Copyright (C) 2014 Ram Raghunathan.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

/**
 * @file local-heap.c
 *
 * @author Ram Raghunathan
 *
 * This file implements the local heap interface defined in
 * local-heap.h.
 */

#include "local-heap.h"

#include "heap-utils.h"

/************************/
/* Function Definitions */
/************************/
void HM_enterLocalHeap (GC_state s) {
  struct HM_HierarchicalHeap* hh = HM_HH_getCurrent(s);

  HM_HH_ensureNotEmpty(hh);
  s->frontier = HM_HH_getFrontier(hh);
  s->limitPlusSlop = HM_HH_getLimit(hh);
  s->limit = s->limitPlusSlop - GC_HEAP_LIMIT_SLOP;
}

void HM_exitLocalHeap (GC_state s) {
  struct HM_HierarchicalHeap* hh = HM_HH_getCurrent(s);

  HM_HH_updateValues(hh, s->frontier);
}

void HM_ensureHierarchicalHeapAssurances(GC_state s,
                                         bool forceGC,
                                         size_t bytesRequested,
                                         bool ensureCurrentLevel) {

  size_t heapBytesFree = s->limitPlusSlop - s->frontier;
  // bool emptyHH = FALSE;
  // bool extend = FALSE;
  bool growStack = FALSE;
  size_t stackBytes = 0;

  LOG(LM_GLOBAL_LOCAL_HEAP, LL_DEBUGMORE,
      "bytesRequested: %zu, heapBytesFree: %zu",
      bytesRequested,
      heapBytesFree);

  // trace pre-collection occupancy before doing anything
  // SAM_NOTE: TODO: removed for now; will need to replace with blocks statistics
  // Trace2(EVENT_CHUNKP_OCCUPANCY, ChunkPool_size(), ChunkPool_allocated());

  if (!invariantForMutatorStack(s)) {
    /* need to grow stack */
    stackBytes =
        sizeofStackWithMetaData(s,
                                sizeofStackGrowReserved (s, getStackCurrent (s)));
    growStack = TRUE;
  }

  /* fetch after management heap GC to make sure that I get the updated value */
  struct HM_HierarchicalHeap* hh = HM_HH_getCurrent(s);

  /* update hh before modification */
  HM_HH_updateValues(hh, s->frontier);

  if (s->limitPlusSlop < s->frontier) {
    DIE("s->limitPlusSlop (%p) < s->frontier (%p)",
        ((void*)(s->limit)),
        ((void*)(s->frontier)));
  }

  uint32_t desiredScope = HM_HH_desiredCollectionScope(s, hh);

  if (forceGC || desiredScope <= hh->level) {
    /* too much allocated, so let's collect */
    HM_HHC_collectLocal(desiredScope);

    hh->bytesAllocatedSinceLastCollection = 0;

    // SAM_NOTE: TODO: removed for now; will need to replace with blocks statistics
    // LOG(LM_GLOBAL_LOCAL_HEAP, LL_INFO,
    //     "%zu/%zu bytes allocated in Chunk Pool after collection",
    //     ChunkPool_allocated(),
    //     ChunkPool_size());

    /* trace post-collection occupancy */
    // SAM_NOTE: TODO: removed for now; will need to replace with blocks statistics
    // Trace2(EVENT_CHUNKP_OCCUPANCY, ChunkPool_size(), ChunkPool_allocated());

    /* I may have reached a new maxHHLCS, so check */
    // if (s->cumulativeStatistics->maxHHLCS < newSize) {
    //   s->cumulativeStatistics->maxHHLCS = newSize;
    // }

    /* I may have reached a new maxHHLHCS, so check */
    // if (s->cumulativeStatistics->maxHHLCHS < hh->collectionThreshold) {
    //   s->cumulativeStatistics->maxHHLCHS = hh->collectionThreshold;
    // }

    if (NULL == hh->lastAllocatedChunk) {
      /* collected everything! */
      s->frontier = NULL;
      s->limitPlusSlop = NULL;
      s->limit = NULL;
      // emptyHH = TRUE;
    } else {
      /* SAM_NOTE: I don't use HM_HH_getFrontier/Limit here, because these have
       * assertions for the chunk frontier invariant, which might be violated
       * here. */
      s->frontier = HM_getChunkFrontier(hh->lastAllocatedChunk);
      s->limitPlusSlop = HM_getChunkLimit(hh->lastAllocatedChunk);
      s->limit = s->limitPlusSlop - GC_HEAP_LIMIT_SLOP;
    }

    /* Thread/stack may have been copied during GC, so need to update */
    setGCStateCurrentThreadAndStack (s);
  }

  /* SAM_NOTE: TODO: clean this shit up */
  if (growStack) {
    LOG(LM_GLOBAL_LOCAL_HEAP, LL_DEBUG,
        "growing stack");
    if (NULL == hh->lastAllocatedChunk ||
        (ensureCurrentLevel && HM_getChunkListLevel(HM_getLevelHead(hh->lastAllocatedChunk)) != hh->level) ||
        HM_getChunkFrontier(hh->lastAllocatedChunk) >= (pointer)hh->lastAllocatedChunk + HM_BLOCK_SIZE ||
        (size_t)(s->limitPlusSlop - s->frontier) < stackBytes)
    {
      if (!HM_HH_extend(hh, stackBytes)) {
        DIE("Ran out of space for Hierarchical Heap!");
      }
      s->frontier = HM_HH_getFrontier(hh);
      s->limitPlusSlop = HM_HH_getLimit(hh);
      s->limit = s->limitPlusSlop - GC_HEAP_LIMIT_SLOP;
    }
    /* SAM_NOTE: growStackCurrent triggers a stack allocation which will
     * guarantee chunk frontier invariants
     */
    growStackCurrent(s);
    /* SAM_NOTE: growing the stack can edit s->frontier, so we need to make sure
     * the saved frontier in the hh is synced. */
    /* SAM_NOTE: TODO: caching the frontier in so many different places is a
     * major headache. We need a refactor. */
    assert(HM_getChunkOf(s->frontier) == hh->lastAllocatedChunk);
    HM_HH_updateValues(hh, s->frontier);
    setGCStateCurrentThreadAndStack(s);
  }

  /* Determine if we need to extend to accommodate bytesRequested (and possibly
   * ensureCurrentLevel */
  if (NULL == hh->lastAllocatedChunk ||
      (ensureCurrentLevel && HM_getChunkListLevel(HM_getLevelHead(hh->lastAllocatedChunk)) != hh->level) ||
      HM_getChunkFrontier(hh->lastAllocatedChunk) >= (pointer)hh->lastAllocatedChunk + HM_BLOCK_SIZE ||
      (size_t)(s->limitPlusSlop - s->frontier) < bytesRequested)
  {
    if (!HM_HH_extend(hh, bytesRequested)) {
      DIE("Ran out of space for Hierarchical Heap!");
    }
    s->frontier = HM_HH_getFrontier(hh);
    s->limitPlusSlop = HM_HH_getLimit(hh);
    s->limit = s->limitPlusSlop - GC_HEAP_LIMIT_SLOP;
  }

  assert(invariantForMutatorFrontier (s));
  assert(invariantForMutatorStack (s));
}
