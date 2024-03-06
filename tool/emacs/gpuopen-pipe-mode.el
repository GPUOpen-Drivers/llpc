;;; gpuopen-pipe-mode --- A mode to inspect pipeline/amdllpc outputs -*- lexical-binding: t; coding: utf-8 -*-
;;
;; Copyright 2023 Jan Rehders, Advanced Micro Devices, Inc.
;;
;; Author: Jan Rehders <jan.rehders@amd.com>
;; Version: 0.1
;; Package-Requires: ((emacs "26.1") (transient "0.1") (highlight-symbol "1.0"))
;; URL: https://github.com/GPUOpen-Drivers/llpc/
;;
;;; Commentary:
;;
;; A simple mode for .pipe files.  It provides some easy navigation to
;; sections, and allows to add highlighting and quick doc lookup
;;
;;; Code:

(defgroup gpuopen nil
  "Options for gpuopen tools."
  :group 'tools)

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;; Parse/visualize live ranges in .pipe files

(defun gpuopen-parse-liverange (string)
  "Parse the given `STRING' as a live range printed by LLVM."
  (when (string-match "\\[\\([0-9]+\\)[Berg],\\([0-9]+\\)[Berg]:[0-9]+)" string)
    (cons
     (string-to-number (match-string 1 string))
     (string-to-number (match-string 2 string)))))

(defun gpuopen-liverange-at-point-extent ()
  "Return (start . end) for the live range at point."
  (unless (looking-at "\\[")
    (ignore-errors
      (backward-up-list 1)))
  (when (looking-at "\\[")
    (let ((startpos (point))
          (endpos (save-excursion
                    (forward-sexp 1)
                    (point))))
      (cons startpos endpos))))

(defun gpuopen-liverange-at-point ()
  "Return the live range after or around point."
  (when-let ((extent (gpuopen-liverange-at-point-extent)))
    (gpuopen-parse-liverange
     (buffer-substring (car extent) (cdr extent)))))

;;;###autoload
(defun gpuopen-mark-liveranges-at-point ()
  "Will mark all live ranges of the current line.

Run

amdllpc loop.frag \
 --debug-only=stack-slot-coloring \
 --print-after=stack-slot-coloring

Add --vgpr-limit=4 if you don't see any spills.

Then look for

  ********** Stack Slot Coloring **********
  ********** Function: _amdgpu_ps_main
  Spill slot intervals:
  SS#0 [544r,864B:0)[944B,2360B:0)[2408B,3112B:0) 0@x  weight:2.390476e+01

Move the cursor over the first [ and call this command.  The ranges in the
following machine code will get highlighted.  Keep in mind that the stack slots
might get renamed so stack.3 might not be the fourth stack slot."
  (interactive)
  (when (looking-at "\\[")
    (require 'highlight-symbol)
    (let* ((color (highlight-symbol-next-color))
           (face `((background-color . ,color)
                   (foreground-color . ,highlight-symbol-foreground-color))))
      (while (looking-at "\\[")
        (save-excursion
          (when-let* ((liverange-extent (gpuopen-liverange-at-point-extent))
                      (liverange (gpuopen-liverange-at-point)))
            (let ((liverange-start (car liverange))
                  (liverange-end (cdr liverange))
                  liverange-startpos
                  liverange-endpos)
              (search-forward-regexp (format "^%sB" liverange-start))
              (setq liverange-startpos (point))
              (search-forward-regexp (format "^%sB" liverange-end))
              (backward-word 1)
              (setq liverange-endpos (point))
              (let ((x (make-overlay liverange-startpos liverange-endpos)))
                (overlay-put x 'face face))
              (let ((x (make-overlay (+ (car liverange-extent) 1)
                                     (- (cdr liverange-extent) 1))))
                (overlay-put x 'face face)))))
        (forward-sexp 1)))))

;;;###autoload
(defun gpuopen-unmark-liveranges ()
  "Remove high-lights of live ranges."
  (interactive)
  (remove-overlays (point-min) (point-max)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;; Mode to view .pipe files

(defvar gpuopen-pipe-keywords
  (list
   "SCRATCH_STORE_[A-Za-z0-9]+"
   "SCRATCH_RESTORE_[A-Za-z0-9]+")
  "Keywords to be highlighted in `gpuopen-pipe-mode'.

This is extremely incomplete and needs updating, patches needed.")

(defvar gpuopen-pipe-font-lock-list
  `(;; Headers/sections like '**** RegAlloc ****'
    (,(rx (seq line-start (>= 3 "*") " " (group (+ not-newline)) " " (>= 3 "*"))) 1 font-lock-doc-face)
    ;; LLVM functions when pass is being run on multiple functions
    (,(rx (seq "********** Function: " (group (+ not-newline)))) 1 font-lock-function-name-face))
  "A list of fontification rules for `gpuopen-pipe-mode'.")

(defvar gpuopen-pipe-mode-hook nil
  "Hook run when enabling `gpuopen-pipe-mode'.")

(defvar gpuopen-pipe-mode-map (make-sparse-keymap)
  "A keymap for `gpuopen-pipe-mode'.")

(defun gpuopen-pipe-mode-setup ()
  "Initialize `gpuopen-pipe-mode'."

  (let ((map gpuopen-pipe-mode-map))
    (use-local-map map)
    (define-key map (kbd "C-c C-h l") #'gpuopen-mark-liveranges-at-point)
    (define-key map (kbd "C-c C-h u") #'gpuopen-unmark-liveranges))

  (let ((llvm-ir-dump-re '("Passes" "^\\(?:# \\)?[*][*][*] IR Dump \\([^*]+\\) [*][*][*]" 1))
        (llvm-debug-type-re '("DEBUG_TYPE" "\\(?:\\*\\*\\*\\*\\*\\*\\*\\*\\*\\* +\\([^*\n]+\\)\\*\\*\\*\\*\\*\\*\\*\\*\\*\\*\\)" 1)))
    (setq imenu-generic-expression `(,llvm-ir-dump-re ,llvm-debug-type-re)))
  (run-hooks 'gpuopen-pipe-mode-hook))

(define-generic-mode gpuopen-pipe-mode
  nil
  gpuopen-pipe-keywords
  gpuopen-pipe-font-lock-list
  '("\\.pipe")
  (list #'gpuopen-pipe-mode-setup)
  "A mode for .pipe files produced by amdllpc or its output.")

(provide 'gpuopen-pipe-mode)
;;; gpuopen-pipe-mode.el ends here
