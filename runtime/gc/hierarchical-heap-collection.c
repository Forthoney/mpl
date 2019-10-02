/* Copyright (C) 2018-2019 Sam Westrick
 * Copyright (C) 2015 Ram Raghunathan.
 *
 * MLton is released under a BSD-style license.
 * See the file MLton-LICENSE for details.
 */

/**
 * @file hierarchical-heap-collection.c
 *
 * @author Ram Raghunathan
 *
 * This file implements the hierarchical heap collection interface described in
 * hierarchical-heap-collection.h.
 */

#include "hierarchical-heap-collection.h"

/******************************/
/* Static Function Prototypes */
/******************************/

void forwardDownPtr(GC_state s, objptr dst, objptr* field, objptr src, void* args);

/**
 * Compute the size of the object, how much of it has to be copied, as well as
 * how much metadata it has.
 *
 * @param s GC state
 * @param p The pointer to copy
 * @param objectSize Where to store the size of the object (in bytes)
 * @param copySize Where to store the number of bytes to copy
 * @param metaDataSize Where to store the metadata size (in bytes)
 *
 * @return the tag of the object
 */
GC_objectTypeTag computeObjectCopyParameters(GC_state s, pointer p,
                                             size_t *objectSize,
                                             size_t *copySize,
                                             size_t *metaDataSize);

/**
 * Copies the object into the new level list of the hierarchical heap provided.
 *
 * @param p The pointer to copy
 * @param objectSize The size of the object
 * @param copySize The number of bytes to copy
 * @param tgtChunkList The ChunkList at which 'p' will be copied.
 *
 * @return pointer to the copied object
 */
pointer copyObject(pointer p,
                   size_t objectSize,
                   size_t copySize,
                   HM_chunkList tgtChunkList);

/**
 * ObjptrPredicateFunction for skipping stacks and threads in the hierarchical
 * heap.
 *
 * @note This function takes as additional arguments the
 * struct SSATOPredicateArgs
 */
struct SSATOPredicateArgs {
  pointer expectedStackPointer;
  pointer expectedThreadPointer;
};
bool skipStackAndThreadObjptrPredicate(GC_state s,
                                       pointer p,
                                       void* rawArgs);

/************************/
/* Function Definitions */
/************************/
#if (defined (MLTON_GC_INTERNAL_BASIS))
#endif /* MLTON_GC_INTERNAL_BASIS */

#if (defined (MLTON_GC_INTERNAL_FUNCS))

