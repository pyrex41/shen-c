(define redirect
 -> (let Screen      (value *stoutput*)
         Sink        (open "test.txt" out)
         Redirect    (set *stoutput* Sink)
         Write       (output "hello")
         Restore     (set *stoutput* Screen)
         Close       (close Sink)
         ok))