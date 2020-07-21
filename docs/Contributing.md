# Contributing to LLPC

## Introduction

This file documents best practices for use when making changes to the LLPC
project.

Where the [Coding Standards](./CodingStandards.md) are concerned with the
quality of the source code of LLPC at any point in time, this document is
concerned with the quality of the process by which the source code evolves
over time.

LLPC uses pull requests on GitHub for code review. We maintain a linear
history, that is, pull requests are rebased on top of the `dev` branch.


## Commits as the logical atomic unit of change

Adapted from [the Linux kernel guide on submitting patches](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/process/submitting-patches.rst),
which really says it best:

> Separate each **logical change** into a separate commit.
>
> For example, if your changes include both a bug fix and a performance
> enhancement, separate those changes into two commits. If your change includes
> a cleanup that extracts common functionality into a new function and a
> bug fix in that common functionality, separate those into two commits.
>
> On the other hand, if you make a single change to numerous files, such as
> adopting a new API provided by LLVM, group those changes into a single
> commit.  Thus a single logical change is contained within a single commit.
>
> The point to remember is that each commit should make an easily understood
> change that can be verified by reviewers. Each commit should be justifiable
> on its own merits.

Each commit must be a strict improvement. When submitting a series of commits,
make a reasonable effort to ensure that there are no regressions in the middle
of the series. This allows us to use `git bisect` when regressions do occur.

Exercise judgment as to whether a series of commits should be submitted as
multiple pull requests or as a single one. GitHub makes it difficult to review
individual commits in a pull request, but having many pull requests is
confusing for reviewers as well, because their relationship, if any, is not
clear in the UI. Some rules of thumb:

* If your changes are entirely separate, such as a new feature and an unrelated
  bug fix that you happened to make while developing the feature, put the
  commits onto separate branches and submit them as separate pull requests.
* If your changes are related, such as a sequence of refactorings and
  enhancements towards implementing a larger feature, consider submitting them
  as a single pull requests. Reviewers can still look at individual commits
  via GitHub's UI or by downloading your branch and examining it locally.

Having multiple incomplete commits locally and rewriting them using
`git rebase` is normal and encouraged as part of the process of shaping
a series of logically self-contained commits. The guidelines above only apply
once you send out your changes via a pull request.


## Write useful commit messages

[Parts of this section are also adapted from
[the Linux kernel guide on submitting patches](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/process/submitting-patches.rst).]

Commit messages should describe the "why" of a change: What does the change
solve? Where does it lead? Convince the reviewer that there is a problem worth
fixing.

Follow best practices for commit messages:

* The first line must be a short summary of the commit. The second line must be
  blank.
* When the commit is contained entirely within a logical module, prefix
  the summary with the module name, e.g. "builder: Add CreateUnicornSample".
* Generally keep lines shorter than 75 characters. Exceeding this limit
  slightly for the summary is okay, and don't break lines where it would
  hurt searchability, e.g. don't break URLs and CTS test names. Don't break
  footer lines, as tools should be able to parse them mechanically.
* Use common practice footer lines to provide more information where
  applicable.
* The most important footer line is a "Fixes:" line. They allow a quick
  identification of relevant bug fix commits, for example for cherry-picking
  onto release branches. Common forms are:
  * Indicate the fix of a regression that was introduced by an earlier commit,
    using both the hash and the summary title of the commit in the following
    format:

        Fixes: 123456789a ("patch: Change the default order of userdata SGPRs")

  * Refer to an issue that was fixed:

        Fixes: https://github.com/GPUOpen-Drivers/llpc/issues/1234

    **Caution:** We still need to clarify how/whether to refer to internal
    tickets.

  * Refer to a CTS test that was fixed:

        Fixes: dEQP-VK.shader.unicorns.horn_winding.ccw.*

* Provide further explanation as necessary in paragraphs between the summary
  and the footer.
* Describe your changes in imperative mood, as if you are giving orders to the
  codebase to change its behaviour, e.g. "make xyzzy do frotz" instead of
  "[This patch] makes xyzzy do frotz" or "[I] changed xyzzy to do frotz".

Quote the relevant specifications (Vulkan, SPIR-V, or other) to provide
justification for your changes. For example:

    Section "Validation Rules within a Module" of the Vulkan Environment
    for SPIR-V appendix (version 1.2.345) says:

        "If OpControlBarrier is used in fragment, vertex, tessellation
        evaluation, or geometry shaders, the execution Scope must be
        Subgroup."

    Remove the unused code for the Workgroup case and guard with an
    assertion instead.

This is particularly important when your change affects a subtle
or uncommonly used corner of the specification.

For optimizations, provide numbers you have obtained in testing.

Remember that some of these additional explanations fit better as comments
in the code itself. Exercise judgment when deciding whether to put the
explanation in a comment or in the commit message. Ask yourself: is this
useful as a comment when reading the code? Or does it only distract?
Is the explanation independent of the current context and is it likely
to stand the test of time?


## Useful things to know about Git

Not all projects that use Git aim for commits as the logical unit of change,
so it is expected that you may not be aware of all useful functionality in
Git. This document is not a Git tutorial, but here is a brief list of tools
that are useful to know of for this workflow:

* `git gui` to interactively commit only a subset of local changes, and to
  amend the most recent commit.
* `git add -p` to achieve the same via the command line.
* `git commit --amend` to amend the most recent commit from the command line.
* `git stash` to temporarily undo an uncommitted change and re-apply it later.
* `git commit --squash=<commit>` and `git commit --fixup=<commit>` to mark
  changes to be added to an earlier commit.
* `git rebase -i <commit>` and the `rebase.autoSquash` option to rework a
  series of commits.
* `git rebase -i <commit> -x "..."` to execute a command after each commit;
  you can use this to verify that LLPC compiles and passes lit tests after
  each commit.
* `git cherry-pick` for moving a commit from one branch to another.
* `gitk` and `tig` to browse the repository's history.
* `git reflog` to find old states of the repository that may seem lost; once
  you commit something, Git will **not** lose it -- at least not for the next
  30 days.

Read Git's documentation or search online to learn more about them. Similar
functionality is available in some GUI tools.


## Additional sources

* [Patch submission guidelines of the Linux kernel](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/process/submitting-patches.rst)
* [Blog post: They want to be small, they want to be big: thoughts on code reviews and the power of patch series](http://nhaehnle.blogspot.com/2020/06/they-want-to-be-small-they-want-to-be.html)