void HM_HHC_collectLocal(uint32_t desiredScope, bool force) {
  GC_state s = pthread_getspecific (gcstate_key);
  GC_thread thread = getThreadCurrent(s);
  struct HM_HierarchicalHeap* hh = thread->hierarchicalHeap;

  struct rusage ru_start;
  struct timespec startTime;
  struct timespec stopTime;
  uint64_t oldObjectCopied;

  if (NONE == s->controls->hhCollectionLevel) {
    /* collection disabled */
    return;
  }

  if (s->wsQueueTop == BOGUS_OBJPTR || s->wsQueueBot == BOGUS_OBJPTR) {
    LOG(LM_HH_COLLECTION, LL_INFO, "Skipping collection, deque not registered yet");
    return;
  }

  if (!force && thread->currentDepth <= 1) {
    LOG(LM_HH_COLLECTION, LL_INFO, "Skipping collection during sequential section");
    return;
  }

  uint64_t topval = *(uint64_t*)objptrToPointer(s->wsQueueTop, NULL);
  uint32_t potentialLocalScope = UNPACK_IDX(topval);

  uint32_t originalLocalScope = pollCurrentLocalScope(s);
  uint32_t minLevel = originalLocalScope;
  // claim as many levels as we can, but only as far as desired
  while (minLevel > desiredScope &&
         minLevel > s->controls->hhConfig.minLocalLevel &&
         tryClaimLocalScope(s)) {
    minLevel--;
  }

  if (minLevel == 0) {
    LOG(LM_HH_COLLECTION, LL_INFO, "Skipping collection that includes root heap");
    goto unlock_local_scope_and_return;
  }

  if (minLevel > thread->currentDepth) {
    LOG(LM_HH_COLLECTION, LL_INFO,
        "Skipping collection because minLevel > current level (%u > %u)",
        minLevel,
        thread->currentDepth);
    goto unlock_local_scope_and_return;
  }

  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "START");

  Trace0(EVENT_GC_ENTER);
  TraceResetCopy();

  s->cumulativeStatistics->numHHLocalGCs++;

  /* used needs to be set because the mutator has changed s->stackTop. */
  getStackCurrent(s)->used = sizeofGCStateCurrentStackUsed (s);
  getThreadCurrent(s)->exnStack = s->exnStack;

  int processor = Proc_processorNumber (s);

  HM_debugMessage(s,
                  "[%d] HM_HH_collectLocal(): Starting Local collection on "
                  "HierarchicalHeap = %p\n",
                  processor,
                  ((void*)(hh)));
  HM_debugDisplayHierarchicalHeap(s, hh);

  assertInvariants(s, thread);

  /* copy roots */
  struct ForwardHHObjptrArgs forwardHHObjptrArgs = {
    .hh = hh,
    .minLevel = minLevel,
    .maxLevel = thread->currentDepth,
    .toLevel = HM_HH_INVALID_LEVEL,
    .toSpace = NULL,
    .containingObject = BOGUS_OBJPTR,
    .bytesCopied = 0,
    .objectsCopied = 0,
    .stacksCopied = 0
  };

  if (SUPERLOCAL == s->controls->hhCollectionLevel) {
    forwardHHObjptrArgs.minLevel = thread->currentDepth;
  }

  size_t sizesBefore[HM_MAX_NUM_LEVELS];
  if (LOG_ENABLED(LM_HH_COLLECTION, LL_INFO)) {
    for (uint32_t i = 0; i < HM_MAX_NUM_LEVELS; i++) {
      HM_chunkList lev = HM_HH_LEVEL(hh, i);
      sizesBefore[i] = (lev == NULL) ? 0 : HM_getChunkListSize(lev);
    }
  }

  // if (s->controls->deferredPromotion) {
  Trace0(EVENT_PROMOTION_ENTER);
  if (needGCTime(s)) {
    timespec_now(&startTime);
  }

  HM_chunkList globalDownPtrs = HM_deferredPromote(s, &forwardHHObjptrArgs);

  if (needGCTime(s)) {
    timespec_now(&stopTime);
    timespec_sub(&stopTime, &startTime);
    timespec_add(&(s->cumulativeStatistics->timeLocalPromo), &stopTime);
  }
  Trace0(EVENT_PROMOTION_LEAVE);
  // }

  if (needGCTime(s)) {
    startTiming (RUSAGE_THREAD, &ru_start);
    timespec_now(&startTime);
  }

  LOG(LM_HH_COLLECTION, LL_INFO,
      "collecting hh %p (L: %u):\n"
      "  potential local scope is %u -> %u\n"
      "  collection scope is      %u -> %u\n",
      // "  lchs %"PRIu64" lcs %"PRIu64,
      ((void*)(hh)),
      thread->currentDepth,
      potentialLocalScope,
      thread->currentDepth,
      forwardHHObjptrArgs.minLevel,
      forwardHHObjptrArgs.maxLevel);

  LOG(LM_HH_COLLECTION, LL_DEBUG, "START root copy");

  HM_chunkList toSpace[HM_MAX_NUM_LEVELS];
  for (uint32_t i = 0; i < HM_MAX_NUM_LEVELS; i++) {
    toSpace[i] = NULL;
  }
  forwardHHObjptrArgs.toSpace = &(toSpace[0]);
  forwardHHObjptrArgs.toLevel = HM_HH_INVALID_LEVEL;

  // if (!s->controls->deferredPromotion) {
  //   HM_preserveDownPtrs(s, &forwardHHObjptrArgs);
  // }

  /* forward contents of stack */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  foreachObjptrInObject(s,
                        objptrToPointer(getStackCurrentObjptr(s),
                                        NULL),
                        FALSE,
                        trueObjptrPredicate,
                        NULL,
                        forwardHHObjptr,
                        &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects from stack",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);

  /* forward contents of thread (hence including stack) */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  foreachObjptrInObject(s,
                        objptrToPointer(getThreadCurrentObjptr(s),
                                        NULL),
                        FALSE,
                        trueObjptrPredicate,
                        NULL,
                        forwardHHObjptr,
                        &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects from thread",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);

  /* forward thread itself */
  LOG(LM_HH_COLLECTION, LL_DEBUG,
    "Trying to forward current thread %p",
    (void*)s->currentThread);
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  forwardHHObjptr(s, &(s->currentThread), &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      (1 == (forwardHHObjptrArgs.objectsCopied - oldObjectCopied)) ?
      "Copied thread from GC_state" : "Did not copy thread from GC_state");
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);

  /* forward contents of deque */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  foreachObjptrInObject(s,
                        objptrToPointer(s->wsQueue,
                                        NULL),
                        FALSE,
                        trueObjptrPredicate,
                        NULL,
                        forwardHHObjptr,
                        &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects from deque",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);

  /* preserve remaining down-pointers from global heap */
  LOG(LM_HH_COLLECTION, LL_DEBUG,
    "START forwarding %zu global down-pointers",
    HM_numRemembered(globalDownPtrs));
  HM_foreachRemembered(s, globalDownPtrs, forwardDownPtr, &forwardHHObjptrArgs);
  LOG(LM_HH_COLLECTION, LL_DEBUG, "END forwarding global down-pointers");
  HM_appendChunkList(s->freeListSmall, globalDownPtrs);

  LOG(LM_HH_COLLECTION, LL_DEBUG, "END root copy");

  /* do copy-collection */
  oldObjectCopied = forwardHHObjptrArgs.objectsCopied;
  /*
   * I skip the stack and thread since they are already forwarded as roots
   * above
   */
  struct SSATOPredicateArgs ssatoPredicateArgs = {
    .expectedStackPointer = objptrToPointer(getStackCurrentObjptr(s),
                                            NULL),
    .expectedThreadPointer = objptrToPointer(getThreadCurrentObjptr(s),
                                             NULL)
  };

  /* off-by-one to prevent underflow */
  uint32_t depth = thread->currentDepth+1;
  while (depth > forwardHHObjptrArgs.minLevel) {
    depth--;
    HM_chunkList toSpaceLevel = toSpace[depth];
    if (NULL != toSpaceLevel && NULL != toSpaceLevel->firstChunk) {
      HM_forwardHHObjptrsInChunkList(
        s,
        toSpaceLevel->firstChunk,
        HM_getChunkStart(toSpaceLevel->firstChunk),
        &skipStackAndThreadObjptrPredicate,
        &ssatoPredicateArgs,
        &forwardHHObjptr,
        &forwardHHObjptrArgs);
    }
  }

  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" objects in copy-collection",
      forwardHHObjptrArgs.objectsCopied - oldObjectCopied);
  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "Copied %"PRIu64" stacks in copy-collection",
      forwardHHObjptrArgs.stacksCopied);
  Trace3(EVENT_COPY,
	 forwardHHObjptrArgs.bytesCopied,
	 forwardHHObjptrArgs.objectsCopied,
	 forwardHHObjptrArgs.stacksCopied);


