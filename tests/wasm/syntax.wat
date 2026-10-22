;; Konstrukce textu WAT, které vestavěný assembler (luxir -a -t wasm32)
;; podporuje: tests/wat_diff.py je porovná s wat2wasm bajt po bajtu.
(module $syntax
  (; blokový komentář (; vnořený ;) ;)
  (import "env" "memory" (memory $mem 1 65536 shared))
  (import "env" "tab" (table $ext 2 10 funcref))
  (import "env" "g" (global $gi i32))
  (import "env" "gm" (global $gm (mut i64)))
  (import "env" "f" (func $imp (param i32 i64) (result i32)))
  (import "env" "noargs" (func $noargs))
  (export "memory" (memory 0))
  ;; implicitní typy: signatury bez shodného explicitního typu se přidají za
  ;; explicitní typy, které jsou v textu až níže
  (func $first (export "first") (export "first2") (param $a i64) (param i32) (result i64)
    (local $x i64) (local i32 i32) (local $y f64) (local i64)
    local.get $a
    local.set $x
    (local.set 3 (i32.add (local.get 1) (i32.const 0x7fffffff)))
    (if (result i64) (i32.eqz (local.get 3))
      (then (i64.const -1))
      (else (local.get $x)))
    drop
    block $out (result i64)
      loop $again
        local.get 1
        br_if $again
        i64.const 9
        br $out
      end
      unreachable
    end
    (block $b
      (br_table $b 0 $b (local.get 2)))
    i32.const -2147483648
    i32.const 4294967295
    i32.add
    if $named
      nop
    else $named
    end $named
    local.get 1
    if
      i64.const 1
      return
    else
    end
    i64.const 0xffffffffffffffff
    i64.const -9223372036854775808
    i64.add
    (i64.store offset=8 align=4 (i32.const 0) (i64.const 1_000_000))
    (drop (i32.load8_u offset=65536 (i32.const 3)))
    (drop (i64.atomic.rmw.cmpxchg offset=16 (i32.const 0) (i64.const 1) (i64.const 2)))
    (drop (memory.atomic.wait32 (i32.const 0) (i32.const 0) (i64.const -1)))
    (atomic.fence)
    (memory.copy (i32.const 0) (i32.const 8) (i32.const 8))
    (memory.fill (i32.const 0) (i32.const 0) (i32.const 8))
    (memory.init $passive (i32.const 0) (i32.const 0) (i32.const 1))
    (data.drop 1)
    (drop (memory.grow (memory.size)))
    (drop (call_indirect $tab (type $t1) (i64.const 5) (i32.const 1)))
    (drop (call $imp (i32.const 1) (i64.const 2)))
    call $noargs
    (global.set $gm (global.get $gm))
    (select (i64.const 1) (i64.const 2) (global.get $gi))
    drop
    (i64.extend_i32_u (i32.trunc_sat_f64_s (f64.reinterpret_i64 (local.get $x))))
  )
  (type $t0 (func))
  (type $t1 (func (param i64) (result i64)))
  (type (func (param i32 i32) (result i32)))
  (func $second (type $t1)
    local.get 0)
  (func (type $t0))
  (func $third (param i32 i32) (result i32)
    (i32.add (local.get 0) (local.get 1)))
  (table $tab 4 funcref)
  (global $counter (mut i32) (i32.const 0))
  (global $const i64 (i64.const 42))
  (global (export "exported") i32 (global.get $gi))
  (export "second" (func $second))
  (export "table" (table $tab))
  (export "counter" (global $counter))
  (start 4)
  (elem (i32.const 1) func $second $third)
  (elem (offset (i32.const 3)) $first)
  (data (i32.const 16) "text\n\t\"\\\00\ff" "\u{7f}\u{e9}\u{20ac}\u{1f600}")
  (data $passive "passive")
  (data $empty (i32.const 0))
)
