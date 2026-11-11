# exsh changelog

Versions are `MAJOR.MINOR.PATCH` (see [README.md](README.md#versions)): MAJOR for a change that
breaks a script that worked before, MINOR for new features or changed behaviour, PATCH for fixes
only. One entry per release that changes exsh.

## 2.0.0 (PSCAL V.1R34)

The first numbered release. `VERSION` held `2026-09-19-1`, a date never bumped.

- bash scripts are meant to just work: this round closes most of the bash differences found in
  testing, among them word splitting by IFS, job control in scripts (process groups, Ctrl-C and
  Ctrl-Z, `jobs`, `wait`, job specs), subshells and compound pipeline stages, process
  substitution, redirections, traps and signals ignored on entry, `history`'s options, and a
  stray `)` as a syntax error.
- On iOS the shell behaves as on Linux: processes, signals, `/dev/fd`, `/proc` and terminals
  follow POSIX in the app.
- `exsh --version`, `$EXSH_VERSION` and `EXSH_VERSINFO`.

The details are in the PSCAL release notes, `RELEASE_NOTES_V.1R33.md` and
`RELEASE_NOTES_V.1R34.md` in the [pscal](https://github.com/emkey1/pscal) repository, under
"Fixed in exsh", "Differs from bash" and "Fixed in the iOS app".
