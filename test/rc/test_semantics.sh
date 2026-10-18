#!/bin/sh
# Compile and execute real KL through the RC emitter/runtime. One build, one run.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
builder=${1:-"$root/bin/rc-build"}
case "$builder" in /*) ;; *) builder="$PWD/$builder" ;; esac
if [ ! -x "$builder" ]; then
    echo "RC semantics: builder unavailable: $builder" >&2
    exit 1
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/shen-rc-semantics.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/input"
: > "$work/input/kernel.kl"
cat > "$work/input/user.kl" <<'KL'
(defun rc-count (N A) (if (= N 0) A (rc-count (- N 1) (+ A 1))))
(defun rc-even (N) (if (= N 0) true (rc-odd (- N 1))))
(defun rc-odd (N) (if (= N 0) false (rc-even (- N 1))))
(defun rc-return-add (X) (lambda Y (+ X Y)))
(defun rc-curry-add (X Y) (+ X Y))
(defun rc-redefined (X) (+ X 1))
(defun rc-and-tail (N) (and true (if (= N 0) true (rc-and-tail (- N 1)))))
(defun rc-or-tail (N) (or false (if (= N 0) false (rc-or-tail (- N 1)))))
(defun rc-bool-even (N) (if (= N 0) true (and true (rc-bool-odd (- N 1)))))
(defun rc-bool-odd (N) (if (= N 0) false (or false (rc-bool-even (- N 1)))))
(defun rc-swap (N X Y) (if (= N 0) (cons X (cons Y ())) (rc-swap (- N 1) Y X)))
(defun rc-snapshot (N) (if (= N 0) 1 (rc-snapshot (let Ignored (defun rc-snapshot (N) 99) 0))))
(defun rc-rebound (N) (if (= N 0) 1 (let Ignored (defun rc-rebound (N) 99) (rc-rebound 0))))
(defun rc-self-error (N) (if (= N 0) (simple-error "self-loop-error") (rc-self-error (- N 1))))
(defun rc-and-invalid (N) (and true (if (= N 0) 42 (rc-and-invalid (- N 1)))))
(defun rc-bound (X N Ignored) (if (= N 0) (lambda Last X) ((value *rc-bound*) 0 0 99)))
(defun rc-do-count (N A) (if (= N 0) A (do (set *rc-do-last* N) (rc-do-count (- N 1) (+ A 1)))))
(defun rc-do-nested (N) (if (= N 0) 42 (do 1 (do 2 (rc-do-nested (- N 1))))))
(defun rc-do-snapshot (N) (if (= N 0) 42 (do 99 (rc-do-snapshot (let Ignored (defun do (X Y) X) 0)))))
(defun rc-do-invalid (N) (and true (if (= N 0) 42 (do 0 (rc-do-invalid (- N 1))))))
(defun rc-do-error (N) (if (= N 0) (simple-error "do-loop-error") (do 0 (rc-do-error (- N 1)))))
KL
cat > "$work/cases" <<'CASES'
lexical-function|(let F (lambda X (+ X 1)) (F 4))|5
lexical-special-shadow|(let if (lambda X (+ X 2)) (if 3))|5
lexical-primitive-shadow|(let + (lambda X X) (+ 7))|7
integer-arithmetic|(+ 20 22)|42
fraction-arithmetic|(= (/ 5 2) 2.5)|true
negative-integer|(- 0 42)|-42
let-alias|(let X (cons 42 ()) (let Y X (hd Y)))|42
list-return|(hd (tl (cons 1 (cons 42 ()))) )|42
closure-capture|(let X 20 (let F (lambda Y (+ X Y)) (F 22)))|42
nested-capture|(let X 10 (let F (lambda Y (lambda Z (+ X (+ Y Z)))) ((F 20) 12)))|42
shadow-capture|(let X 20 (let F (lambda Y (+ X Y)) (let X 99 (F 22))))|42
partial-application|((rc-curry-add 20) 22)|42
over-application|(rc-return-add 20 22)|42
closure-in-vector|(let V (absvector 1) (do (address-> V 0 (lambda X (+ X 1))) ((<-address V 0) 41)))|42
vector-alias|(let V (absvector 1) (let W V (do (address-> V 0 42) (<-address W 0))))|42
vector-replacement|(let V (absvector 1) (do (address-> V 0 (cons 42 ())) (let X (<-address V 0) (do (address-> V 0 ()) (hd X)))))|42
vector-self-cycle|(let V (absvector 1) (do (address-> V 0 V) (= V (<-address V 0))))|true
vector-two-node-cycle|(let V (absvector 1) (let W (absvector 1) (do (address-> V 0 W) (do (address-> W 0 V) (= V (<-address (<-address V 0) 0))))))|true
closure-vector-cycle|(let V (absvector 1) (let F (lambda X (<-address V X)) (do (address-> V 0 F) (= F (F 0)))))|true
global-root|(do (set *rc-root* (cons 42 ())) (hd (value *rc-root*)))|42
global-replacement-alias|(let X (value *rc-root*) (do (set *rc-root* ()) (hd X)))|42
global-self-assignment|(do (set *rc-root* (cons 42 ())) (do (set *rc-root* (value *rc-root*)) (hd (value *rc-root*))))|42
global-cycle-release|(let V (absvector 1) (do (address-> V 0 V) (do (set *rc-root* V) (do (set *rc-root* ()) 42))))|42
tail-self|(rc-count 100000 0)|100000
tail-mutual|(rc-even 100001)|false
tail-simultaneous-swap|(= (rc-swap 100001 1 2) (cons 2 (cons 1 ())))|true
tail-callee-snapshot|(rc-snapshot 1)|1
tail-snapshot-redefinition-visible|(rc-snapshot 0)|99
tail-rebound-callee|(rc-rebound 1)|99
tail-loop-error|(trap-error (rc-self-error 10000) (lambda E (= (error-to-string E) "self-loop-error")))|true
tail-loop-capture-reload|(let V (absvector 1) (let X 42 (let F (lambda N (if (= N 0) X ((<-address V 0) (- N 1)))) (do (address-> V 0 F) (F 10000)))))|42
tail-bound-overapplication|(let P (rc-bound 42) (do (set *rc-bound* P) (P 1 0)))|42
tail-boolean-invalid|(trap-error (rc-and-invalid 10000) (lambda E true))|true
tail-and-self|(rc-and-tail 100000)|true
tail-or-self|(rc-or-tail 100000)|false
tail-boolean-mutual-even|(rc-bool-even 100000)|true
tail-boolean-mutual-odd|(rc-bool-even 100001)|false
and-final-value-type|(trap-error (and true 42) (lambda E true))|true
or-final-value-type|(trap-error (or false 42) (lambda E true))|true
and-final-call-type|(trap-error (and true (rc-curry-add 20 22)) (lambda E true))|true
or-final-call-type|(trap-error (or false (rc-curry-add 20 22)) (lambda E true))|true
trap-simple|(trap-error (simple-error "boom") (lambda E (= (error-to-string E) "boom")))|true
trap-captured-handler|(let S "saved" (trap-error (simple-error "boom") (lambda E (= S "saved"))))|true
trap-nested|(trap-error (trap-error (simple-error "inner") (lambda E (simple-error "outer"))) (lambda E (= (error-to-string E) "outer")))|true
trap-head-empty|(trap-error (hd ()) (lambda E 42))|42
trap-vector-bounds|(trap-error (<-address (absvector 1) 1) (lambda E 42))|42
trap-non-boolean|(trap-error (if 1 2 3) (lambda E 42))|42
error-cleanup-vector|(let V (absvector 1) (do (address-> V 0 (cons 42 ())) (trap-error (do (hd ()) 99) (lambda E (hd (<-address V 0))))))|42
branch-lazy-if|(if true 42 (simple-error "eager-if"))|42
branch-lazy-and|(and false (simple-error "eager-and"))|false
branch-lazy-or|(or true (simple-error "eager-or"))|true
cond-branch|(cond (false 1) (true 42))|42
string-concat|(= (cn "foo" "bar") "foobar")|true
string-tail|(= (tlstr "hello") "ello")|true
character-roundtrip|(= (string->n (n->string 65)) 65)|true
dynamic-eval|(eval-kl (cons + (cons 20 (cons 22 ()))))|42
dynamic-eval-trap|(trap-error (eval-kl (cons simple-error (cons "dynamic-boom" ()))) (lambda E (= (error-to-string E) "dynamic-boom")))|true
function-before-redefinition|(rc-redefined 41)|42
function-redefinition|(do (defun rc-redefined (X) (+ X 2)) (rc-redefined 40))|42
function-after-redefinition|(rc-redefined 40)|42
top-first-definition|(defun rc-source-order () 1)|@definition
top-first-call|(rc-source-order)|1
top-second-definition|(defun rc-source-order () 2)|@definition
top-second-call|(rc-source-order)|2
do-partial-application|((do 1) 2)|2
do-tail-builtin|(rc-do-count 100000 0)|100000
do-tail-left-effect|(value *rc-do-last*)|1
do-tail-nested|(rc-do-nested 100000)|42
do-tail-boolean-error|(trap-error (rc-do-invalid 100) (lambda E true))|true
do-tail-recursive-error|(trap-error (rc-do-error 10000) (lambda E (= (error-to-string E) "do-loop-error")))|true
do-tail-snapshot|(rc-do-snapshot 1)|42
do-tail-redefined-fallback|(rc-do-count 3 0)|3
do-tail-native-identity-restore|(defun do (X Y) Y)|@definition
do-tail-native-identity|(rc-do-count 100000 0)|100000
do-tail-native-nested|(rc-do-nested 100000)|42
do-first-definition|(defun do (X Y) X)|@definition
do-redefined-first|(do 10 20)|10
do-standard-definition|(defun do (X Y) Y)|@definition
do-restored-second|(do 10 20)|20
do-callee-before-arguments|(do (defun do (X Y) X) (do 1 2))|1
do-arity-one-redefinition|(let Ignored (defun do (X) X) (do 7))|7
do-final-restore|(defun do (X Y) Y)|@definition
callee-initial-definition|(defun rc-early-callee (X) 1)|@definition
callee-before-arguments|(rc-early-callee (let Ignored (defun rc-early-callee (X) 2) 0))|1
callee-after-redefinition|(rc-early-callee 0)|2
indirect-callee-before-arguments|(let F rc-early-callee (F (let Ignored (defun rc-early-callee (X) 3) 0)))|2
indirect-callee-after-redefinition|(rc-early-callee 0)|3
module-snapshot-before-args|(let R (rc-early-callee (let Ignored (defun rc-early-callee (X) 4) 0)) R)|3
module-snapshot-after-redefinition|(let R (rc-early-callee 0) R)|4
module-partial-fallback|(let F (rc-curry-add 20) (let R (F 22) R))|42
module-overapply-fallback|(let R (rc-return-add 20 22) R)|42
module-returned-tail-guard|(trap-error (let R (rc-and-invalid 100) R) (lambda E true))|true
module-returned-tail-result|(let R (rc-even 10000) R)|true
indirect-undefined-before-arguments|(do (set *rc-argument-side-effect* 0) (do (trap-error (let F rc-missing (F (set *rc-argument-side-effect* 1))) (lambda E ())) (value *rc-argument-side-effect*)))|0
CASES
while IFS='|' read -r name expression expected; do
    printf '%s\n' "$expression" >> "$work/input/user.kl"
    if [ "$expected" != @definition ]; then
        printf '%s\n' "$expected" >> "$work/expected"
        printf '%s\n' "$name" >> "$work/names"
    fi
done < "$work/cases"
SHEN_C_HOME="$root" "$builder" "$work/input" "$work/app"
SHEN_RC_PRINT=1 "$work/app/app" > "$work/actual"
if ! diff -u "$work/expected" "$work/actual"; then
    echo "RC semantic case order:" >&2
    nl -ba "$work/names" >&2
    exit 1
fi
count=$(wc -l < "$work/expected" | tr -d ' ')
printf 'RC emitted semantics: %s/%s cases PASS\n' "$count" "$count"
# A tail operand under do must retain the snapshotted-do identity guard.
grep -q '&& rc_transparent_do(t\[' "$work/app/app.c"
grep -q 'rc_compiled_call_matches(fn,rc_native_' "$work/app/app.c"
# Audit the actual generated executable, independently from the host compiler.
"$root/scripts/test-rc.sh" --audit-only "$work/app/app"
