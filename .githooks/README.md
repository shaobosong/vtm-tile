# Commit message hook

The recent history uses English imperative subjects such as
`Add Disconnect action to Parvion Quick Connect bar` and
`Fix horizontal scrollbar thumb press highlight`, rather than Conventional
Commits (`feat:`, `fix(scope):`, etc.). Occasional component prefixes such as
`parvion: contain mouse gestures within the app body` are also supported.

The `commit-msg` hook checks these mechanical conventions:

- A nonempty English subject starting with a capitalized word and a description.
  A lowercase component prefix allows a lowercase first word as in the history.
- No final period, exclamation mark, or question mark in the subject.
- A blank line between the subject and an optional body.
- Every subject and body line must fit within 80 bytes, equivalent to 80 ASCII
  characters. UTF-8 characters use multiple bytes. This limit also applies to
  merge and autosquash messages, including their prefixes.

Use an imperative verb (Add, Fix, Refactor, Preserve, etc.) and describe the
concrete change. Grammar and meaning require human review; the hook does not
maintain a restrictive verb whitelist. Bodies may explain the problem, behavior,
implementation, and validation in any language. Git comments are ignored using
the configured comment character. Merge messages are accepted, and `fixup!`,
`squash!`, and `amend!` prefixes are supported. Standard `Revert "..."` subjects
pass the same sentence-style checks.

Enable from the repository root (requires Git and a POSIX shell with awk;
Git for Windows includes both):

```sh
git config --local core.hooksPath .githooks
```

On Unix, also make the hook executable if the checkout does not preserve its mode:

```sh
chmod +x .githooks/commit-msg
```

This config selects `.githooks` for all hooks in this clone. If you already use a
different hooks directory, integrate this hook there instead. To return to Git's
default hooks directory:

```sh
git config --local --unset core.hooksPath
```
