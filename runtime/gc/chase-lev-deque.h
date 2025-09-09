/* Copyright (C) 2025 Seong-Heon Jung
 * 
 * MLton is released under a HPND-style license.
 * See the file MLton-LICENSE for details.
 */

#if (defined (MLTON_GC_INTERNAL_BASIS))

// Try to push `elem_to_push`, returning success or failure if the deque
// is at capacity.
PRIVATE Bool ChaseLev_pushBot(
  GC_state s,
  objptr top,
  objptr bot,
  objptr data,
  objptr elem_to_push
);


PRIVATE objptr ChaseLev_tryPopBot(
  GC_state s,
  objptr top,
  objptr bot,
  objptr data,
  objptr fail_value   // should be the value NONE, to return in case of failure
);


PRIVATE objptr ChaseLev_tryPopTop(
  GC_state s,
  objptr top_op,
  objptr bot_op,
  objptr data_op,
  objptr fail_value   // should be the value NONE, to return in case of failure
);


PRIVATE void ChaseLev_setDepth(
  GC_state s,
  objptr top_op,
  objptr bot_op,
  objptr data_op,
  uint64_t desired_depth
);


#endif /* (defined (MLTON_GC_INTERNAL_BASIS)) */