#if ASSERT
  /* clear out memory to quickly catch some memory safety errors */
  FOR_LEVEL_IN_RANGE(level, i, hh, forwardHHObjptrArgs.minLevel, thread->currentDepth+1, {
    HM_chunk chunk = level->firstChunk;
    while (chunk != NULL) {
      pointer start = HM_getChunkStart(chunk);
      size_t length = (size_t)(chunk->limit - start);
      memset(start, 0xBF, length);
      chunk = chunk->nextChunk;
    }

    if (NULL != level->rememberedSet) {
      chunk = level->rememberedSet->firstChunk;
      while (chunk != NULL) {
        pointer start = HM_getChunkStart(chunk);
        size_t length = (size_t)(chunk->limit - start);
        memset(start, 0xBF, length);
        chunk = chunk->nextChunk;
      }
    }
  });
#endif

  /* Free old chunks */
  FOR_LEVEL_IN_RANGE(level, i, hh, forwardHHObjptrArgs.minLevel, thread->currentDepth+1, {
    HM_chunkList remset = level->rememberedSet;
    if (NULL != remset) {
      level->size -= remset->size;
      level->rememberedSet = NULL;
      HM_appendChunkList(s->freeListSmall, remset);
    }
    HM_HH_LEVEL(hh, i) = NULL;
    HM_appendChunkList(s->freeListSmall, level);
  });

  /* merge in toSpace */
  for (uint32_t i = 0; i <= thread->currentDepth; i++) {
    if (NULL == HM_HH_LEVEL(hh, i)) {
      HM_HH_LEVEL(hh, i) = toSpace[i];
      if (NULL != toSpace[i]) {
        toSpace[i]->containingHH = hh;
        toSpace[i]->isInToSpace = FALSE;
      }
    } else {
      HM_appendChunkList(HM_HH_LEVEL(hh, i), toSpace[i]);
    }
  }

  /* update lastAllocatedChunk and associated */
  HM_chunk lastChunk = NULL;
  FOR_LEVEL_DECREASING_IN_RANGE(level, i, hh, 0, thread->currentDepth+1, {
    if (HM_getChunkListLastChunk(level) != NULL) {
      lastChunk = HM_getChunkListLastChunk(level);
      break;
    }
  });
  hh->lastAllocatedChunk = lastChunk;

  if (lastChunk != NULL && !lastChunk->mightContainMultipleObjects) {
    if (!HM_HH_extend(thread, GC_HEAP_LIMIT_SLOP)) {
      DIE("Ran out of space for hierarchical heap!\n");
    }
  }

  /* SAM_NOTE: the following assert is broken, because it is possible that
   * lastChunk == NULL (if we collected everything). Also, note that even when
   * lastChunk != NULL, this assert sometimes trips... which is puzzling,
   * because during collection we are careful to allocate fresh chunks
   * specifically to prevent this.
   *
   * assert(lastChunk->frontier < (pointer)lastChunk + HM_BLOCK_SIZE);
   */

  assertInvariants(s, thread);

  HM_debugMessage(s,
                  "[%d] HM_HH_collectLocal(): Finished Local collection on "
                  "HierarchicalHeap = %p\n",
                  processor,
                  ((void*)(hh)));

  s->cumulativeStatistics->bytesHHLocaled += forwardHHObjptrArgs.bytesCopied;

  /* SAM_NOTE: bytesSurvivedLastCollection is more precise than the
   * corresponding bytesAllocatedSinceLastCollection, which granularizes on
   * chunk boundaries.
   *
   * TODO: IS THIS A PROBLEM?
   */
  thread->bytesSurvivedLastCollection =
    forwardHHObjptrArgs.bytesMoved + forwardHHObjptrArgs.bytesCopied;

  thread->bytesAllocatedSinceLastCollection = 0;

  if (LOG_ENABLED(LM_HH_COLLECTION, LL_INFO)) {
    for (uint32_t i = 0; i < HM_MAX_NUM_LEVELS; i++) {
      size_t sizeBefore = sizesBefore[i];

      HM_chunkList lev = HM_HH_LEVEL(hh, i);
      size_t sizeAfter = (lev == NULL) ? 0 : HM_getChunkListSize(lev);

      if (sizeBefore == 0 && sizeAfter == 0)
        continue;

      const char *sign;
      size_t diff;
      if (sizeBefore > sizeAfter) {
        sign = "-";
        diff = sizeBefore - sizeAfter;
      } else {
        sign = "+";
        diff = sizeAfter - sizeBefore;
      }

      LOG(LM_HH_COLLECTION, LL_INFO,
          "level %u, after collect: %zu --> %zu (%s%zu)",
          i,
          sizeBefore,
          sizeAfter,
          sign,
          diff);
    }
  }

  /* enter statistics if necessary */
  if (needGCTime(s)) {
    if (detailedGCTime(s)) {
      stopTiming(RUSAGE_THREAD, &ru_start, &s->cumulativeStatistics->ru_gcHHLocal);
    }

    /*
     * RAM_NOTE: small extra here since I recompute delta, but probably not a
     * big deal...
     */
    stopTiming(RUSAGE_THREAD, &ru_start, &s->cumulativeStatistics->ru_gc);

    timespec_now(&stopTime);
    timespec_sub(&stopTime, &startTime);
    timespec_add(&(s->cumulativeStatistics->timeLocalGC), &stopTime);
  }

  TraceResetCopy();
  Trace0(EVENT_GC_LEAVE);

  LOG(LM_HH_COLLECTION, LL_DEBUG,
      "END");

