# Commit messages

Use an English action-oriented title, a blank line, and explanatory paragraphs.
The title describes the change to the component or behavior. Put the concrete
failure case, affected application, reproduction, and symptoms in the body when
they explain why the change is needed. An application name belongs in the title
when that application is itself the component being changed.

For example:

```text
Correct terminal device status responses

Reply to CSI 5 n with CSI 0 n instead of the literal text OK. Neovim
0.12.x treats the old response as keyboard input during startup, entering
Insert mode and inserting unwanted text.

Add coverage for single and repeated queries and cursor position reports.
Verify terminal input and Neovim startup with clean and default settings.
```

Keep the explanation proportional to the change. A short paragraph is enough
for a small change. Explain the reason and resulting behavior; add implementation
details and validation when they help a reviewer. Lists and Git trailers may
follow the opening paragraph. Do not invent testing claims to fill a template.

## Basis in the history

The latest 100 commits reviewed through `18794009` all have bodies. Their titles
use English actions such as Add, Fix, Unify, Preserve, and Correct; one uses the
older `parvion:` component prefix. Ninety-nine titles fit within 80 bytes. One
548-byte title reported by `git log --format=%s` folds explanatory text into
the subject and is treated as an outlier. All body lines fit within 80 bytes.
Replaying the new hook accepts 97 messages and rejects three that omit the blank
line between title and body (`209bd29c`, `ce60a2c2`, and `e4814603`).

The required body also reflects the project preference for a title plus
explanation. The preference for behavior-level titles and concrete cases in the
body refines the history; some older titles describe particular symptoms.

## Enforced by the hook

- A nonempty English sentence-style title starting with a capitalized word and
  a description. Legacy lowercase component prefixes remain supported, for
  example `parvion: contain mouse gestures within the app body`.
- No final period, exclamation mark, or question mark in the title.
- A blank line followed by a required body whose first nonempty line is prose,
  rather than a list, heading, quotation, code fence, or Git trailer.
- Every line fits within 80 UTF-8 bytes, including any autosquash prefix.

Use an imperative action word. Grammar, relevance, the distinction between a
component and a reproduction case, and the quality of the explanation require
human review. The hook does not pretend to enforce these using a verb whitelist,
application-name blacklist, or required section headings. Conventional Commits
such as `fix(term): ...` are not the repository format.

Git-generated merge titles and `fixup!`/`squash!` messages may omit the body.
The line-length and separator checks still apply. `amend!` messages replace the
original message and require explanatory prose. Ordinary reverts require a body
too. Comments are removed using Git's configured comment character, and CRLF
input is accepted.

## Setup and verification

Enable from the repository root (requires Git and a POSIX shell with awk;
Git for Windows includes both):

```sh
git config --local core.hooksPath .githooks
```

On Unix, also make the hook executable if the checkout does not preserve its mode:

```sh
chmod +x .githooks/commit-msg
```

Run the regression cases and validate a message file directly:

```sh
sh .githooks/test-commit-msg.sh
sh .githooks/commit-msg /path/to/message
```

This config selects `.githooks` for all hooks in this clone. If another hooks
directory is already in use, integrate this hook there instead. To return to
Git's default hooks directory:

```sh
git config --local --unset core.hooksPath
```
