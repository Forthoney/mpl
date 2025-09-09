structure ChaseLevDeque =
struct
  val capacity = 64

  fun myWorkerId () =
    MLton.Parallel.processorNumber ()

  fun die strfn =
    ( print (strfn () ^ "\n")
    ; OS.Process.exit OS.Process.failure
    )

  val capacityStr = Int.toString capacity
  fun exceededCapacityError () =
    die (fn _ => "Scheduler error: exceeded max fork depth (" ^ capacityStr ^ ")")

  type gcstate = MLton.Pointer.t
  val gcstate = _prim "GC_state": unit -> gcstate;

  type top = Word64.word
  type bot = Word64.word

  val pushBotFFI = _import "ChaseLev_pushBot" runtime private: gcstate * top ref * bot ref * 'a option array * 'a option -> bool;
  val tryPopBotFFI = _import "ChaseLev_tryPopBot" runtime private: gcstate * top ref * bot ref * 'a option array * 'a option -> 'a option;
  val tryPopTopFFI = _import "ChaseLev_tryPopTop" runtime private: gcstate * top ref * bot ref * 'a option array * 'a option -> 'a option;
  val setDepthFFI = _import "ChaseLev_setDepth" runtime private: gcstate * top ref * bot ref * 'a option array * Word64.word -> unit;

  type 'a t = {data: 'a option array, top: top ref, bot: bot ref}

  fun new () =
    {data = Array.array (capacity, NONE),
     top = ref (0w0: top),
     bot = ref (0w0: bot)}

  fun register ({top, bot, data, ...} : 'a t) p =
    ( MLton.HM.registerQueue (Word32.fromInt p, data)
    ; MLton.HM.registerQueueTop (Word32.fromInt p, top)
    ; MLton.HM.registerQueueBot (Word32.fromInt p, bot)
    )

  fun setDepth (q as {top, bot, data}) d =
    setDepthFFI (gcstate (), top, bot, data, Word64.fromInt d)

  fun pushBot (q as {data, top, bot}) x =
    if pushBotFFI (gcstate (), top, bot, data, SOME x) then ()
    else exceededCapacityError ()

  fun tryPopTop (q as {data, top, bot}) =
    tryPopTopFFI (gcstate (), top, bot, data, NONE)

  fun popBot (q as {data, top, bot}) =
    tryPopBotFFI (gcstate (), top, bot, data, NONE)
end