unlock_local_scope_and_return:
  releaseLocalScope(s, originalLocalScope);
  return;
}

/* ========================================================================= */

/* SAM_NOTE: TODO: DRY: this code is similar (but not identical) to
 * forwardHHObjptr */
objptr relocateObject(
  GC_state s,
  objptr op,
  HM_chunkList tgtChunkList,
  struct ForwardHHObjptrArgs *args)
{
  pointer p = objptrToPointer(op, NULL);

  assert(!hasFwdPtr(p));
  assert(HM_isLevelHead(tgtChunkList));

  size_t metaDataBytes;
  size_t objectBytes;
  size_t copyBytes;

  /* compute object size and bytes to be copied */
  computeObjectCopyParameters(s,
                              p,
                              &objectBytes,
                              &copyBytes,
                              &metaDataBytes);

  if (!HM_getChunkOf(p)->mightContainMultipleObjects) {
    /* This chunk contains *only* this object, so no need to copy. Instead,
     * just move the chunk. */
    HM_chunk chunk = HM_getChunkOf(p);
    HM_unlinkChunk(chunk);
    HM_appendChunk(tgtChunkList, chunk);
    /* SAM_NOTE: this is inefficient. unnecessary fragmentation. */
    if (!HM_allocateChunk(tgtChunkList, GC_HEAP_LIMIT_SLOP)) {
      DIE("Ran out of space for Hierarchical Heap!");
    }
    LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
      "Moved single-object chunk %p of size %zu",
      (void*)chunk,
      HM_getChunkSize(chunk));
    args->bytesMoved += copyBytes;
    args->objectsMoved++;
    return op;
  }

  pointer copyPointer = copyObject(p - metaDataBytes,
                                   objectBytes,
                                   copyBytes,
                                   tgtChunkList);

  /* Store the forwarding pointer in the old object metadata. */
  *(getFwdPtrp(p)) = pointerToObjptr (copyPointer + metaDataBytes,
                                      NULL);
  assert (hasFwdPtr(p));

  args->bytesCopied += copyBytes;
  args->objectsCopied++;

  /* use the forwarding pointer */
  return getFwdPtr(p);
}

