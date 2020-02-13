(* Copyright (C) 2009,2018,2020 Matthew Fluet.
 * Copyright (C) 1999-2005, 2008 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 * Copyright (C) 1997-2000 NEC Research Institute.
 *
 * MLton is released under a HPND-style license.
 * See the file MLton-LICENSE for details.
 *)

(* This pass must happen before polymorphic equality is implemented because
 * it will make polymorphic equality faster because some types are simpler
 *
 * This pass computes a "cardinality" of each datatype, which is an
 * abstraction of the number of values of the datatype.
 *   Zero means the datatype has no values (except for bottom).
 *   One means the datatype has one value (except for bottom).
 *   Many means the datatype has many values.
 *
 * This pass removes all datatypes whose cardinality is Zero or One
 * and removes
 *   components of tuples
 *   function args
 *   constructor args
 * which are such datatypes.
 *
 * This pass marks constructors as one of
 *   Useless: it never appears in a ConApp.
 *   Transparent: it is the only variant in its datatype
 *     and its argument type does not contain any uses of
 *     Tycon.array or Tycon.vector.
 *   Useful: otherwise
 * This pass also removes Useless and Transparent constructors.
 *
 * We must keep track of Transparent constructors whose argument type
 * uses Tycon.array because of datatypes like the following:
 *   datatype t = T of t array
 * Such a datatype has Cardinality.Many, but we cannot eliminate
 * the datatype and replace the lhs by the rhs, i.e. we must keep the
 * circularity around.
 * Must do similar things for vectors.
 *
 * Also, to eliminate as many Transparent constructors as possible, for
 * something like the following,
 *   datatype t = T of u array
 *   and u = U of t vector
 * we (arbitrarily) expand one of the datatypes first.
 * The result will be something like
 *   datatype u = U of u array array
 * where all uses of t are replaced by u array.
 *)

functor SimplifyTypes (S: SSA_TRANSFORM_STRUCTS): SSA_TRANSFORM =
struct

open S
open Exp Transfer
structure Cardinality =
   struct
      structure L = ThreePointLattice(val bottom = "zero"
                                      val mid = "one"
                                      val top = "many")
      open L

      val isZero = isBottom
      val newZero = new
      val isOne = isMid
      val makeOne = makeMid
      val whenOne = whenMid
      val makeMany = makeTop
      val whenMany = whenTop

      local
         fun mkNew (make: t -> unit) () =
            let val c = newZero ()
            in make c; c end
      in
         val newOne = mkNew makeOne
         val newMany = mkNew makeMany
      end

      val one = newOne ()
      val many = newMany ()

      structure Card =
         struct
            datatype t = Zero | One | Many
            fun toCard c =
               if isZero c then Zero
               else if isOne c then One
               else Many
            fun sum esc (c1, c2) =
               case (toCard c1, c2) of
                  (Zero, c2) => c2
                | (c1, Zero) => c1
                | _ => esc Many
            fun prod esc (c1, c2) =
               case (toCard c1, c2) of
                  (Zero, _) => esc Zero
                | (_, Zero) => esc Zero
                | (One, One) => One
                | _ => Many
         end

      local
         fun make (f, id) cs =
            let
               val c' = newZero ()
               fun doit () =
                  case (Exn.withEscape
                        (fn escape =>
                         Vector.fold (cs, id, f escape))) of
                     Card.Zero => ()
                   | Card.One => makeOne c'
                   | Card.Many => makeMany c'
               val () =
                  Vector.foreach
                  (cs, fn c =>
                   (whenOne (c, doit); whenMany (c, doit)))
               val () = doit ()
            in
               c'
            end
      in
         val sum = make (Card.sum, Card.Zero)
         val prod = make (Card.prod, Card.One)
      end
   end

