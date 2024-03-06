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
;; A simple function to quickly compile the current buffer using amdllpc and
;; show the output using `gpuopen-pipe-mode' plus some utilities
;;
;; Add this to your config to enable it:
;;
;;   (with-eval-after-load 'glsl-mode
;;     (define-key glsl-mode-map (kbd "C-c C-e") #'gpuopen-run-amdllpc)
;;     (define-key glsl-mode-map (kbd "C-c C-i") #'gpuopen-insert-amdllpc-generated-code))
;;
;;; Code:

(require 'compile)
(require 'gpuopen-pipe-mode)

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;; Quickly compile shader using amdllpc

(defvar gpuopen-amdllpc-invocation-format "amdllpc -v %s"
  "String to be used to create amdllpc invocations.

Will be passed to `format'.  It should have one argument %s which will be the
file to be compiled.")

;;;###autoload
(defun gpuopen-run-amdllpc ()
  "Will compile the current shader in LLPC."
  (interactive)
  (let ((compile-command (format gpuopen-amdllpc-invocation-format (buffer-file-name))))
    (call-interactively 'project-compile))
  (with-current-buffer next-error-last-buffer
    (gpuopen-pipe-mode)
    (compilation-minor-mode 1)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;; Extract and insert generated code

(defun gpuopen-get-amdllpc-generated-code ()
  "Extract generated code from the compilation buffer."
  (beginning-of-line 1)
  (with-current-buffer next-error-last-buffer
    (goto-char (point-max))
    (search-backward "_amdgpu_cs_main:")
    (let (the-start)
      (backward-sentence)
      (setq the-start (point))
      (forward-sentence)
      (buffer-substring the-start (point)))))

;;;###autoload
(defun gpuopen-insert-amdllpc-generated-code ()
  "Compile current file and try to insert generated code."
  (interactive)
  ;; (gpuopen-run-amdllpc)
  (let (the-start the-indent)
    (end-of-line 1)
    (newline-and-indent 1)
    (setq the-indent (current-column))
    (delete-horizontal-space t)
    (setq the-start (point))
    (insert
     (with-temp-buffer
       (require 'glsl-mode)
       (glsl-mode)
       (insert (gpuopen-get-amdllpc-generated-code))
       (untabify (point-min) (point-max))
       (comment-region (point-min) (point-max))
       (indent-rigidly (point-min) (point-max) the-indent)
       (buffer-string)))))

(provide 'gpuopen-compile-buffer)
;;; gpuopen-compile-buffer.el ends here