/* ========================================================================= */

void forwardDownPtr(GC_state s, objptr dst, objptr* field, objptr src, void* rawArgs) {
  struct ForwardHHObjptrArgs* args = (struct ForwardHHObjptrArgs*)rawArgs;
  uint32_t srcLevel = HM_getObjptrLevel(src);

  assert(args->minLevel <= srcLevel);
  assert(srcLevel <= args->maxLevel);
  assert(args->toLevel == HM_HH_INVALID_LEVEL);

  forwardHHObjptr(s, &src, rawArgs);
  assert(NULL != args->toSpace[srcLevel]);

  *field = src;
  HM_rememberAtLevel(args->toSpace[srcLevel], dst, field, src);
}

/* ========================================================================= */

void forwardHHObjptr (GC_state s,
                      objptr* opp,
                      void* rawArgs) {
  struct ForwardHHObjptrArgs* args = ((struct ForwardHHObjptrArgs*)(rawArgs));
  objptr op = *opp;
  pointer p = objptrToPointer (op, NULL);
  bool inPromotion = (args->toLevel != HM_HH_INVALID_LEVEL);

  if (DEBUG_DETAILED) {
    fprintf (stderr,
             "forwardHHObjptr  opp = "FMTPTR"  op = "FMTOBJPTR"  p = "
             ""FMTPTR"\n",
             (uintptr_t)opp,
             op,
             (uintptr_t)p);
  }

  LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
      "opp = "FMTPTR"  op = "FMTOBJPTR"  p = "FMTPTR,
      (uintptr_t)opp,
      op,
      (uintptr_t)p);

  if (!isObjptr(op) || isObjptrInRootHeap(s, op)) {
    /* does not point to an HH objptr, so not in scope for collection */
    LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
        "skipping opp = "FMTPTR"  op = "FMTOBJPTR"  p = "FMTPTR": not in HH.",
        (uintptr_t)opp,
        op,
        (uintptr_t)p);
    return;
  }

  struct HM_ObjptrInfo opInfo;
  HM_getObjptrInfo(s, op, &opInfo);

  if (opInfo.level > args->maxLevel) {
    DIE("entanglement detected during %s: %p is at level %u, below %u",
        inPromotion ? "promotion" : "collection",
        (void *)p,
        opInfo.level,
        args->maxLevel);
  }

  /* RAM_NOTE: This is more nuanced with non-local collection */
  if ((opInfo.level > args->maxLevel) ||
      /* cannot forward any object below 'args->minLevel' */
      (opInfo.level < args->minLevel)) {
      LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
          "skipping opp = "FMTPTR"  op = "FMTOBJPTR"  p = "FMTPTR
          ": level %d not in [minLevel %d, maxLevel %d].",
          (uintptr_t)opp,
          op,
          (uintptr_t)p,
          opInfo.level,
          args->minLevel,
          args->maxLevel);
      return;
  }

  assert(HM_getObjptrLevel(op) >= args->minLevel);

  while (hasFwdPtr(p)) {
    op = getFwdPtr(p);
    p = objptrToPointer(op, NULL);
  }

  if (HM_getObjptrLevel(op) < args->minLevel) {
    *opp = op;
    assert(!HM_isObjptrInToSpace(s, op));
  } else if (HM_isObjptrInToSpace(s, op)) {
    *opp = op;
  } else {
    assert(!HM_isObjptrInToSpace(s, op));
    assert(HM_getObjptrLevel(op) >= args->minLevel);
    /* forward the object */
    GC_objectTypeTag tag;
    size_t metaDataBytes;
    size_t objectBytes;
    size_t copyBytes;

    /* compute object size and bytes to be copied */
    tag = computeObjectCopyParameters(s,
                                      p,
                                      &objectBytes,
                                      &copyBytes,
                                      &metaDataBytes);

    switch (tag) {
    case STACK_TAG:
        args->stacksCopied++;
        break;
    case WEAK_TAG:
        die(__FILE__ ":%d: "
            "forwardHHObjptr() does not support WEAK_TAG objects!",
            __LINE__);
        break;
    default:
        break;
    }

    HM_chunkList tgtChunkList = args->toSpace[opInfo.level];

    assert(!inPromotion);
    if (tgtChunkList == NULL) {
      /* Level does not exist, so create it */
      tgtChunkList = HM_newChunkList(COPY_OBJECT_HH_VALUE, opInfo.level);
      /* SAM_NOTE: TODO: This is inefficient, because the current object
       * might be a large object that just needs to be logically moved. */
      if (NULL == HM_allocateChunk(tgtChunkList, objectBytes)) {
        DIE("Ran out of space for Hierarchical Heap!");
      }
      tgtChunkList->isInToSpace = TRUE;
      args->toSpace[opInfo.level] = tgtChunkList;
    }

    assert (!hasFwdPtr(p));

    LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
        "during %s, copying pointer %p at level %u to level list %p",
        (inPromotion ? "promotion" : "collection"),
        (void *)p,
        opInfo.level,
        (void *)tgtChunkList);

    /* SAM_NOTE: TODO: get this spaghetti code out of here.
     * TODO: does this work with promotion? */
    if (!HM_getChunkOf(p)->mightContainMultipleObjects) {
      // assert(FALSE);
      /* This chunk contains *only* this object, so no need to copy. Instead,
       * just move the chunk. */
      assert(!inPromotion);
      assert(!hasFwdPtr(p));
      HM_chunk chunk = HM_getChunkOf(p);
      HM_unlinkChunk(chunk);
      /* SAM_NOTE: TODO: this is inefficient, because we have to abandon the
       * previous last chunk, resulting in unnecessary fragmentation. This can
       * be avoided by not relying upon using the tgtChunkList...lastChunk to
       * allocate the next object, similiar to how hh->lastAllocatedChunk
       * doesn't need to be at the end of its chunk list. */
      /* SAM_NOTE: it is crucial that this is append and not prepend, because
       * traversing the to-space executes left-to-right. */
      HM_appendChunk(tgtChunkList, chunk);
      if (!HM_allocateChunk(tgtChunkList, GC_HEAP_LIMIT_SLOP)) {
        DIE("Ran out of space for Hierarchical Heap!");
      }
      LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
        "Moved single-object chunk %p of size %zu",
        (void*)chunk,
        HM_getChunkSize(chunk));
      args->bytesMoved += copyBytes;
      args->objectsMoved++;
      return;
    }

    pointer copyPointer = copyObject(p - metaDataBytes,
                                     objectBytes,
                                     copyBytes,
                                     tgtChunkList);

    args->bytesCopied += copyBytes;
    args->objectsCopied++;
    LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
        "%p --> %p", ((void*)(p - metaDataBytes)), ((void*)(copyPointer)));

    /* Store the forwarding pointer in the old object metadata. */
    *(getFwdPtrp(p)) = pointerToObjptr (copyPointer + metaDataBytes,
                                        NULL);
    assert (hasFwdPtr(p));

    /* use the forwarding pointer */
    *opp = getFwdPtr(p);