structure ConRep =
   struct
      datatype t =
         Useless
       | Transparent
       | Useful
       | FFI

      val isFFI =
       fn FFI => true
        | _ => false

      val isUseful =
         fn Useful => true
          | _ => false

      val isUseless =
         fn Useless => true
          | _ => false

      val toString =
         fn Useless => "useless"
          | Transparent => "transparent"
          | Useful => "useful"
          | FFI => "ffi"

      val layout = Layout.str o toString
   end

structure Result =
   struct
      datatype 'a t =
         Dead
       | Delete
       | Keep of 'a

      fun layout layoutX =
         let
            open Layout
         in
            fn Dead => str "Dead"
             | Delete => str "Delete"
             | Keep x => seq [str "Keep ", layoutX x]
         end
   end

fun transform (Program.T {datatypes, globals, functions, main}) =
   let
      val {get = conInfo: Con.t -> {args: Type.t vector,
                                    cardinality: Cardinality.t,
                                    rep: ConRep.t ref},
           set = setConInfo, ...} =
         Property.getSetOnce
         (Con.plist, Property.initRaise ("SimplifyTypes.conInfo", Con.layout))
      val conInfo =
         Trace.trace ("SimplifyTypes.conInfo",
                      Con.layout,
                      fn {args, cardinality, rep} =>
                      Layout.record [("args", Vector.layout Type.layout args),
                                     ("cardinality", Cardinality.layout cardinality),
                                     ("rep", ConRep.layout (!rep))])
         conInfo
      local
         fun make sel = sel o conInfo
         fun make' sel = let val get = make sel
                         in (! o get, fn (c, x) => get c := x)
                         end
      in
         val conArgs = make #args
         val conCardinality = make #cardinality
         val (conRep, setConRep) = make' #rep
      end
      val setConRep =
         Trace.trace2
         ("SimplifyTypes.setConRep", Con.layout, ConRep.layout, Unit.layout)
         setConRep
      val conIsUseful = ConRep.isUseful o conRep
      val conIsUseful =
         Trace.trace
         ("SimplifyTypes.conIsUseful", Con.layout, Bool.layout)
         conIsUseful
      val conIsFFI = ConRep.isFFI o conRep
      val conIsFFI =
         Trace.trace
         ("SimplifyTypes.conIsFFI", Con.layout, Bool.layout)
         conIsFFI
      val {get = tyconInfo: Tycon.t -> {cardinality: Cardinality.t,
                                        ffi: unit -> unit,
                                        numCons: int ref,
                                        replacement: Type.t option ref},
           set = setTyconInfo, ...} =
         Property.getSetOnce
         (Tycon.plist, Property.initRaise ("SimplifyTypes.tyconInfo", Tycon.layout))
      local
         fun make sel = sel o tyconInfo
         fun make' sel = let val get = make sel
                         in (! o get, fn (t, x) => get t := x)
                         end
      in
         val tyconCardinality = make #cardinality
         val tyconFFI = make #ffi
         val (tyconNumCons, setTyconNumCons) = make' #numCons
         val (tyconReplacement, setTyconReplacement) = make' #replacement
      end
      (* Initialize conInfo and typeInfo *)
      val _ =
         Vector.foreach
         (datatypes, fn Datatype.T {tycon, cons} =>
          (setTyconInfo (tycon, {cardinality = Cardinality.newZero (),
                                 ffi = (fn () =>
                                        (Control.diagnostics
                                         (fn display =>
                                          let
                                             open Layout
                                          in
                                             display (seq [str "  tycon: ",
                                                           Tycon.layout tycon])
                                          end);
                                         Vector.foreach
                                         (cons, fn {con, ...} =>
                                          (Control.diagnostics
                                           (fn display =>
                                            let
                                               open Layout
                                            in
                                               display (seq [str "    con: ",
                                                             Con.layout con])
                                            end);
                                           setConRep (con, ConRep.FFI))))),
                                 numCons = ref 0,
                                 replacement = ref NONE});
           Vector.foreach
           (cons, fn {con, args} =>
            setConInfo (con, {args = args,
                              cardinality = Cardinality.newZero (),
                              rep = ref ConRep.Useless}))))
      (* Tentatively mark all constructors appearing in a ConApp or an FFI as Useful
       * (some may later be marked as Useless or Transparent).
       * Mark any tycons created by MLton_bogus as cardinality Many.
       *)
      val _ =
         let
            fun setConRepUseful c = setConRep (c, ConRep.Useful)
            fun handleStatement (Statement.T {exp, ...}) =
               case exp of
                  ConApp {con, ...} =>
                     if not (conIsFFI con)
                        then setConRep (con, ConRep.Useful)
                        else ()
                | PrimApp {prim, targs, ...} =>
                     let
                        fun deepSetFFI t =
                           case Type.dest t of
                              Type.Array t => deepSetFFI t
                            | Type.Datatype tycon => tyconFFI tycon ()
                            | Type.Ref t => deepSetFFI t
                            | Type.Tuple tv => Vector.foreach(tv, deepSetFFI)
                            | Type.Vector t => deepSetFFI t
                            | Type.Weak t => deepSetFFI t
                            | _ => ()
                     in
                        case Prim.name prim of
                           Prim.Name.FFI cfunction =>
                              (Control.diagnostics
                               (fn display =>
                                let
                                   open Layout
                                in
                                   display (seq [str "deepSetFFI for ",
                                                 str (CFunction.cPrototype cfunction)])
                                end);
                               Vector.foreach (CFunction.args cfunction, deepSetFFI);
                               deepSetFFI (CFunction.return cfunction))
                         | Prim.Name.MLton_bogus =>
                              (case Type.dest (Vector.sub (targs, 0)) of
                                  Type.Datatype tycon =>
                                     Cardinality.makeMany (tyconCardinality tycon)
                                | _ => ())
                         | _ => ()
                   end
                 | _ => ()
            (* Booleans are special because they are generated by primitives. *)
            val _ = setConRepUseful Con.truee
            val _ = setConRepUseful Con.falsee
            val _ = Vector.foreach (globals, handleStatement)
            val _ = List.foreach
                    (functions, fn f =>
                     Vector.foreach
                     (Function.blocks f, fn Block.T {statements, ...} =>
                      Vector.foreach (statements, handleStatement)))
         in ()
         end
      (* Compute the type cardinalities with a fixed point
       * over the Cardinality lattice.
       *)
      val {get = typeCardinality, destroy = destroyTypeCardinality} =
         Property.destGet
         (Type.plist,
          Property.initRec
          (fn (t, typeCardinality) =>
           let
              fun ptrCard t =
                 (Cardinality.prod o Vector.new2)
                 (typeCardinality t, Cardinality.many)
              fun tupleCard ts =
                 (Cardinality.prod o Vector.map)
                 (ts, typeCardinality)
              fun vecCard t =
                 (Cardinality.sum o Vector.new2)
                 (Cardinality.one,
                  (Cardinality.prod o Vector.new2)
                  (typeCardinality t, Cardinality.many))
              datatype z = datatype Type.dest
           in
              case Type.dest t of
                 Array _ => Cardinality.many
               | CPointer => Cardinality.many
               | Datatype tycon => tyconCardinality tycon
               | IntInf => Cardinality.many
               | Real _ => Cardinality.many
               | Ref t => ptrCard t
               | Thread => Cardinality.many
               | Tuple ts => tupleCard ts
               | Vector t => vecCard t
               | Weak t => ptrCard t
               | Word _ => Cardinality.many
           end))
      (* Remove useless constructors from datatypes.
       * Remove datatypes which have no cons.
       * Lower-bound cardinality of cons by product of arguments.
       * Lower-bound cardinality of tycons by sum of cons.
       *)
      val origDatatypes = datatypes
      val datatypes =
         Vector.keepAllMap
         (datatypes, fn Datatype.T {tycon, cons} =>
          let
             val cons = Vector.keepAll (cons, fn {con, ...} =>
                                        conIsUseful con
                                        orelse conIsFFI con)
             val _ =
                Cardinality.<=
                (Cardinality.sum
                 (Vector.map (cons, conCardinality o #con)),
                 tyconCardinality tycon)
             val _ =
                Vector.foreach
                (cons, fn {con, args} =>
                 Cardinality.<=
                 (Cardinality.prod
                  (Vector.map (args, typeCardinality)),
                  conCardinality con))
          in
             if Vector.isEmpty cons
                then NONE
                else SOME (Datatype.T {tycon = tycon, cons = cons})
          end)
      (* diagnostic *)
      val _ =
         Control.diagnostics
         (fn display =>
          let
             open Layout
          in
             display (Layout.str "isFFI");
             Vector.foreach
             (datatypes,
              fn Datatype.T {tycon, cons} =>
              (display (Tycon.layout tycon);
               Vector.foreach
               (cons,
                fn {con, ...} =>
                display (seq [str "   ",
                              Con.layout con,
                              str " ",
                              str (Bool.toString (conIsFFI con))]))))
          end)
      val _ =
         Control.diagnostics
         (fn display =>
          let
             open Layout
          in
             Vector.foreach
             (origDatatypes, fn Datatype.T {tycon, cons} =>
              (display (seq [str "cardinality of ",
                             Tycon.layout tycon,
                             str " = ",
                             Cardinality.layout (tyconCardinality tycon)]);
               Vector.foreach
               (cons, fn {con, ...} =>
                (display (seq [str "cardinality of ",
                               Con.layout con,
                               str " = ",
                               Cardinality.layout (conCardinality con)])))))
          end)
      fun transparent (tycon, con, args) =
         (setTyconReplacement (tycon, SOME (Type.tuple args))
          ; setConRep (con, ConRep.Transparent)
          ; setTyconNumCons (tycon, 1))
      (* "unary" is datatypes with one constructor whose rhs contains an
       * array (or vector) type.
       * For datatypes with one variant not containing an array type, eliminate
       * the datatype unless it is an FFI type.
       *)
      fun containsArrayOrVector (ty: Type.t): bool =
         let
            datatype z = datatype Type.dest
            fun loop t =
               case Type.dest t of
                  Array _ => true
                | Ref t => loop t
                | Tuple ts => Vector.exists (ts, loop)
                | Vector _ => true
                | Weak t => loop t
                | _ => false
         in loop ty
         end
      val (datatypes, unary) =
         Vector.fold
         (datatypes, ([], []), fn (Datatype.T {tycon, cons}, (datatypes, unary)) =>
          let
             (* remove all cons with zero cardinality and mark them as useless *)
             val cons =
                Vector.keepAllMap
                (cons, fn c as {con, ...} =>
                 if Cardinality.isZero (conCardinality con)
                    then (setConRep (con, ConRep.Useless)
                          ; NONE)
                    else SOME c)

             val _ =
                Control.diagnostics
                (fn display =>
                 let
                    open Layout
                 in
                    display (Layout.str "Remaining numCons");
                    display (seq [Tycon.layout tycon,
                                  str " = ",
                                  str (Int.toString (Vector.length cons))])
                 end)
          in
             case Vector.length cons of
                0 => (datatypes, unary)
              | 1 =>
                   let
                      val {con, args} = Vector.first cons
                   in
                      if conIsFFI con
                         then (Datatype.T {tycon = tycon, cons = cons} :: datatypes,
                               unary)
                      else if Vector.exists (args, containsArrayOrVector)
                         then (datatypes,
                               {tycon = tycon, con = con, args = args} :: unary)
                         else (transparent (tycon, con, args)
                               ; (datatypes, unary))
                   end
              | _ => (Datatype.T {tycon = tycon, cons = cons} :: datatypes,
                      unary)
          end)

      val _ =
         Control.diagnostics
         (fn display =>
          let
             open Layout
          in
             display (Layout.str "Remaining datatypes 1");
             display (Layout.str "datatypes");
             Vector.foreach
             (Vector.fromList datatypes, fn Datatype.T {tycon, ...} =>
              display (Tycon.layout tycon));
             display (Layout.str "unary");
             Vector.foreach
             (Vector.fromList unary, fn {tycon, ...} =>
              display (Tycon.layout tycon))
          end)

      (* convert FFI datatypes back into useful *)
      val _ =
         List.foreach
         (datatypes, fn Datatype.T {cons, ...} =>
          Vector.foreach
          (cons, fn {con, ...} =>
           if conIsFFI con
              then setConRep (con, ConRep.Useful)
              else ()))

      fun containsTycon (ty: Type.t, tyc: Tycon.t): bool =
         let
            datatype z = datatype Type.dest
            val {get = containsTycon, destroy = destroyContainsTycon} =
               Property.destGet
               (Type.plist,
                Property.initRec
                (fn (t, containsTycon) =>
                 case Type.dest t of
                    Array t => containsTycon t
                  | Datatype tyc' =>
                       (case tyconReplacement tyc' of
                           NONE => Tycon.equals (tyc, tyc')
                         | SOME t => containsTycon t)
                  | Tuple ts => Vector.exists (ts, containsTycon)
                  | Ref t => containsTycon t
                  | Vector t => containsTycon t
                  | Weak t => containsTycon t
                  | _ => false))
            val res = containsTycon ty
            val () = destroyContainsTycon ()
         in res
         end
      (* Keep the circular transparent tycons, ditch the rest. *)
      val datatypes =
         List.fold
         (unary, datatypes, fn ({tycon, con, args}, accum) =>
          if Vector.exists (args, fn arg => containsTycon (arg, tycon))
             then Datatype.T {tycon = tycon,
                              cons = Vector.new1 {con = con, args = args}}
                  :: accum
          else (transparent (tycon, con, args)
                ; accum))

      val _ =
         Control.diagnostics
         (fn display =>
          let
             open Layout
          in
             display (Layout.str "Remaining datatypes 2");
             Vector.foreach
             (Vector.fromList datatypes, fn Datatype.T {tycon, ...} =>
              display (Tycon.layout tycon))
          end)

      val void = Tycon.newString "void"

      fun makeSimplifyTypeFns simplifyTypeOpt =
         let
            fun simplifyType t =
               case simplifyTypeOpt t of
                  NONE => Error.bug (concat ["SimplifyTypes.simplifyType: ",
                                             Layout.toString (Type.layout t)])
                | SOME t => t
            fun simplifyTypeAsVoid t =
               case simplifyTypeOpt t of
                  NONE => Type.datatypee void
                | SOME t => t
            fun simplifyTypesOpt ts =
               Exn.withEscape
               (fn escape =>
                SOME (Vector.map (ts, fn t =>
                                  case simplifyTypeOpt t of
                                     NONE => escape NONE
                                   | SOME t => t)))
            fun simplifyTypes ts = Vector.map (ts, simplifyType)
            fun keepSimplifyTypes ts = Vector.keepAllMap (ts, simplifyTypeOpt)
         in
            {simplifyType = simplifyType,
             simplifyTypeAsVoid = simplifyTypeAsVoid,
             simplifyTypes = simplifyTypes,
             simplifyTypesOpt = simplifyTypesOpt,
             keepSimplifyTypes = keepSimplifyTypes}
         end
      val {get = simplifyTypeOpt, destroy = destroySimplifyTypeOpt} =
         Property.destGet
         (Type.plist,
          Property.initRec
          (fn (t, simplifyTypeOpt) =>
           if Cardinality.isZero (typeCardinality t)
              then NONE
              else SOME (let
                            val {simplifyType, simplifyTypeAsVoid, simplifyTypes, ...} =
                               makeSimplifyTypeFns simplifyTypeOpt
                            open Type
                         in
                            case dest t of
                               Array t => array (simplifyTypeAsVoid t)
                             | Datatype tycon =>
                                  (case tyconReplacement tycon of
                                      SOME t =>
                                         let
                                            val t = simplifyType t
                                            val _ = setTyconReplacement (tycon, SOME t)
                                         in
                                            t
                                         end
                                    | NONE => t)
                             | Ref t => reff (simplifyType t)
                             | Tuple ts => Type.tuple (simplifyTypes ts)
                             | Vector t => vector (simplifyTypeAsVoid t)
                             | Weak t => weak (simplifyType t)
                             | _ => t
                         end)))
      val simplifyTypeOpt =
         Trace.trace ("SimplifyTypes.simplifyTypeOpt", Type.layout, Option.layout Type.layout)
         simplifyTypeOpt
      val {simplifyTypes, keepSimplifyTypes, ...} =
         makeSimplifyTypeFns simplifyTypeOpt
      (* Simplify constructor argument types. *)
      val datatypes =
         Vector.fromListMap
         (datatypes, fn Datatype.T {tycon, cons} =>
          (setTyconNumCons (tycon, Vector.length cons)
           ; Datatype.T {tycon = tycon,
                         cons = Vector.map (cons, fn {con, args} =>
                                            {con = con,
                                             args = keepSimplifyTypes args})}))
      val datatypes =
         Vector.concat
         [Vector.new1 (Datatype.T {tycon = void, cons = Vector.new0 ()}),
          datatypes]
      val unitVar = Var.newNoname ()
      val {get = varInfo: Var.t -> Type.t, set = setVarInfo, ...} =
         Property.getSetOnce
         (Var.plist, Property.initRaise ("varInfo", Var.layout))
      fun simplifyVarType (x: Var.t, t: Type.t): Type.t option =
         (setVarInfo (x, t)
          ; simplifyTypeOpt t)
      fun simplifyMaybeVarType (x: Var.t option, t: Type.t): Type.t option =
         case x of
            SOME x => simplifyVarType (x, t)
          | NONE => simplifyTypeOpt t
      val oldVarType = varInfo
      val newVarType = simplifyTypeOpt o oldVarType
      val varIsUseless = Option.isNone o newVarType
      fun removeUselessVars xs = Vector.keepAll (xs, not o varIsUseless)
      fun tuple xs =
         let
            val xs = removeUselessVars xs
         in
            if 1 = Vector.length xs
               then Var (Vector.first xs)
               else Tuple xs
         end
      fun simplifyFormalsOpt xts =
         Exn.withEscape
         (fn escape =>
          SOME (Vector.map
                (xts, fn (x, t) =>
                 case simplifyVarType (x, t) of
                    NONE => escape NONE
                  | SOME t => (x, t))))
      val typeIsUseful = Option.isSome o simplifyTypeOpt
      datatype result = datatype Result.t
      fun simplifyExp (e: Exp.t): Exp.t result =
         case e of
            ConApp {con, args} =>
               (case conRep con of
                   ConRep.Transparent => Keep (tuple args)
                 | ConRep.Useful =>
                      Keep (ConApp {con = con,
                                    args = removeUselessVars args})
                 | ConRep.Useless => Dead
                 | ConRep.FFI => Dead)
          | PrimApp {prim, targs, args} =>
               Keep
               (let
                   fun normal () =
                      PrimApp {prim = prim,
                               targs = simplifyTypes targs,
                               args = args}
                   fun length () =
                      case simplifyTypeOpt (Vector.first targs) of
                         NONE => Exp.Const (Const.word (WordX.zero (WordSize.seqIndex ())))
                       | SOME _ => normal ()
                   datatype z = datatype Prim.Name.t
                in
                   case Prim.name prim of
                      Array_length => length ()
                    | Vector_length => length ()
                    | _ => normal ()
                end)
          | Select {tuple, offset} =>
               let
                  val ts = Type.deTuple (oldVarType tuple)
               in
                  Vector.fold'
                  (ts, 0, (offset, 0), fn (pos, t, (n, offset)) =>
                   if n = 0
                      then (Vector.Done
                            (Keep
                             (if offset = 0
                                 andalso not (Vector.existsR
                                              (ts, pos + 1, Vector.length ts,
                                               typeIsUseful))
                                 then Var tuple
                                 else Select {tuple = tuple,
                                              offset = offset})))
                      else Vector.Continue (n - 1,
                                            if typeIsUseful t
                                               then offset + 1
                                               else offset),
                   fn _ => Error.bug "SimplifyTypes.simplifyExp: Select:newOffset")
               end
          | Tuple xs => Keep (tuple xs)
          | _ => Keep e
      val simplifyExp =
         Trace.trace ("SimplifyTypes.simplifyExp",
                      Exp.layout, Result.layout Exp.layout)
         simplifyExp
      fun simplifyTransfer (t : Transfer.t): Statement.t vector * Transfer.t =
         case t of
            Bug => (Vector.new0 (), t)
          | Call {func, args, return} =>
               (Vector.new0 (),
                Call {func = func, return = return,
                      args = removeUselessVars args})
          | Case {test, cases = Cases.Con cases, default} =>
               let
                  val cases =
                     Vector.keepAll (cases, fn (con, _) =>
                                     not (ConRep.isUseless (conRep con)))
                  val default =
                     case (Vector.length cases, default) of
                        (_,     NONE)    => NONE
                      | (0,     SOME l)  => SOME l
                      | (n,     SOME l)  =>
                           if n = tyconNumCons (Type.deDatatype (oldVarType test))
                              then NONE
                              else SOME l
                  fun normal () =
                     (Vector.new0 (),
                      Case {test = test,
                            cases = Cases.Con cases,
                            default = default})
               in case (Vector.length cases, default) of
                  (0,         NONE)   => (Vector.new0 (), Bug)
                | (0,         SOME l) =>
                     (Vector.new0 (), Goto {dst = l, args = Vector.new0 ()})
                | (1, NONE)   =>
                     let
                        val (con, l) = Vector.first cases
                     in
                        if ConRep.isUseful (conRep con)
                           then
                              (* This case can occur because an array or vector
                               * tycon was kept around.
                               *)
                              normal ()
                        else (* The type has become a tuple.  Do the selects. *)
                           let
                              val ts = keepSimplifyTypes (conArgs con)
                              val (args, stmts) =
                                 if 1 = Vector.length ts
                                    then (Vector.new1 test, Vector.new0 ())
                                 else
                                    Vector.unzip
                                    (Vector.mapi
                                     (ts, fn (i, ty) =>
                                      let
                                         val x = Var.newNoname ()
                                      in
                                         (x,
                                          Statement.T
                                          {var = SOME x,
                                           ty = ty,
                                           exp = Select {tuple = test,
                                                         offset = i}})
                                      end))
                           in
                              (stmts, Goto {dst = l, args = args})
                           end
                     end
                | _ => normal ()
               end
          | Case _ => (Vector.new0 (), t)
          | Goto {dst, args} =>
               (Vector.new0 (), Goto {dst = dst, args = removeUselessVars args})
          | Raise xs => (Vector.new0 (), Raise (removeUselessVars xs))
          | Return xs => (Vector.new0 (), Return (removeUselessVars xs))
          | Runtime {prim, args, return} =>
               (Vector.new0 (), Runtime {prim = prim,
                                         args = args,
                                         return = return})
      val simplifyTransfer =
         Trace.trace
         ("SimplifyTypes.simplifyTransfer",
          Transfer.layout,
          Layout.tuple2 (Vector.layout Statement.layout, Transfer.layout))
         simplifyTransfer
      fun simplifyStatement (Statement.T {var, ty, exp}) =
         case simplifyMaybeVarType (var, ty) of
            NONE =>
               (* It is impossible for a statement to produce a value of an
                * uninhabited type; block must be unreachable.
                * Example: `Vector_sub` from a `(ty) vector`, where `ty` is
                * uninhabited.  The `(ty) vector` type is inhabited, but only by
                * the vector of length 0; this `Vector_sub` is unreachable due
                * to a dominating bounds check that must necessarily fail.
                *)
               Dead
          | SOME ty =>
               (* It is wrong to omit calling simplifyExp when var = NONE because
                * targs in a PrimApp may still need to be simplified.
                *)
               (case simplifyExp exp of
                   Dead => Dead
                 | Delete => Delete
                 | Keep exp => Keep (Statement.T {var = var, ty = ty, exp = exp}))
      val simplifyStatement =
         Trace.trace
         ("SimplifyTypes.simplifyStatement",
          Statement.layout,
          Result.layout Statement.layout)
         simplifyStatement
      fun simplifyBlock (Block.T {label, args, statements, transfer}) =
         case simplifyFormalsOpt args of
            NONE =>
               (* It is impossible for a block to be called with a value of an
                * uninhabited type; block must be unreachable.
                *)
               NONE
          | SOME args =>
               let
                  val statements =
                     Vector.fold'
                     (statements, 0, [], fn (_, statement, statements) =>
                      case simplifyStatement statement of
                         Dead => Vector.Done NONE
                       | Delete => Vector.Continue statements
                       | Keep s => Vector.Continue (s :: statements),
                      SOME o Vector.fromListRev)
               in
                  case statements of
                     NONE => SOME ({dead = true},
                                   Block.T {label = label,
                                            args = args,
                                            statements = Vector.new0 (),
                                            transfer = Bug})
                   | SOME statements =>
                        let
                           val (stmts, transfer) = simplifyTransfer transfer
                           val statements = Vector.concat [statements, stmts]
                        in
                           SOME ({dead = false},
                                 Block.T {label = label,
                                          args = args,
                                          statements = statements,
                                          transfer = transfer})
                        end
               end
      fun simplifyFunction f =
         let
            val {args, mayInline, name, raises, returns, start, ...} =
               Function.dest f
         in
            case simplifyFormalsOpt args of
               NONE =>
                  (* It is impossible for a function to be called with a value of an
                   * uninhabited type; function must be unreachable.
                   *)
                  NONE
             | SOME args =>
                  let
                     val blocks = ref []
                     fun loop (Tree.T (b, children)) =
                        case simplifyBlock b of
                           NONE => ()
                         | SOME ({dead}, b) =>
                              let
                                 val _ = List.push (blocks, b)
                              in
                                 if dead
                                    then ()
                                    else Tree.Seq.foreach (children, loop)
                              end
                     val _ = loop (Function.dominatorTree f)

                     val returns = Option.map (returns, keepSimplifyTypes)
                     val raises = Option.map (raises, keepSimplifyTypes)
                  in
                     SOME (Function.new {args = args,
                                         blocks = Vector.fromList (!blocks),
                                         mayInline = mayInline,
                                         name = name,
                                         raises = raises,
                                         returns = returns,
                                         start = start})
                  end
         end
      val globals =
         Vector.concat
         [Vector.new1 (Statement.T {var = SOME unitVar,
                                    ty = Type.unit,
                                    exp = Exp.unit}),
          Vector.keepAllMap (globals, fn s =>
                             case simplifyStatement s of
                                Dead => Error.bug "SimplifyTypes.globals: Dead"
                              | Delete => NONE
                              | Keep b => SOME b)]
      val shrink = shrinkFunction {globals = globals}
      val simplifyFunction = fn f => Option.map (simplifyFunction f, shrink)
      val functions = List.revKeepAllMap (functions, simplifyFunction)
      val program =
         Program.T {datatypes = datatypes,
                    globals = globals,
                    functions = functions,
                    main = main}
      val _ = destroyTypeCardinality ()
      val _ = destroySimplifyTypeOpt ()
      val _ = Program.clearTop program
   in
      program
   end

end
