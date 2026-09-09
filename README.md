# HyprExpo

HyprExpo is a maintained Hyprland plugin for expose-style workspace overview with keyboard selection, drag-drop window movement, labels, configurable gaps and borders, multi-monitor placement, and Lua gestures.

https://github.com/user-attachments/assets/861baa26-46b6-4fa8-8d37-65cbb9ecbed4

Native Hyprland scrolling-layout workspaces open a separate window-level
scrolling overview. It preserves the full offscreen tape, supports pointer,
touch, keyboard, panning, and positional window moves, and reuses grid captures
for mixed-layout rows. This is intentionally not full Niri parity: hot corners,
dwell activation, layer-shell/wallpaper composition, and arbitrary workspace
insertion are outside the implemented contract.

If you experience any bugs, you are encouraged to [open an issue](https://github.com/sandwichfarm/hyprexpo/issues/new). Information I can use to reproduce a bug is appreciated. 

[Docs (markdown)](docs/index.md) - [Docs (website)](http://hyprexpo.lol/docs) - [Announcement Post](https://www.reddit.com/r/hyprland/comments/1o30dsg/hyprexpoplus_outer_gaps_keyboard_navigation_and/)

## History

HyprExpo continues the original expose-style workspace overview plugin from the Hyprland plugins ecosystem. After [the upstream plugin was retired](https://github.com/hyprwm/hyprland-plugins/pull/507#issuecomment-4433386463) from official plugins, this fork signaled contiuation and intends to chase Hyprland releases.

Born from [a PR to the old official HyprExpo](https://github.com/hyprwm/hyprland-plugins/pull/507) and formerly known as HyperExpo+ (`hyprexpo-plus`), has become the home for practical additions that made the
overview more usable day to day: keyboard navigation, visible workspace labels, configurable gaps and borders, multi-monitor placement, and Lua gesture setup.
See the [upstream retirement context](https://github.com/hyprwm/hyprland-plugins/pull/663)
and the [original launch announcement of this plugin](https://www.reddit.com/r/hyprland/comments/1o30dsg/hyprexpoplus_outer_gaps_keyboard_navigation_and/)
for the project's well established background.

## Related

- https://github.com/colonelpanic8/hyprexpo - Another HyprExpo fork 

____

## Branches and Releases

This development candidate pins Hyprland
`34eb03bd8da01024596c367fba66485a8c9b8ca7`. Use the
[development installation guide](docs/guides/development-installation.md) for
its matching source, dependencies, and validation boundaries.

`master` is the default branch for supported, released Hyprland versions.
The separate `hyprland-git` track targets explicitly tested upstream
development commits. Compatible fixes flow from `master` into the chase branch;
support for a new Hyprland release is promoted back through a validated PR
using the compatible candidate commit. Older `release/*` branches are optional
and require an explicit maintenance commitment.

See the [branch and release policy](docs/reference/branch-policy.md) for branch
contracts, promotion gates, package and pin handling, and the rollout checklist.
Use the [development installation guide](docs/guides/development-installation.md)
for explicit hyprpm revision selection and aligned Nix inputs.

## Install

`master` targets tagged Hyprland **v0.56.1 and v0.56.2**. Build against the
exact revision and dependencies used by your compositor. Hyprland-git is a
separate compatibility target; see [compatibility and release provenance](docs/reference/compatibility.md).

### hyprpm

```bash
hyprpm add https://github.com/sandwichfarm/hyprexpo
hyprpm enable hyprexpo
hyprpm reload
```

The repository name in `hyprpm.toml` is `hyprexpo`, and the built plugin output is `hyprexpo.so`.
The release pins from PR #112 intentionally select earlier compatible plugin
commits for v0.56.1 and v0.56.2. A source fix on master does not automatically
change those pins.

### Build From Source

Install a C++23 compiler, `pkg-config`, Hyprland development headers, and these pkg-config packages:

```text
hyprland pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon lua5.4
```

The build prefers the `lua5.4` pkg-config module and falls back to `lua` for
distributions such as Fedora where `lua-devel` exposes the generic module name.

Build with the Makefile:

```bash
git clone https://github.com/sandwichfarm/hyprexpo
cd hyprexpo
make all
```

For day-to-day development, prefer a disposable nested Hyprland session. This
matches Hyprland's plugin development guidance: build the plugin, load it by
absolute path with `hyprctl plugin load`, then unload and load again after
changes.

```bash
./scripts/run-nested.sh
```

If you already have a disposable Hyprland session running, build to a
user-owned cache path and load or reload that `.so` directly:

```bash
make dev-load
make dev-reload
```

Only replace the hyprpm-managed copy when you intentionally want the installed
plugin to point at this checkout's build:

```bash
make install
hyprpm reload
```

If your distro or install path stores hyprpm artifacts under a root-owned cache,
keep privilege at the command line instead of baking `sudo` into the Makefile:

```bash
sudo make install INSTALL_USER="$USER"
hyprpm reload
```

Use `install` or `make install`, not plain `cp`, when replacing a loaded `.so`.
Hyprland maps plugin files into the running process, and overwriting that file
in place can corrupt the live mapping.

Other build entry points:

```bash
meson setup build
meson compile -C build
```

```bash
cmake -S . -B build
cmake --build build
```

### Nix

Nix users should build HyprExpo through the Nix Hyprland plugin path instead of mixing a `hyprpm` artifact into a Nix-managed Hyprland session. This repository includes `default.nix`, which uses `hyprlandPlugins.mkHyprlandPlugin` so the plugin follows the Hyprland input supplied by the caller.

Hyprland plugins are ABI-sensitive. Keep the plugin build and running Hyprland revision aligned.
The `master` flake defaults to v0.56.2. This development candidate instead
pins the upstream commit above. Select a compatible plugin branch and align
its Hyprland input with the running system; overriding development source to
a released Hyprland input does not make the APIs compatible.

## Quick Config

Add the plugin block to your Hyprland config:

```ini
plugin {
    hyprexpo {
        columns = 3
        rows = 0 # Follow columns; use a positive value for a rectangular fixed grid.
        gaps_in = 5
        gaps_out = 0
        bg_col = rgb(111111)
        workspace_method = center current
        gesture_distance = 200
        cancel_key = escape
        show_cursor = 1
        show_pinned_windows = 0
        drag_drop_enable = 0 # Disable moving windows by dragging workspace previews.
    }
}
```

For `hyprland.lua`, use `hl.config()`:

```lua
hl.config({
    plugin = {
        hyprexpo = {
            columns = 3,
            rows = 0, -- Follow columns; positive values set fixed-grid rows.
            gaps_in = 5,
            gaps_out = 0,
            bg_col = "rgb(111111)",
            workspace_method = "center current",
            gesture_distance = 200,
            cancel_key = "escape",
            show_cursor = 1,
            drag_drop_enable = 0, -- Disable moving windows by dragging workspace previews.
        },
    },
})
```

`drag_drop_enable` defaults to `1`. Set it to `0` to keep workspace clicks from moving windows when the pointer shifts during a click.

For ten fixed-grid slots, use `columns = 5`, `rows = 2`, `dynamic_grid = 0`,
and `skip_empty = 0`. Empty workspaces remain selectable and can receive dragged
windows. `rows = 0` (the default) keeps the existing square grid; positive rows
are clamped to `1..7`. Dynamic grids and native scrolling overviews size themselves
as before and ignore `rows`.

Add a dispatcher binding:

```ini
bind = SUPER, g, hyprexpo:expo, toggle
```

Or in Lua:

```lua
hl.bind("SUPER + G", function()
    hl.plugin.hyprexpo.expo("toggle")
end)
```

Optional keyboard navigation:

```ini
plugin {
    hyprexpo {
        keynav_enable = 1
        number_key_mode = passthrough
        keynav_wrap_h = 1
        keynav_wrap_v = 1
        keynav_reading_order = 0
    }
}
```

`number_key_mode` controls the plugin's automatic raw digit handling:

- `workspace` (default) keeps selecting global workspace IDs.
- `index` selects positions in the active overview; for example, `2` selects
  its second visible tile even when that tile is workspace 11.
- `passthrough` leaves digits to user-defined mappings such as the `kb_selecti`
  bindings below.

```ini
submap = hyprexpo
    bind = , left,   hyprexpo:kb_focus, left
    bind = , right,  hyprexpo:kb_focus, right
    bind = , up,     hyprexpo:kb_focus, up
    bind = , down,   hyprexpo:kb_focus, down
    bind = , return, hyprexpo:kb_confirm
    bind = , escape, hyprexpo:expo, cancel
    bind = , 1,      hyprexpo:kb_selecti, 1
    bind = , 2,      hyprexpo:kb_selecti, 2
    bind = , 3,      hyprexpo:kb_selecti, 3
    bind = , 4,      hyprexpo:kb_selecti, 4
    bind = , 5,      hyprexpo:kb_selecti, 5
    bind = , 6,      hyprexpo:kb_selecti, 6
    bind = , 7,      hyprexpo:kb_selecti, 7
    bind = , 8,      hyprexpo:kb_selecti, 8
    bind = , 9,      hyprexpo:kb_selecti, 9
    bind = , 0,      hyprexpo:kb_selecti, 10
submap = reset
```

For `hyprland.lua`, define the same active submap in Lua instead of adding a
`submap = hyprexpo` block to `hyprland.conf`:

```lua
hl.define_submap("hyprexpo", function()
    hl.bind("h",      function() hl.plugin.hyprexpo.kb_focus("left") end)
    hl.bind("l",      function() hl.plugin.hyprexpo.kb_focus("right") end)
    hl.bind("k",      function() hl.plugin.hyprexpo.kb_focus("up") end)
    hl.bind("j",      function() hl.plugin.hyprexpo.kb_focus("down") end)
    hl.bind("return", function() hl.plugin.hyprexpo.kb_confirm() end)
    hl.bind("escape", function() hl.plugin.hyprexpo.expo("cancel") end)
end)
```

## Active workspace grid

For a more dynamic workspace grid with labels and wallpaper background:

```
plugin {
    hyprexpo {
        dynamic_grid = 1
        fill_gaps = 0
        mru_sort = 0
        show_workspace_names = 1
        label_pos = top_right
        label_size = 48
        wallpaper_bg = 1
    }
}
```

For more options, see the [configuration options](https://hyprexpo.lol/docs/configuration/options/).

## Next Steps

- [Repository layout and development-branch reconciliation](docs/reference/repository-layout.md)

- [Installation details](https://hyprexpo.lol/docs/getting-started/installation/)
- [Quick start](https://hyprexpo.lol/docs/getting-started/quick-start/)
- [All configuration options](https://hyprexpo.lol/docs/configuration/options/)
- [Labels and borders](https://hyprexpo.lol/docs/configuration/labels-borders/)
- [Keyboard navigation](https://hyprexpo.lol/docs/configuration/keyboard/)
- [Lua gestures](https://hyprexpo.lol/docs/guides/lua-gestures/)
- [Multi-monitor placement](https://hyprexpo.lol/docs/guides/multi-monitor/)
- [Migration from old keyword config](https://hyprexpo.lol/docs/guides/migration/)
- [Runtime smoke checklist](https://hyprexpo.lol/docs/guides/runtime-smoke/)
- [Scrolling overview guide](https://hyprexpo.lol/docs/guides/scrolling-overview/)
- [Compatibility and release provenance](https://hyprexpo.lol/docs/reference/compatibility/)
- [Dispatcher reference](https://hyprexpo.lol/docs/reference/dispatchers/)
- [Troubleshooting](https://hyprexpo.lol/docs/troubleshooting/)
