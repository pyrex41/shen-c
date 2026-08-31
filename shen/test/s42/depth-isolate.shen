\\ Isolated AOT depth crash repro. Sidecar; not shaken into kernel.kl.
\\ Kerneltests depth-first-search pair after search.shen's 3-arg depth.
(load "search.shen")
(load "depth.shen")
(depth 4 (/. X [(+ X 3) (+ X 4) (+ X 5)]) (/. X (= X 27)) (/. X (> X 27)))
(depth 4 (/. X [(+ X 3)]) (/. X (= X 27)) (/. X (> X 27)))