#if ASSERT
    /* args->hh->newLevelList has containingHH set to COPY_OBJECT_HH_VALUE
     * during a copy-collection. */
    HM_getObjptrInfo(s, *opp, &opInfo);
    /* TODO have a more precise assert that also handles the promotion case. */
    assert (inPromotion || COPY_OBJECT_HH_VALUE == opInfo.hh);
#endif
  }

  LOG(LM_HH_COLLECTION, LL_DEBUGMORE,
      "opp "FMTPTR" set to "FMTOBJPTR,
      ((uintptr_t)(opp)),
      *opp);
}
#endif /* MLTON_GC_INTERNAL_FUNCS */

GC_objectTypeTag computeObjectCopyParameters(GC_state s, pointer p,
                                             size_t *objectSize,
                                             size_t *copySize,
                                             size_t *metaDataSize) {
    GC_header header;
    GC_objectTypeTag tag;
    uint16_t bytesNonObjptrs;
    uint16_t numObjptrs;
    header = getHeader(p);
    splitHeader(s, header, &tag, NULL, &bytesNonObjptrs, &numObjptrs);

    /* Compute the space taken by the metadata and object body. */
    if ((NORMAL_TAG == tag) or (WEAK_TAG == tag)) { /* Fixed size object. */
      if (WEAK_TAG == tag) {
        die(__FILE__ ":%d: "
            "computeObjectSizeAndCopySize() #define does not support"
            " WEAK_TAG objects!",
            __LINE__);
      }
      *metaDataSize = GC_NORMAL_METADATA_SIZE;
      *objectSize = bytesNonObjptrs + (numObjptrs * OBJPTR_SIZE);
      *copySize = *objectSize;
    } else if (SEQUENCE_TAG == tag) {
      *metaDataSize = GC_SEQUENCE_METADATA_SIZE;
      *objectSize = sizeofSequenceNoMetaData (s, getSequenceLength (p),
                                              bytesNonObjptrs, numObjptrs);
      *copySize = *objectSize;
    } else {
      /* Stack. */
      bool current;
      size_t reservedNew;
      GC_stack stack;

      assert (STACK_TAG == tag);
      *metaDataSize = GC_STACK_METADATA_SIZE;
      stack = (GC_stack)p;

      /* RAM_NOTE: This changes with non-local collection */
      /* Check if the pointer is the current stack of my processor. */
      current = getStackCurrent(s) == stack;

      reservedNew = sizeofStackShrinkReserved (s, stack, current);
      if (reservedNew < stack->reserved) {
        LOG(LM_HH_COLLECTION, LL_DEBUG,
            "Shrinking stack of size %s bytes to size %s bytes"
            ", using %s bytes.",
            uintmaxToCommaString(stack->reserved),
            uintmaxToCommaString(reservedNew),
            uintmaxToCommaString(stack->used));
        stack->reserved = reservedNew;
      }
      *objectSize = sizeof (struct GC_stack) + stack->reserved;
      *copySize = sizeof (struct GC_stack) + stack->used;
    }

    *objectSize += *metaDataSize;
    *copySize += *metaDataSize;

    return tag;
}

