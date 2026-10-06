# Clipboard copy preference â€” isolated source experiment

This experiment is frozen against commit
`4531db2a91e5d64455f8746c83e9377a1f948b5b`. `baseline/` contains the exact
two Git blobs; `candidate/` contains the proposed source-only changes.
`copy-preference-five-experiment.patch` uses the real repository paths and
`manifest.json` binds the base, candidate and patch SHA256 hashes. No live
source, index, build, native process or clipboard was changed by this export.
The candidate has not been compiled or tested.

The sole helper semantic change is COPY preference 1 to COPY|LINK preference 5
in `ShellOperations::copyToClipboard`. Cut remains MOVE 2. IDataObject binding,
publication, exact STA producer ownership, conditional clear, shutdown flush,
and completed-transfer acknowledgments are unchanged. This unconditional 5 is
an experiment for the actual owned filesystem fixture, not a proposed general
eligibility rule for arbitrary Shell providers.

The candidate fixture requires exact DWORD 5 in both the producer and actual
OLE consumer for every helper Copy. The independent original-native-Copy
control retains its own captured preference, including exact 5 when observed.
Complete CIDA, HDROP, FileIDs, source bytes and all existing output assertions
remain. Cut still requires exact 2, and direct drop effects remain unchanged.

Before the single Paste Shortcut operation, the actual registered canonical
leaf must be enabled, have an exact native ID and the original CommandStoreMenu
route. The independently read actual native-view background leaf must have
exactly one enabled match. The original 5000 ms readiness loop is unchanged;
there is no selection retry, clipboard republishing or enabled-state override.
The real invocation recomputes the existing strict native plan. Its completion,
native destination membership, IShellLink target, source identity/content and
owned clipboard clear must all pass before `actualShortcutOutputVerified=1`
is logged.

The standalone folder-background oracle's original strict predicate and error
are retained, but evaluated after that actual output verification. The CI base
already reported no standalone folder match even with the independent original
native Copy preference 5. This ordering exposes real causal output while
retaining the oracle failure. A log containing the verified-output marker can
still be a failing test; it is not a whole-fixture PASS. The original PASS line
remains after the oracle assertion. The normal root fixture is untouched.

The existing CI observations support an experiment, not a cause claim:

- Original native Copy published exact preference 5 with the registered and
  actual view-background Paste Shortcut leaves enabled.
- The helper published exact preference 1 with those same leaves disabled.
- Both had the same measured link formats and exact owned source identities.
- The standalone folder-background match was absent in both observations.
- The native view's pre-Copy IDataObject had no existing preferred-effect
  value; the current helper creates preference 1 rather than demonstrably
  overwriting a preexisting preference 5.

Microsoft documents preferred effect as a preferred operation which the target
need not honor, and COPY/MOVE/LINK as DWORD flags. Those contracts do not require
every provider to advertise 5 or prove that 5 causes enablement:
[Shell Clipboard Formats](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard),
[DROPEFFECT Constants](https://learn.microsoft.com/en-us/windows/win32/com/dropeffect-constants).

Root can apply this patch only to its isolated checkout at the exact base and
run the existing headless native transfer fixture in a fresh hosted VM. Retain
the raw registered/view/folder facts and actual output proof even if the final
independent oracle assertion fails. No production adoption is established until
that genuine run. A subsequent repair still needs whole-array provider
eligibility and Copy/Cut clipboard ownership/lifecycle review.

## Isolated hosted workflow

`build-experiment.yml` is copied from the exact base workflow. Its only changes
are the experiment name, a push filter for
`verify/copy-preference-five-20261006`, removal of the pull-request trigger,
and retaining only the original `native-transfer` job. The transfer build, both
strict one-test JUnit gates, independent drop step after clipboard failure,
and unconditional raw-report upload are byte-exact to the base. The pull-request
trigger is removed; this experiment accepts only that push branch or manual
`workflow_dispatch`. No PR is needed.

Root can create an isolated worktree/branch at the recorded base, apply this
frozen patch, and replace that worktree's `.github/workflows/build.yml` with
this artifact. Only Root may commit/push the experiment. Do not copy the
workflow into the live main worktree or merge it as a release workflow. The
job remains a fresh hosted `windows-2022` VM with genuine-CI and explicit
transfer opt-in checks. It publishes reports only and no application release.
Neither a verified-output marker nor the experiment job is production
clipboard compatibility proof; retain the strict test exit and both XML/log
reports even if the standalone folder oracle remains absent.
