# Compatibility and Release Provenance

Hyprland checks plugin API compatibility when loading a plugin. If HyprExpo was built against an incompatible Hyprland revision, loading should fail with a visible API/hash mismatch instead of silently running against the wrong ABI.

## Build Provenance

Release builds attach `release-provenance.txt` next to `hyprexpo.so`. That file records:

- `hyprctl version`
- `pkg-config --modversion hyprland`
- `pkg-config --modversion lua5.4 || pkg-config --modversion lua`
- compiler version
- `ldd -r hyprexpo.so`

## Supported Releases

`master` targets these tagged Hyprland releases:

| Release | Hyprland source commit |
| --- | --- |
| v0.56.1 | `5c9377c15f85c50648f35ca5a213754f95b93ca0` |
| v0.56.2 | `efb50993780079460b0cbed1363e2166a2de1d9f` |

Build the plugin against the revision and dependency ABI that will load it.
A successful build for one release does not make its `.so` interchangeable
with the other release. Compile checks also do not prove compositor runtime
behavior, physical gestures, or the Nix derivation on every platform.

PR #109 introduced development-only window and keybind APIs and removed the
flake release reference. Its revert restores the release APIs while retaining
the other fixes. [Issue #113](https://github.com/sandwichfarm/hyprexpo/issues/113)
tracks the recovery and the affected feature PRs.

## hyprpm and Nix

The release pins introduced by PR #112 now select the combined, tested source
on master (including the issue #110 fix and the PR #116 feature integration):

| Hyprland release | Pinned HyprExpo commit |
| --- | --- |
| v0.56.1 | `5891014c611e1bd56d0121143f0221d46b5c0967` |
| v0.56.2 | `5891014c611e1bd56d0121143f0221d46b5c0967` |

Changing current source does not update what those pins select. Before moving
a pin, build and test the proposed plugin commit against that exact tagged
Hyprland release, verify the plugin commit is an ancestor of the published
branch with `make check-pins REF=HEAD`,
and validate loading in a disposable matching compositor.

PR #116 was squash-merged, so its former integration-branch pin was not an
ancestor of master. Both pins now select the landed version commit after fresh
separate builds and matching disposable runtime acceptance on both releases.
The v0.56.2+1 pins additionally include PR #118's rectangular fixed grids and
PR #119's current HyprPM description. That landed master tree was again built
and loaded against both supported releases before the pins moved.
See [feature integration](./feature-integration.md) for the historical integration
checkpoints, release revalidation and remaining test boundaries.

The `master` flake has an explicit v0.56.2 reference; this development candidate
pins `34eb03bd8da01024596c367fba66485a8c9b8ca7`. Nix users should keep the plugin's
Hyprland input aligned with their system input through `default.nix` and the
Hyprland plugin packaging path. Overrides must select a supported release;
following an arbitrary development revision is not a compatibility guarantee.

## Release Verification

For the recovered base and each reconstructed feature PR, run `make test`,
`make check-pins REF=HEAD`, and a forced plugin build against **each** tagged
release. Use separate output directories and isolated include paths. `make`
does not track a changed pkg-config environment as a target dependency, so
switching headers and running an ordinary `make all` can reuse the wrong binary.

Record the candidate plugin commit, upstream tag commit, generated-header and
dependency provenance, compiler invocation, and artifact checksum per build.
The packaged non-generated Hyprland headers must match the tagged source;
generated headers must come from that release's build or a verified package.
Do not patch newer headers to stand in for a release. Repeat tests and builds
after resolving PR conflicts, and review feature preservation separately from
whether the compiler accepts the code.

## Hyprland-git Support

See the [workflow validation receipt](./workflow-validation.md) for the tested
release and development artifacts, consumer evidence, and verification limits.

See the [branch and release policy](./branch-policy.md) for contribution targets,
release promotion rules, and the chase workflow. The
[development installation guide](../guides/development-installation.md) explains
explicit hyprpm revision selection, Nix input alignment, and rollback.

Develop git support on a separate branch based on the release-compatible
master. Pin that branch's Hyprland input to an explicit upstream commit and
record its dependency ABI; an unpinned input makes failures unreproducible.
Keep release hyprpm pins intact and distribute git artifacts separately.

Port each API boundary with its existing behavior and tests preserved. Check
window mapping, pinned-window save/restore, alpha animations, keybind modifiers,
and exact-submap restoration when adopting the newer window/keybind APIs.
Before merging any shared compatibility code, require the v0.56.1/v0.56.2
build matrix to stay green and run a separate build/load test for the pinned
git revision. A future tagged release can become a supported default after
those checks and matching nested interaction tests pass.
