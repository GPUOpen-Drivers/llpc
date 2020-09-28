# LLPC Review Process of Pull Requests

## Introduction

This file documents the review process of LLPC pull requests, proposing
several suggestions to speed up the review process and make it work more
efficiently. Given LLPC is heavily based on LLVM, review interactions here
should follow [LLVM Community Code of Conduct](https://llvm.org/docs/CodeOfConduct.html),
which prohibits unacceptable language and behavior.  

## For authors

### Create a pull request

This useful link tells you [how to create a pull request](https://docs.github.com/en/github/collaborating-with-issues-and-pull-requests/creating-a-pull-request)
if you are a first-time user of GitHub.

### Prepare for review

Before you post a pull request for review when you finish your work, make
sure the following items are done:

* The commits are formatted without coding standard issues. Although CI
  formatting check is able to catch most issues, some still have to be
  checked manually. Please see [Coding Standards](./CodingStandards.md) to
  get familiar with LLPC coding standards and strictly follow them.

* The commit descriptions are well written to describe the changes you
  have made. Please see [Contributing to LLPC](./Contributing.md) for more
  info. Reviewers would better understand your changes when those
  descriptions are informative.

* If your pull request is a large one to implement complex features or
  code refactoring, a separate pull request adding the design document
  is expected to be reviewed in advance. With this design document,
  reviewers could know your roadmap and detailed design plans, improving
  review efficiency. Don't forget to add design document links to commit
  descriptions.

* If your pull request is relevant to opened issues, please add issue
  links to commit descriptions so reviewers know the background. Meanwhile,
  this will facilitate administrators to track solutions and close issues
  timely when they are resolved.

### Assign reviewers

After your preparation is done, post the pull request on GitHub. It is
very likely others are more familiar with your change especially when
they did relevant work in this functional area or have discussed your
design plan with you when reviewing the design document. In such cases,
you can suggest `Reviewers` in the right-top corner of the webpage of this
pull request and we encourage you to do this.

If you haven't got permission to assign reviewers then one of the active
project participants (members, approvers or reviewers) will assign one for
you. If you do have a suggestion for a reviewer then they can be requested
in the comment field. If a reviewer is not assigned for 3 working days then
you should ping the pull request.

### Address review comments

When your pull request is reviewed, you often receive review issues opened
by reviewers. Please address them by updating your commits or continue to
discuss them with reviewers if the suggestions are inappropriate. Once a
review issue is either resolved or dropped, please click `Resolve conversation`
to close it. If you forget to do so, it will cause misleading messages when
reviewers recheck your updated commits.

Don't make new commits to address review issues. Just update original
commits and force-push them, which makes code history clear. And when you
update the pull request with a new revision, it is helpful to also add a
comment to the pull request mentioning what you did, for example (assuming
the 4th revision to the pull request):
```
V4: Pulled out common code into ConvertFooToBar helper method
```
Reviewers therefore could easily track those revisions.


### Request review and approval

Typically, there are several rounds of reviews for a pull request, please
be patient. Reviewers need some time to go through your change and
understand the design logic while they also have other tasks to handle. It
is reasonable a pull request will be reviewed by several reviewers for a
few days. If no one responds to the pull request for 5+ working days,
please ping reviewers.

If your pull request is updated according to review comments and there is no
further discussion for 5+ working days, you can also ping approvers.

Please be patient when your pull request is approved while it is not merged.
Administrators are monitoring the status of pull requests all the time.
Before a pull request can be merged, administrators need do internal CI
testing. Also, we set waiting time to let a pull request fully reviewed by
everyone even though it is marked as `LGTM`. This will be discussed later.

## For reviewers

### Review a pull request

Anyone is free to review a pull request and make comments if they are
interested in changes of this pull request. If you are reviewing a pull
request and do have some comments for the author, we encourage you to add
yourself to the reviewer list by clicking `Reviewers` in the right-top
corner of the webpage of this pull request.

During your reviewing a pull request, go to the tab `Commits` to view all
linked commits of a pull request. Go to the tab `Files changed` to view
all changes in files (this merges all commits together). If you just want
to add some simple comments immediately, you can click `Add single comment`
after writing the comments. If you would like to do a formal full review,
click `Start a review`. This will cache all your review comments and publish
them until you click `Review changes` and `Submit review` in the right-top
corner of the webpage of this pull request.

If you are assigned as a reviewer, the author believes you might be the
right person to check this pull request. Please respond to it timely. If
you are busy doing something else and need more time, please notify the
author as well. We expect a pull request could be reviewed within 5 working
days assuming it is not complex.

### Request changes

Reviewers might be unhappy with certain changes. This case is caused by:

* The author makes serious logic mistakes in the commits.

* The author makes a large pull request while it is not well documented or
  there is even no design document. Reviewers are unable to understand the
  design logic.

* The author just handles some cases while there are others that are not
  handled and might become latent problems in the future.

Reviewers can therefore request changes and block this pull request. Click
`Start a review`, `Request changes`, and `Submit review`. After that, this
pull request is blocked in red status. The pull request is unable to be
merged until the reviewer who blocked this pull request reapproves it.

It is expected the reviewer who blocked this pull request should recheck it
after the author makes updates. But as you know, reviewers are busy and
could forget to do this, which leaves this pull request in dead status, so
please ping reviewers once the pull request is updated as requested but you
receive no response in the following days.

### Approve a pull request

Only approvers have the right to approve a pull request although anyone
could do code review and make comments. If the pull request is small, an
approver can check `Approve` radio button and click `Submit review`. If
the pull request is somewhat large and approvers doesn't have confidence
that everyone will accept this pull request, they could just leave
the comment `LGTM, but please wait for somebody more knowledgeable to chime
in`. This sends the message that others who are more knowledgeable can make
further decision. Meanwhile, the approver can assign the review to
someone who is more knowledgeable by adding him or her to the reviewer list.

### Final approval and merging

Different reviewers have different opinions on a pull request. It is
reasonable to set waiting time to make a `LGTM` pull request pending for
a few days to let everyone fully review it. This is because some reviewers
might be busy or on vacation so they are unable to respond to a pull
request quickly enough. Thus, a `LGTM` pull request doesn't mean everyone
is happy with it. Let's discuss those cases:

* For a tiny pull request resolving build failure, fixing coding typos,
  addressing missing review comments of a merged pull request, updating
  codes for upcoming LLVM promotion, it could be approved immediately
  and be merged afterwards.

* For a small pull request that resolves a small problem and does minor
  code refactoring, please make it pending for 1 working day before final
  approval if it is marked by assigned reviewers as `LGTM`.

* For a middle pull request that resolves a not-small problem or implements
  code refactoring involving several source files, please make it pending
  for 2 working days before final approval if it is marked by assigned
  reviewers as `LGTM`.

* For a large pull request that implements a very complex feature or makes
  large re-architecturing, please make it pending for 3 working days
  before final approval if it is marked by assigned reviewers as `LGTM`.

During the waiting time, if anyone objects to a `LGTM` pull request, they
can make comments to express their opinions and concerns. Further,
anyone disagreeing with previous reviewers can `Request changes` and block
final approval of this pull request and propose discussion.

After the waiting time ends, approvers will make final approval and let
administrators start merging process.
