/* Copyright (C) 2019-2025 Sam Westrick
 *
 * MLton is released under a HPND-style license.
 * See the file MLton-LICENSE for details.
 */

bool tryClaimLocalScope(GC_state s) {
  objptr result = ChaseLev_tryPopBot(
    s,
    s->wsQueueTop,
    s->wsQueueBot,
    s->wsQueue,
    BOGUS_OBJPTR
  );
  return (result != BOGUS_OBJPTR);
}

void releaseLocalScope(GC_state s, uint64_t originalBot) {
  uint64_t *bot = (uint64_t*)objptrToPointer(s->wsQueueBot, NULL);
  __atomic_store_n(bot, originalBot, __ATOMIC_SEQ_CST);
}

uint64_t pollCurrentLocalScope(GC_state s) {
  uint64_t *bot = (uint64_t*)objptrToPointer(s->wsQueueBot, NULL);
  return __atomic_load_n(bot, __ATOMIC_SEQ_CST);
}
