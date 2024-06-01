/* Copyright (C) 2011-2012,2017,2019-2020,2022 Matthew Fluet.
 * Copyright (C) 1999-2007 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 * Copyright (C) 1997-2000 NEC Research Institute.
 *
 * MLton is released under a HPND-style license.
 * See the file MLton-LICENSE for details.
 */

// #if ASSERT
// bool invariantForGC(__attribute__((unused)) GC_state s) {
//   WARN("Skipping GC invariants check while hierarchical heaps are used.");
//   return TRUE;
// }
// #endif

bool invariantForMutatorFrontier (GC_state s) {
  GC_thread thread = getThreadCurrent(s);
  return (thread->bytesNeeded <= (size_t)(s->limitPlusSlop - s->frontier))
      && (s->frontier == HM_getChunkFrontier(thread->currentChunk))
      && (s->limitPlusSlop == HM_getChunkLimit(thread->currentChunk))
      && s->frontier < (pointer)thread->currentChunk + HM_BLOCK_SIZE - GC_SEQUENCE_METADATA_SIZE
      && (TRUE == HM_getChunkOf(s->frontier)->mightContainMultipleObjects);
}

#if ASSERT
bool strongInvariantForMutatorFrontier (GC_state s) {
  GC_thread thread = getThreadCurrent(s);
  return (thread->bytesNeeded <= (size_t)(s->limitPlusSlop - s->frontier))
      && inSameBlock(s->frontier, s->limitPlusSlop-1)
      && ((HM_chunk)blockOf(s->frontier))->magic == CHUNK_MAGIC;
}
#endif

bool invariantForMutatorStack (GC_state s) {
  GC_stack stack = getStackCurrent(s);
  return (getStackTop (s, stack)
          <= getStackLimit (s, stack) + getStackTopFrameSize (s, stack))
      && (FALSE == HM_getChunkOf((pointer)stack)->mightContainMultipleObjects);
}

#if ASSERT
bool carefulInvariantForMutatorStack(GC_state s) {
  GC_stack stack = getStackCurrent(s);
  GC_returnAddress ra = *((GC_returnAddress*)(getStackTop(s, stack) - GC_RETURNADDRESS_SIZE));
  GC_frameIndex fi = getFrameIndexFromReturnAddress(s, ra);

  bool badfi = fi >= s->frameInfosLength;

  return !badfi && invariantForMutatorStack(s);
}

void displayStackInfo(GC_state s) {
  GC_stack stack = getStackCurrent(s);
  GC_returnAddress ra = *((GC_returnAddress*)(getStackTop(s, stack) - GC_RETURNADDRESS_SIZE));
  GC_frameIndex fi = getFrameIndexFromReturnAddress(s, ra);

  bool badfi = fi >= s->frameInfosLength;
  int fsize = badfi ? -1 : s->frameInfos[fi].size;

  fprintf(stderr,
    "stack bottom %p limit +%zu top +%zu; fi %u; fsize %d\n",
    (void*)getStackBottom(s, stack),
    (size_t)(getStackLimit(s, stack) - getStackBottom(s, stack)),
    (size_t)(getStackTop(s, stack) - getStackBottom(s, stack)),
    fi,
    fsize);
}
#endif

#if ASSERT
bool invariantForMutator (GC_state s, bool frontier, bool stack) {
  if (DEBUG)
    displayGCState (s, stderr);
  if (frontier)
    assert (invariantForMutatorFrontier(s));
  if (stack)
    assert (invariantForMutatorStack(s));
  // assert (invariantForGC (s));
  return TRUE;
}
#endif
