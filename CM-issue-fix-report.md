# CM-issue-fix 修复与验证记录

- 仓库：`/home/wilf/data/Hyprland`，本地分支：`CM-issue-fix`。
- 基线：`1bd31aaa5`。按问题拆分提交；跨问题的公共实现和集中回归检查使用独立提交。
- 范围：[hypr-bug 文件清单](https://github.com/gfhdhytghd/hypr-bug/)，尤其 `pending-evaluation/2026-10-color-management-audit-list.md` 的 29 项，以及 [Hyprland #16329](https://github.com/hyprwm/Hyprland/issues/16329)。该清单仓库没有 GitHub issue，采用仓库内文件作为输入。
- 下表是实现记录，不表示 29 项均已得到实体 HDR 显示的复现与效果确认。仅本地代码、提交和隔离测试；未推送、未修改当前桌面配置、未重载当前桌面。

## 逐项实现

| 清单编号 | 实现 | 主要提交 |
| --- | --- | --- |
| 1 | 反馈对象在 surface 销毁后转为 inert，避免悬空引用。 | `2f13a61ba` |
| 2 | 拒绝未 ready / failed 的 image description 用于信息查询或 surface。 | `24f16580c` |
| 3 | 颜色 description 随 pending surface state 合并并在 commit 生效；销毁扩展时排队 unset，保留已提交状态并损伤 surface。 | `2db127690`, `e1c3ddfcf`, `199acbd15` |
| 4 | PQ 编码范围统一为 min + 10000，与 mastering / 内容峰值分开；忽略客户端指定的 PQ maximum，匹配 PQ 编码仍检查实际显示峰值。 | `913e322f1`, `dcf165309`, `d8e79e338` |
| 5 | 修正 Windows HDR 预设的完整信号亮度范围，保留其 reference 语义。 | `2b4614ced` |
| 6 | creator 必须包含 primaries 和唯一 transfer function。 | `3cd21522a` |
| 7 | 按协议版本校验 CLL / FALL，拒绝不一致的内容和 mastering 峰值。 | `dcc8b1da1` |
| 8 | 拒绝零、倒置的 mastering 亮度范围。 | `58e5a678e` |
| 9 | 按绑定的协议版本广告和校验 named transfer function。 | `976529e7c` |
| 10 | TF power 不支持错误使用 creator 的正确 error enum。 | `efeacc415` |
| 11 | 对 inert CM surface 的请求返回协议错误。 | `98954f634` |
| 12 | ICC output description 改变时更新身份并通知客户端。 | `68d5edf49` |
| 13 | 实现含黑白端点的 BT.1886 EOTF / inverse；保留 xvYCC 的 BT.709 曲线。 | `c881faa9d` |
| 14 | 实现 HLG reference-display OOTF / inverse，使用源和目标 primaries 的亮度系数。 | `7202f0fb2` |
| 15 | tone mapping 保留扩展线性色域的负分量。 | `7c92a0a8e` |
| 16 | 修正 generic film primaries 通道顺序。 | `f7686f681` |
| 17 | 以 homogeneous chromaticity 构造 XYZ 矩阵，支持 y=0 的 XYZ basis；拒绝奇异自定义 primaries。 | `fe2da9188` |
| 18 | 像素解码使用 description 的亮度范围。 | `662b24b0f` |
| 19 | 缺少 HLG 输出元数据通路时禁用 HLG direct scanout，走合成到 PQ。 | `73e37220f` |
| 20 | 没有有效 EDID peak 时使用合法 HDR fallback，忽略非正 EDID peak。 | `220c5c634` |
| 21 | mirror 保留 work buffer 的格式及颜色 description。 | `157b59941` |
| 22 | HDR direct scanout 检查编码、primaries 和完整亮度范围兼容性；保留已有显式 ignore 行为。 | `1b14e5f46`, `82ea0604a` |
| 23 | DRM metadata 使用 mastering gamut；保留未知 CLL / FALL 为零，并限制 uint16 字段。 | `5149dd2d5` |
| 24 | capture 使用内容 / mastering 峰值，避免峰值由 FP16 格式决定。 | `6f1fde61e` |
| 25 | FP16 blur 的 SDR 亮度 / 饱和度在正确的线性光阶段补偿和恢复。 | `b8e480b47` |
| 26 | auto HDR 在 damage gate、资源准备和绘制之前确定，元数据改变触发 damage，并在离开 scanout 后重新检查。 | `4d075166d` |
| 27 | tone mapping 输入采用真实绝对 source peak，保持峰值单位一致。 | `63a4b0127`, `d8e79e338` |
| 28 | reference white 调整对像素、亮度范围和峰值使用同一比例，并覆盖相对 / 内部 extended-linear 区别。 | `369c1f43c` |
| 29 | 保留默认 HDR reference 203；通过 #16329 的 opt-in 输出选项独立配置 reference。 | `73f41a27b`, `0fc469268` |
| 16329 | 新增 Lua monitor reference_luminance；支持正整数和 -1 reset，并覆盖 forced HDR / 无 EDID 的 preferred description。 | `0fc469268`, `2919cd1ce` |


补充修复：HDR metadata 单次赋值；preferred description 身份和变化通知；ICC parametric preferred 请求拒绝；真实 ICC 输出文件信息；未知光级不发虚构信息；输出 description change 后发送 `wl_output.done`；停止广告未实现的 debug 功能；HDR 非预乘颜色的 tone mapping；SDR 亮度 / 饱和度的线性光处理及亮度矩阵行选择。

### 清单中需要区分的判断

- HLG 输入解码后已转换为绝对 display-light，再使用 PQ ICtCp 空间做 tone mapping；因此这里没有改用被注释的 HLG ICtCp 矩阵。已移除误导性的未使用矩阵和参数并说明处理阶段。
- I 与 Y、色度保留等艺术性 tone mapping 策略没有整体重设计，需要参考图和实体显示评估。
- ICC 输出信息与通知已修复，但未实现 ICC surface creator / 任意输入 ICC 渲染。现在正确报告不支持，避免虚假广告。

## #16329 配置

Lua monitor 增加 `reference_luminance`，单位 cd/m²，正整数；`-1` 恢复默认。`0` 和小于 `-1` 的值会被拒绝。原有 SDR 配置行为保留，HDR 默认 reference 仍为 203。

例如，需要将 HDR reference 和 SDR white 明确对齐到 450 时：

```lua
hl.monitor({
    output = "你的输出名称",
    cm = "hdr",
    reference_luminance = 450,
    sdr_max_luminance = 450,
})
```

这两个配置项独立，不自动相互覆盖，也不自动按 EDID 推导 reference。新选项需要用户另行提交 wiki PR。

## 验证结果和范围

| 检查 | 结果 | 能证明的范围 |
| --- | --- | --- |
| Hyprland、hyprland_gtests、color-management client、hyprtester 构建 | 通过 | 编译、链接及当前依赖 API 兼容 |
| 全量单元测试 | 677 / 677，75 suites | 数值、状态、配置和所覆盖的协议路径 |
| 隔离实际 Wayland server wire 测试 | 11 / 11，包含在上述 677 项中 | 实际 request / error、对象生命周期、description 和扩展销毁后的提交状态 |
| GLSL 离线编译 | 215 / 215 shader variants | 表面、边框、PQ、HLG、BT.1886、extended-linear、blur 的所测 shader 组合可编译 |
| 实际嵌套 Hyprland 客户端检查 | 21 / 21；compositor 存活 | inert / failed description、creator validation、反馈对象销毁、输出 reference 的实际协议事件 |
| reference 配置 CLI 检查 | 450 和 -1 接受；0 和 -2 拒绝 | 配置 parser 实际接受 / 拒绝行为 |
| Lua stubs、clang-format、git diff --check | 通过 | 新选项进入生成元数据，修改文件风格和 whitespace 合规 |

嵌套测试使用私有 `XDG_RUNTIME_DIR`、显式测试 instance signature、父 Wayland socket 和 `AQ_DRM_DEVICES=/dev/null`；在虚拟 `WAYLAND-1` 输出检查 reference 450、675 和 reset 后的 203。测试结束已退出该私有实例。

标准 hyprtester headless runner **未完成**：本环境的 headless 输出启动时没有可用 allocator / 出现 0×0 framebuffer 和 GBM 分配失败，无法进入测试。因此不宣称已通过新增的三个标准 hyprtester 场景。改用隔离 wire server 测试和实际嵌套 Wayland 实例取得上述证据；标准 runner 仍待可工作的 headless 环境验证。

实体 HDR 输出的亮度、视觉质量、mirror / capture、DRM direct scanout、auto HDR 切换仍待实机复现与回归确认。上述编译、shader 和协议检查不能替代这些验证。

临时证据日志：

- `/tmp/cm-issue-fix-unit-all3.log` 与最终 `/tmp/cm-issue-fix-unit-delivery.log`
- `/tmp/cm-issue-fix-shaders-delivery.log`
- `/tmp/cm-issue-fix-wire-tests3.log`
- `/tmp/cm-issue-fix-live-regressions.log`
- `/tmp/cm-issue-fix-build-debug.log` 与 `/tmp/cm-issue-fix-build-delivery.log`
- `/tmp/cm-issue-fix-headless.log`、`/tmp/cm-issue-fix-nested3.log`
- `/tmp/cm-verify-valid.log`、`/tmp/cm-verify-reset.log`、`/tmp/cm-verify-zero.log`、`/tmp/cm-verify-negative.log`

临时目录可能被系统清理；长期复查请使用本分支新增的 `tests/` 和 `hyprtester/` 回归源码。

## 提交索引

- `2f13a61ba` protocols/cm: make feedback inert when its surface is destroyed (#1)
- `24f16580c` protocols/cm: reject use of image descriptions that are not ready (#2)
- `98954f634` protocols/cm: reject requests on inert surface objects (#11)
- `3cd21522a` protocols/cm: require primaries and a single transfer function (#6)
- `58e5a678e` protocols/cm: reject empty mastering luminance ranges (#8)
- `efeacc415` protocols/cm: use the creator error enum for unsupported TF power (#10)
- `976529e7c` protocols/cm: validate named transfer functions against the bound version (#9)
- `b6e86f8f2` protocols/cm: enforce single assignment of HDR metadata (audit supplement)
- `dcc8b1da1` protocols/cm: validate content light levels when creating descriptions (#7)
- `2db127690` protocols/cm: commit image descriptions with queued surface state (#3)
- `f7686f681` cm: correct generic film primary channel order (#16)
- `fe2da9188` cm: build finite conversion matrices for XYZ basis primaries (#17)
- `c881faa9d` cm: implement the BT.1886 display EOTF and retain xvYCC BT.709 (#13)
- `e1c3ddfcf` protocols/cm: include surface state commit implementation dependency (#3)
- `913e322f1` cm: separate PQ encoding luminances from display mastering peak (#4)
- `2b4614ced` cm: give Windows HDR presets their full signal luminance range (#5)
- `68d5edf49` output/cm: notify clients when an ICC output description changes (#12)
- `e0225a07d` protocols/cm: send preferred changes using effective description identities (audit supplement)
- `7202f0fb2` cm: apply the HLG reference display OOTF and its inverse (#14)
- `7c92a0a8e` render/cm: preserve extended linear gamut during tone mapping (#15)
- `662b24b0f` render/cm: use description luminances for pixel decoding (#18)
- `63a4b0127` render/cm: pass the actual absolute source peak to tone mapping (#27)
- `6f1fde61e` render/cm: derive capture tone map peaks from content metadata (#24)
- `369c1f43c` render/cm: anchor reference whites while scaling pixels and peaks together (#28)
- `73f41a27b` output/cm: retain the default HDR reference before per-output overrides (#29)
- `73e37220f` output/cm: composite HLG rather than scan it out without HLG metadata (#19)
- `1b14e5f46` output/cm: require compatible HDR descriptions for direct scanout (#22)
- `5149dd2d5` render/cm: use mastering gamut and preserve unknown HDR content light levels (#23)
- `0fc469268` config/cm: add an opt-in per-output reference_luminance setting (#16329, audit #29)
- `157b59941` render/cm: preserve the work buffer format and color encoding for mirrors (#21)
- `436a4acc2` render/cm: tone map unassociated HDR color before applying opacity (audit minor)
- `ca5376999` render/cm: apply SDR brightness and saturation before transfer encoding (audit minor)
- `06430181e` render/cm: read the luminance row rather than column for saturation (audit minor)
- `4d075166d` render/cm: resolve auto HDR state before damage gating and drawing (#26)
- `b8e480b47` render/cm: compensate SDR brightness and saturation in FP16 blur (#25)
- `220c5c634` output/cm: fall back to a valid HDR peak when EDID luminance is absent (#20)
- `72c292713` protocols/cm: reject ICC descriptions in parametric preferred requests (audit supplement)
- `fd217bf9a` protocols/cm: retain description identities and omit unknown light-level information (audit minor)
- `9898c53d1` protocols/cm: stop advertising unimplemented debug color-management features (audit minor)
- `133d86105` protocols/cm: return the real ICC file for output image-description information (audit minor)
- `8e1a1f841` tests/cm: exercise failed descriptions, inert resources and creator errors
- `f880aeee3` cm: read matrix coefficients through the installed hyprgraphics API
- `37ada0fd6` tests/cm: compile HDR, extended-linear and blur shader variants
- `6a64e08b8` protocols/cm: validate luminance ranges after PQ overrides and reject singular primaries
- `7ccd50a12` tests/cm: verify per-output reference luminance and ignored PQ maximum
- `2da0f1908` render/cm: document display-light ICtCp and remove its unused source reference argument
- `5c3d8686b` cm: distinguish relative extended-linear descriptions from Windows and internal scRGB (audit minor)
- `dcf165309` protocols/cm: normalize PQ maxima before creator validation (#4)
- `199acbd15` protocols/cm: damage surface contents when its pending color description changes (#3)
- `2919cd1ce` cm: honor reference overrides in forced HDR descriptions without EDID (#16329)
- `0b7ff7a7d` protocols/cm: finish output description changes with wl_output.done (audit minor)
- `d8e79e338` cm: require tone mapping when matching PQ encodings have different display peaks (#4, #27)
- `82ea0604a` output/cm: compare the complete HDR encoding luminance range for scanout (#22)
- `ffb06bc63` tests/cm: preprocess shader variants without requiring a live config manager
- `eaf4889e7` tests/cm: select the named headless output for reference-luminance checks
- `fbff5c653` tests/cm: dispatch protocol regressions on an isolated Wayland server
- `b0e6c8a9a` tests/cm: allow reference checks on a named nested Wayland output
- `8d520e4e5` tests/cm: isolate presentation event listeners between protocol fixtures
- `f5afb8463` tests/cm: check committed color state after description and extension destruction
- `75872febd` tests/cm: validate creator errors after the client destroys its creator proxy
