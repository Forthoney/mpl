/* Copyright (C) 1999-2005 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 * Copyright (C) 1997-2000 NEC Research Institute.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

/* ---------------------------------------------------------------- */
/*                    Cheney Copying Collection                     */
/* ---------------------------------------------------------------- */

void updateWeaksForCheneyCopy (GC_state s) {
  pointer p;
  GC_weak w;

  for (w = s->weaks; w != NULL; w = w->link) {
    assert (BOGUS_OBJPTR != w->objptr);

    if (DEBUG_WEAK)
      fprintf (stderr, "updateWeaksForCheneyCopy  w = "FMTPTR"  ", (uintptr_t)w);
    p = objptrToPointer (w->objptr, s->heap->start);
    if (GC_FORWARDED == getHeader (p)) {
      if (DEBUG_WEAK)
        fprintf (stderr, "forwarded from "FMTOBJPTR" to "FMTOBJPTR"\n",
                 w->objptr,
                 *(objptr*)p);
      w->objptr = *(objptr*)p;
    } else {
      if (DEBUG_WEAK)
        fprintf (stderr, "cleared\n");
      *(getHeaderp((pointer)w - offsetofWeak (s))) = GC_WEAK_GONE_HEADER;
      w->objptr = BOGUS_OBJPTR;
    }
  }
  s->weaks = NULL;
}

void swapHeapsForCheneyCopy (GC_state s) {
  struct GC_heap tempHeap;

  tempHeap = s->secondaryHeap;
  s->secondaryHeap = s->heap;
  s->heap = tempHeap;
  setCardMapAbsolute (s);
}

void majorCheneyCopyGC (GC_state s) {
  size_t bytesCopied;
  struct rusage ru_start;
  pointer toStart;

  assert (s->secondaryHeap->size >= s->heap->oldGenSize);
  if (detailedGCTime (s))
    startTiming (&ru_start);
  s->cumulativeStatistics->numCopyingGCs++;
  s->forwardState.amInMinorGC = FALSE;
  s->forwardState.toStart = s->secondaryHeap->start;
  s->forwardState.toLimit = s->secondaryHeap->start + s->secondaryHeap->size;
  if (DEBUG or s->controls->messages) {
    fprintf (stderr, "[GC: Major Cheney-copy; from heap at "FMTPTR" of size %s bytes,]\n",
             (uintptr_t)(s->heap->start), 
             uintmaxToCommaString(s->heap->size));
    fprintf (stderr, "[GC:                    to heap at "FMTPTR" of size %s bytes.]\n",
             (uintptr_t)(s->secondaryHeap->start), 
             uintmaxToCommaString(s->secondaryHeap->size));
  }
  assert (s->secondaryHeap->start != (pointer)NULL);
  /* The next assert ensures there is enough space for the copy to
   * succeed.  It does not assert 
   *   (s->secondaryHeap->size >= s->heap->size) 
   * because that is too strong.
   */
  assert (s->secondaryHeap->size >= s->heap->oldGenSize);
  toStart = alignFrontier (s, s->secondaryHeap->start);
  s->forwardState.back = toStart;
  foreachGlobalObjptr (s, forwardObjptr);
  foreachObjptrInRange (s, toStart, &s->forwardState.back, forwardObjptr, TRUE);
  updateWeaksForCheneyCopy (s);
  s->secondaryHeap->oldGenSize = s->forwardState.back - s->secondaryHeap->start;
  bytesCopied = s->secondaryHeap->oldGenSize;
  s->cumulativeStatistics->bytesCopied += bytesCopied;
  swapHeapsForCheneyCopy (s);
  clearCrossMap (s);
  s->lastMajorStatistics->kind = GC_COPYING;
  if (detailedGCTime (s))
    stopTiming (&ru_start, &s->cumulativeStatistics->ru_gcCopy);
  if (DEBUG or s->controls->messages)
    fprintf (stderr, "[GC: Major Cheney-copy done; %s bytes copied.]\n",
             uintmaxToCommaString(bytesCopied));
}

/* ---------------------------------------------------------------- */
/*                 Minor Cheney Copying Collection                  */
/* ---------------------------------------------------------------- */

void minorCheneyCopyGC (GC_state s) {
  size_t bytesAllocated;
  size_t bytesCopied;
  struct rusage ru_start;

  if (DEBUG_GENERATIONAL)
    fprintf (stderr, "minorGC  nursery = "FMTPTR"  frontier = "FMTPTR"\n",
             (uintptr_t)s->heap.nursery, (uintptr_t)s->frontier);
  assert (invariantForGC (s));
  bytesAllocated = s->frontier - s->heap.nursery;
  if (bytesAllocated == 0)
    return;
  s->cumulativeStatistics.bytesAllocated += bytesAllocated;
  if (not s->canMinor) {
    s->heap.oldGenSize += bytesAllocated;
    bytesCopied = 0;
  } else {
    if (DEBUG_GENERATIONAL or s->controls.messages)
      fprintf (stderr, "[GC: Minor Cheney-copy; nursery at "FMTPTR" of size %s bytes,]\n",
               (uintptr_t)(s->heap.nursery),
               uintmaxToCommaString(bytesAllocated));
    if (detailedGCTime (s))
      startTiming (&ru_start);
    s->forwardState.amInMinorGC = TRUE;
    s->forwardState.toStart = s->heap->start + s->heap->oldGenSize;
    if (DEBUG_GENERATIONAL or s->controls->messages)
      fprintf (stderr, "[GC:                    to "FMTPTR".]\n",
               (uintptr_t)(s->forwardState.toStart));
    assert (isFrontierAligned (s, s->forwardState.toStart));
    s->forwardState.toLimit = s->forwardState.toStart + bytesAllocated;
    assert (invariantForGC (s));
    s->cumulativeStatistics->numMinorGCs++;
    s->lastMajorStatistics->numMinorGCs++;
    s->forwardState.back = s->forwardState.toStart;
    /* Forward all globals.  Would like to avoid doing this once all
     * the globals have been assigned.
     */
    foreachGlobalObjptr (s, forwardObjptrIfInNursery);
    forwardInterGenerationalObjptrs (s);
    foreachObjptrInRange (s, s->forwardState.toStart, &s->forwardState.back, 
                          forwardObjptrIfInNursery, TRUE);
    updateWeaksForCheneyCopy (s);
    bytesCopied = s->forwardState.back - s->forwardState.toStart;
    s->cumulativeStatistics->bytesCopiedMinor += bytesCopied;
    s->heap->oldGenSize += bytesCopied;
    if (detailedGCTime (s))
      stopTiming (&ru_start, &s->cumulativeStatistics->ru_gcMinor);
    if (DEBUG_GENERATIONAL or s->controls->messages)
      fprintf (stderr, "[GC: Minor Cheney-copy done; %s bytes copied.]\n",
               uintmaxToCommaString(bytesCopied));
  }
}
