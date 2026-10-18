\\ Compile once through the pinned Shen frontend. Driver setup and input
\\ allocation are outside the measured phase; these are actual @s/@v forms.
(define fusion-count-string
  "" N -> N
  (@s _ S) N -> (fusion-count-string S (+ N 1)))

(define fusion-rebuild-string
  "" -> ""
  (@s C S) -> (@s C (fusion-rebuild-string S)))

(define fusion-build-string
  0 -> ""
  N -> (@s "a" (fusion-build-string (- N 1))))

(define fusion-count-vector
  V N -> N where (= (limit V) 0)
  (@v _ V) N -> (fusion-count-vector V (+ N 1)))

(define fusion-build-vector
  0 -> (vector 0)
  N -> (@v 7 (fusion-build-vector (- N 1))))