pointer copyObject(pointer p,
                   size_t objectSize,
                   size_t copySize,
                   HM_chunkList tgtChunkList) {
  assert(NULL != tgtChunkList);
  assert(copySize <= objectSize);
  assert(HM_isLevelHead(tgtChunkList));

  /* get the chunk to allocate in */
  HM_chunk chunk = HM_getChunkListLastChunk(tgtChunkList);
  pointer frontier = HM_getChunkFrontier(chunk);
  pointer limit = HM_getChunkLimit(chunk);
  assert(frontier <= limit);

  bool mustExtend = ((size_t)(limit - frontier) < objectSize) ||
                    (frontier >= (pointer)chunk + HM_BLOCK_SIZE);

  if (mustExtend) {
    /* need to allocate a new chunk */
    chunk = HM_allocateChunk(tgtChunkList, objectSize);
    if (NULL == chunk) {
      DIE("Ran out of space for Hierarchical Heap!");
    }
    frontier = HM_getChunkFrontier(chunk);
  }

  GC_memcpy(p, frontier, copySize);
  pointer newFrontier = frontier + objectSize;
  HM_updateChunkValues(chunk, newFrontier);
  if (newFrontier >= (pointer)chunk + HM_BLOCK_SIZE) {
  // if (blockOf(newFrontier) != (pointer)chunk) {
    /* size is arbitrary; just need a new chunk */
    chunk = HM_allocateChunk(tgtChunkList, GC_HEAP_LIMIT_SLOP);
    if (NULL == chunk) {
      DIE("Ran out of space for Hierarchical Heap!");
    }
  }

  return frontier;
}

bool skipStackAndThreadObjptrPredicate(GC_state s,
                                       pointer p,
                                       void* rawArgs) {
  /* silence compliler */
  ((void)(s));

  /* extract expected stack */
  LOCAL_USED_FOR_ASSERT const struct SSATOPredicateArgs* args =
      ((struct SSATOPredicateArgs*)(rawArgs));

  /* run through FALSE cases */
  GC_header header;
  header = getHeader(p);
  if (header == GC_STACK_HEADER) {
    assert(args->expectedStackPointer == p);
    return FALSE;
  } else if (header == GC_THREAD_HEADER) {
    assert(args->expectedThreadPointer == p);
    return FALSE;
  }

  return TRUE;
}
