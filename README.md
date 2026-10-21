<div align = center>

<img src="https://raw.githubusercontent.com/hyprwm/Hyprland/main/assets/header.svg" width="750" height="300" alt="banner">

Hyprland is a 100% independent, dynamic tiling Wayland compositor that doesn't sacrifice on its looks.

It provides the latest Wayland features, is highly customizable, has all the eyecandy, the most powerful plugins,
easy IPC, much more QoL stuff than other compositors and more...

This is my own experimental branch for HDR issue fix.
<br>
<br>

---

<br>

</div>

# AI-powered maintenance

This fork uses AI-powered automated maintenance, including upstream synchronization and issue/PR handling. Human maintainers periodically review the project and its changes to ensure code quality.

Your [issues](https://github.com/gfhdhytghd/Hyprland/issues) and [pull requests](https://github.com/gfhdhytghd/Hyprland/pulls) will usually be handled or receive a response within three hours.

# Building this fork

This fork's HDR / color-management work needs DRM color pipeline support that upstream aquamarine doesn't have yet. Build it against the [aquamarine fork](https://github.com/gfhdhytghd/aquamarine), branch `drm-color-pipeline`:

- `IOutput::supportsCTM()` and `IOutput::getSupportedColorspaces()` (connector Colorspace values matched by name)
- working DEGAMMA / GAMMA LUTs and CTM with correct blob lifetime
- clearing `HDR_OUTPUT_METADATA` (property set to 0) when returning to SDR

The fork keeps aquamarine's version at `0.15.1` but bumps the SOVERSION to 15, so pkg-config accepts a stock aquamarine 0.15.x and the build then fails on the missing API. Make sure the fork is the one found.

Everything else is built as described in the [installation guide][Install] (manual / CMake build).

**Install the aquamarine fork into a separate prefix** (no root needed):

```sh
git clone -b drm-color-pipeline https://github.com/gfhdhytghd/aquamarine
cmake -S aquamarine -B aquamarine/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local/aquamarine-hdr"
cmake --build aquamarine/build -j"$(nproc)"
cmake --install aquamarine/build
```

**Build Hyprland against it:**

```sh
export PKG_CONFIG_PATH="$HOME/.local/aquamarine-hdr/lib/pkgconfig:$PKG_CONFIG_PATH"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The binary in `build/` carries a RUNPATH to the fork. `cmake --install` strips it, so for an installed Hyprland either add `-DCMAKE_INSTALL_RPATH="$HOME/.local/aquamarine-hdr/lib"` at configure time or start it with `LD_LIBRARY_PATH="$HOME/.local/aquamarine-hdr/lib"`. Installing the fork to `/usr` instead is not recommended: its library file is also named `libaquamarine.so.0.15.1`, so it overwrites the file the distribution's `libaquamarine.so.14` symlink points to, and everything built against SOVERSION 14 (a distribution Hyprland, hyprlock, …) then loads an ABI-incompatible library until rebuilt.

**Nix:** the flake already pins the aquamarine fork (`github:gfhdhytghd/aquamarine/drm-color-pipeline`), so `nix build` needs no extra steps.

**CI** builds against the same branch (`.github/actions/setup_base`, `flake.nix`).

# Features

- All of the eyecandy: gradient borders, many types of blur, animations, glow, shadows and much more
- A lot of customization
- 100% independent, no wlroots, no libweston, no kwin, no mutter.
- Custom bezier and spring curves for the best animations
- Powerful plugin support
- Built-in plugin manager
- Tearing support for better gaming performance
- Easily expandable and readable codebase
- Fast and active development
- Not afraid to provide bleeding-edge features
- Config reloaded instantly upon saving
- Global keybinds passed to your apps of choice
- Tiling/pseudotiling/floating/fullscreen windows
- Special workspaces (scratchpads)
- Window groups (tabbed mode)
- Powerful window/monitor/layer rules
- Socket-based IPC
- Native IME and Input Panels Support
- Fully dynamic workspaces
- Extensive layout support
    - Dwindle
    - Scrolling
    - Master
    - Monocle
    - Custom layout with Lua
    - Custom layouts with plugins
- Per Workspace Layouts
- and much more...

<br>
<br>

<div align = center>

# Gallery

<br>

![Preview A]

<br>

![Preview B]

<br>

![Preview C]

<br>
<br>

</div>

# Special Thanks

<br>

**[wlroots]** - *For powering Hyprland in the past*

**[tinywl]** - *For showing how 2 do stuff*

**[Sway]** - *For showing how 2 do stuff the overkill way*

**[Vivarium]** - *For showing how 2 do stuff the simple way*

**[dwl]** - *For showing how 2 do stuff the hacky way*

**[Wayfire]** - *For showing how 2 do some graphics stuff*

**[KWin]** - *For showing how 2 do HDR and color management right*


<!----------------------------------------------------------------------------->

[Configure]: https://wiki.hypr.land/configuring/
[Stars]: https://starchart.cc/hyprwm/Hyprland
[Hypr]: https://github.com/hyprwm/Hypr

[Pull Requests]: https://github.com/hyprwm/Hyprland/pulls
[Issues]: https://github.com/hyprwm/Hyprland/issues
[Todo]: https://github.com/hyprwm/Hyprland/projects?type=beta

[Contribute]: https://wiki.hypr.land/contributing-and-debugging/
[Install]: https://wiki.hypr.land/getting-started/installation/
[Quick Start]: https://wiki.hypr.land/getting-started/master-tutorial/
[Workflow]: https://github.com/hyprwm/Hyprland/actions/workflows/ci.yaml
[License]: LICENSE


<!----------------------------------{ Thanks }--------------------------------->

[Vivarium]: https://github.com/inclement/vivarium
[WlRoots]: https://gitlab.freedesktop.org/wlroots/wlroots
[Wayfire]: https://github.com/WayfireWM/wayfire
[TinyWl]: https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/tinywl/tinywl.c
[Sway]: https://github.com/swaywm/sway
[DWL]: https://codeberg.org/dwl/dwl
[KWin]: https://invent.kde.org/plasma/kwin

<!----------------------------------{ Images }--------------------------------->

[Preview A]: ./assets/prev1.png
[Preview B]: ./assets/prev2.png
[Preview C]: ./assets/prev3.png


<!----------------------------------{ Badges }--------------------------------->

[Badge Workflow]: https://github.com/hyprwm/Hyprland/actions/workflows/ci.yaml/badge.svg

[Badge Issues]: https://img.shields.io/github/issues/hyprwm/Hyprland
[Badge Pull Requests]: https://img.shields.io/github/issues-pr/hyprwm/Hyprland
[Badge Language]: https://img.shields.io/github/languages/top/hyprwm/Hyprland
[Badge License]: https://img.shields.io/github/license/hyprwm/Hyprland
[Badge Lines]: https://img.shields.io/tokei/lines/github/hyprwm/Hyprland
[Badge Hi Mom]: https://img.shields.io/badge/Hi-mom!-ff69b4
