/* Copyright (C) 2025 Seong-Heon Jung
 *
 * MLton is released under a HPND-style license.
 * See the file MLton-LICENSE for details.
 */

#define CAP 64


Bool ChaseLev_pushBot(
  __attribute__ ((unused)) GC_state s,
  objptr top_op,
  objptr bot_op,
  objptr data_op,
  objptr elem_to_push_op)
{
  uint64_t* top = (uint64_t*)objptrToPointer(top_op, NULL);
  uint64_t* bot = (uint64_t*)objptrToPointer(bot_op, NULL);
  objptr* data = (objptr*)objptrToPointer(data_op, NULL);

  uint64_t local_bot = __atomic_load_n(bot, __ATOMIC_RELAXED);
  uint64_t local_top = __atomic_load_n(top, __ATOMIC_ACQUIRE);

  uint64_t size = local_bot - local_top;
  if (size >= getSequenceLength(objptrToPointer(data_op, NULL))) {
    return (Bool)FALSE;
  }

  __atomic_store_n(data + (local_bot % CAP), elem_to_push_op, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_store_n(bot, local_bot+1, __ATOMIC_RELAXED);

  return (Bool)TRUE;
}

objptr ChaseLev_tryPopBot(
  __attribute__ ((unused)) GC_state s,
  objptr top_op,
  objptr bot_op,
  objptr data_op,
  objptr fail_value)
{
  uint64_t* top = (uint64_t*)objptrToPointer(top_op, NULL);
  uint64_t* bot = (uint64_t*)objptrToPointer(bot_op, NULL);
  objptr* data = (objptr*)objptrToPointer(data_op, NULL);

  uint64_t local_bot = __atomic_load_n(bot, __ATOMIC_RELAXED) - 1;
  __atomic_store_n(bot, local_bot, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  uint64_t local_top = __atomic_load_n(top, __ATOMIC_RELAXED);

  if (local_top <= local_bot) {
    objptr elem = __atomic_load_n(data + (local_bot % CAP), __ATOMIC_RELAXED);
    if (local_top == local_bot) {
      if (!__atomic_compare_exchange_n(top, &local_top, local_top + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        elem = fail_value;
      }
      __atomic_store_n(bot, local_bot + 1, __ATOMIC_RELAXED);
    }
    return elem;
  } else {
    __atomic_store_n(bot, local_bot + 1, __ATOMIC_RELAXED);
    return fail_value;
  }
}


objptr ChaseLev_tryPopTop(
  __attribute__ ((unused)) GC_state s,
  objptr top_op,
  objptr bot_op,
  objptr data_op,
  objptr fail_value)
{
  uint64_t* top = (uint64_t*)objptrToPointer(top_op, NULL);
  uint64_t* bot = (uint64_t*)objptrToPointer(bot_op, NULL);
  objptr* data = (objptr*)objptrToPointer(data_op, NULL);

  uint64_t local_top = __atomic_load_n(top, __ATOMIC_ACQUIRE);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  uint64_t local_bot = __atomic_load_n(bot, __ATOMIC_ACQUIRE);

  if (local_top < local_bot) {
    objptr elem = __atomic_load_n(data + (local_top % CAP), __ATOMIC_RELAXED);
    if (!__atomic_compare_exchange_n(top, &local_top, local_top + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
      return fail_value;
    } 
    return elem;
  }
  return fail_value;
}


PRIVATE void ChaseLev_setDepth(
  __attribute__ ((unused)) GC_state s,
  objptr top_op,
  objptr bot_op,
  __attribute__ ((unused)) objptr data_op,
  uint64_t desired_depth)
{
  uint64_t* top = (uint64_t*)objptrToPointer(top_op, NULL);
  uint64_t* bot = (uint64_t*)objptrToPointer(bot_op, NULL);

  uint64_t local_top = __atomic_load_n(top, __ATOMIC_ACQUIRE);
  uint64_t local_bot = __atomic_load_n(bot, __ATOMIC_ACQUIRE);

  if (local_top != local_bot) {
    DIE(
      "Bug! Attempt to set depth of non-empty deque! top=%" PRIu64 "bot=%" PRIu64 "desired_depth=%" PRIu64,
      local_top,
      local_bot,
      desired_depth
    );
  }

  /* Have to make sure the intermediate state of the deque still appears to
   * be empty, i.e., we need to maintain bot index <= top index. So, if the
   * desired depth is smaller, we move the bot first. Otherwise, we move th
   * top first.
   */

  if (desired_depth == local_bot) {
    return;
  }
  else if (desired_depth < local_bot) {
    __atomic_store_n(bot, desired_depth, __ATOMIC_SEQ_CST);
    __atomic_store_n(top, desired_depth, __ATOMIC_SEQ_CST);
  }
  else {
    __atomic_store_n(top, desired_depth, __ATOMIC_SEQ_CST);
    __atomic_store_n(bot, desired_depth, __ATOMIC_SEQ_CST);
  }
}
