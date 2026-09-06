# Patch series

Numbered patches applied by `install.sh` to the upstream libosmocore tree,
after the in-place edits (the `setresgid` rewrite, the `#ifdef __linux__`
wrapping, `darwin_stubs.c` and `darwin_compat.h`) and before `autoreconf`.

Naming is `NNN-short-description.patch`, three digits, zero filled. The number
is the order of application. Each file carries a `git format-patch` compatible
header and a body that names the Darwin problem, the root cause and why the
fix is shaped the way it is.

`install.sh` skips a patch that is already applied, so re-running it on an
existing build tree is safe.

Patches touch upstream sources only. `darwin_stubs.c` and `darwin_compat.h`
belong to this repository and are edited directly instead.
