\\ Incremental AOT seed for the 579-defun runme runner.
\\ Dynamic (load …) does not shake kernel symbols. seed.kl calls
\\ maxinferences (already in the 579 kernel) and store-arity for sidecar
\\ NativeFunctions. qmachine.kl / depth.kl / sum.kl are small shakes, not
\\ interpreter.shen + all 40 tests. Do not floor *maxinferences* to 1e10:
\\ that turns a typed-load failure into an unbounded t* run.
\\ emit.c must not identity-fold shen.demodulate: synonyms-h eval-redefines
\\ shen.demod (c-minus preamble = (list statement)).
(maxinferences 10000000)
