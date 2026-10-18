"Copyright (c) 2010-2021, Mark Tarver

   3 clause BSD; see license"

(DEFUN char-stream? (Stream)
  (IF (SUBTYPEP (STREAM-ELEMENT-TYPE Stream) 'CHARACTER)
      'true
      'false))
