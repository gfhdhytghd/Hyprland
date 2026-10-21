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

This fork's HDR / color-management work needs DRM color pipeline support that upstream aquamarine doesn't have yet. Build it against the [aquamarine fork](https://github.com/gfhdhytghd/aquamarine), branch `master`:

- `IOutput::supportsCTM()` and `IOutput::getSupportedColorspaces()` (connector Colorspace values matched by name)
- working DEGAMMA / GAMMA LUTs and CTM with correct blob lifetime
- clearing `HDR_OUTPUT_METADATA` (property set to 0) when returning to SDR

The fork keeps aquamarine's version at `0.15.1` but bumps the SOVERSION to 15, so pkg-config accepts a stock aquamarine 0.15.x and the build then fails on the missing API. Make sure the fork is the one found.

Everything else is built as described in the [installation guide][Install] (manual / CMake build).

**Install the aquamarine fork into a separate prefix** (no root needed):

```sh
git clone -b master https://github.com/gfhdhytghd/aquamarine
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

**Nix:** the flake already pins the aquamarine fork (`github:gfhdhytghd/aquamarine/master`), so `nix build` needs no extra steps.

**CI** builds against the same branch (`.github/actions/setup_base`, `flake.nix`).

# Legacy GLES2 renderer

Hyprland normally needs OpenGL ES 3.0. This fork falls back to a GLES 2.0 renderer when the driver can't create a GLES3 context, so it also runs on roughly 2010-era GPUs, e.g. Intel GMA 950 / X3100 / X4500 and first-generation HD Graphics (Mesa `i915` / `crocus`), or ATI Radeon X1000-series cards (Mesa `r300`). There is nothing to configure; the log says `Running the legacy GLES2 renderer` when it is active.

**Requirements:** Mesa (or another driver) exposing `GL_OES_vertex_array_object`, `GL_EXT_unpack_subimage` and `GL_EXT_texture_format_BGRA8888`, plus EGL dmabuf import.

**Not available on GLES2:**

- color management: HDR, wide gamut, ICC profiles. `color-management-v1` isn't advertised, so clients send plain sRGB.
- FP16 work buffers, 10-bit outputs and 10-bit SHM formats
- `render:keep_unmodified_copy` (needs a second color attachment)
- motion blur, and blur variants other than Kawase. Other variants fall back to Kawase with a one-time notice.
- `decoration:screen_shader` must be written in GLSL ES 1.00 (`#version 100` or no `#version`)

**Blur** works with the default Kawase variant. The blurred background is always blended in the window shader, as with `render:use_shader_blur_blend`, because GLES2 has no depth-stencil textures for the stencil path.

**Performance** was checked by emulating a ThinkPad T60-class GPU (Mesa llvmpipe calibrated to GMA 950 / Radeon X1400 fill rate) at 1400x1050 with continuously animated content:

| Scenario | T60 class | 2010 Intel HD class |
|---|---|---|
| no blur | 60 FPS, ~6 ms per frame | 60 FPS, ~4 ms |
| blur, tiled windows (precomputed blur) | 60 FPS, ~6.5 ms | 60 FPS, ~4.5 ms |
| live blur behind an 800x520 floating window | ~58 FPS, ~16 ms | 60 FPS, ~11 ms |

Live blur behind floating windows is the expensive part and sits right at the 60 FPS budget on T60-class hardware. On such machines set `decoration:blur:xray = true` (floating windows reuse the precomputed blur), and keep `decoration:blur:passes` and `decoration:blur:new_optimizations` at their defaults (1 and on). These numbers are emulated; GMA 950 shader instruction limits and real driver overhead have not been verified on hardware yet. When measuring yourself, note that `debug:overlay` blurs its own background, which adds two live blurs per frame.

**Testing on a modern GPU:** run Hyprland nested with Mesa limited to GLES 2.0, `MESA_GLES_VERSION_OVERRIDE=2.0 Hyprland`. `MESA_DEBUG=1` together with `debug:gl_debugging = true` reports any GLES3-only call.

# Features

- All feature Hyprland have
- Runs on GLES 2.0-only GPUs (see [Legacy GLES2 renderer](#legacy-gles2-renderer))
- Superfast issue&PR response

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
