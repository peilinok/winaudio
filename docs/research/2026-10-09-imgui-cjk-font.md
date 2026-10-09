# ImGui CJK font for Chinese GUI copy (2026-10-09)

Research for [#134](https://github.com/peilinok/winaudio/issues/134). Parent map: [#132](https://github.com/peilinok/winaudio/issues/132).

This note records facts from Dear ImGui 1.90.9 docs and the vendored tree, font license texts, and `src/gui` as it stands. It does **not** pick a product font and does not implement i18n.

Vendored Dear ImGui is **1.90.9** (`IMGUI_VERSION` / `IMGUI_VERSION_NUM 19090` in `third_party/imgui/imgui.h`). Glyph ranges are still required. The 1.92 dynamic atlas (`ImGuiBackendFlags_HasTextures`, optional ranges) is not in this tree.

The vendored copy has no `docs/` and no `misc/fonts/`. Official FONTS.md / FAQ cited below are the matching **v1.90.9** files from `ocornut/imgui`.

## Facts for the spec

1. **Today:** `WinAudioGui` does not use the embedded ProggyClean default as the live font. After DX11/Win32 backend init, `src/gui/main.cpp` loads `C:\Windows\Fonts\msyh.ttc` at 18 px with `GetGlyphRangesChineseSimplifiedCommon()`. That bakes Latin + ~2500 common Simplified-Chinese ideographs into the atlas. There is no `MergeMode`. The return value is not checked.
2. **ImGui 1.90.9 requirement:** a TTF/OTF that actually contains the needed CJK outlines, an explicit persistent glyph-range pointer, atlas build (`GetTexDataAsRGBA32` / `Build`), then the DX11 backend uploading that atlas. Strings must be UTF-8. Merge is optional (ASCII font + CJK font, or icons), not required if one face already covers ASCII + CJK.
3. **Redistribution:** Microsoft YaHei is a Windows-supplied font. An app may *use* the installed file; it may **not** copy, sidecar, or embed the font file in the app. SIL OFL 1.1 fonts named by ImGui (Noto Sans CJK) and the Adobe twin (Source Han Sans) **may** be bundled, embedded, or sold *with* software if the OFL copyright notice and license travel with them. ProggyClean is MIT and already embedded; it has no CJK.
4. **Load path:** keep `AddFont*` in `src/gui` after `ImGui_ImplDX11_Init` and before the first `ImGui_ImplDX11_NewFrame()`. That first `NewFrame` builds the atlas and creates the DX11 font texture. No extra user-installed runtime: static CRT `/MT`, imgui is a static lib, stb_truetype is compiled in, FreeType is not enabled. A shipped OFL file does not need a font installer. The current YaHei path does need `C:\Windows\Fonts\msyh.ttc` to exist.

## 1. What WinAudio GUI loads today

`src/gui/main.cpp` is the ImGui Win32+DX11 example adapted for WinAudio. Font load is the only WinAudio-specific change in that block (commit `d0349e0`, *fix(gui): load Microsoft YaHei CJK font so Chinese device names render*):

```87:90:src/gui/main.cpp
    // WinAudio: load Microsoft YaHei with Simplified-Chinese glyph ranges so
    // Chinese device names render instead of '???'.
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f, nullptr,
                                 io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
```

Sequence around it:

- `ImGui::CreateContext()` then `ImGui_ImplWin32_Init` / `ImGui_ImplDX11_Init`.
- Then `AddFontFromFileTTF` (above). Commented example lines still show `AddFontDefault()` and `GetGlyphRangesJapanese()`; they are not executed.
- First `ImGui_ImplDX11_NewFrame()` in the loop creates device objects and the font texture.

`ImFontAtlas::Build()` loads embedded ProggyClean **only when no font was queued**:

```2677:2683:third_party/imgui/imgui_draw.cpp
bool    ImFontAtlas::Build()
{
    IM_ASSERT(!Locked && "Cannot modify a locked ImFontAtlas between NewFrame() and EndFrame/Render()!");

    // Default font is none are specified
    if (ConfigData.Size == 0)
        AddFontDefault();
```

A successful YaHei `AddFontFromFileTTF` therefore **replaces** ProggyClean; it does not merge with it. `ImFontConfig::MergeMode` stays default `false`. `ImFontConfig::FontNo` stays `0`, so stb_truetype takes the first face in the TTC (`stbtt_GetFontOffsetForIndex(..., cfg.FontNo)` in `imgui_draw.cpp`). On this machine `C:\Windows\Fonts\msyh.ttc` is 19 704 352 bytes (Microsoft YaHei / YaHei UI Regular is index 0 of that collection).

Windows lists that file as a system font (`Msyh.ttc`, family Microsoft YaHei / Microsoft YaHei UI) on the [Windows 10 font list](https://learn.microsoft.com/en-us/typography/fonts/windows_10_font_list).

### Does the atlas already contain CJK?

**Yes, if that `AddFontFromFileTTF` succeeds.** The fourth argument is `GetGlyphRangesChineseSimplifiedCommon()`, which ImGui documents as “Default + Half-Width + Japanese Hiragana/Katakana + set of 2500 CJK Unified Ideographs for common simplified Chinese” (`imgui.h`) and implements as:

- Base: U+0020–00FF, U+2000–206F, U+3000–30FF, U+31F0–31FF, U+FF00–FFEF, U+FFFD.
- Plus 2500 ideographs packed as accumulative offsets from U+4E00, sourced from the modern-Chinese common-character list (comment: 97.97% of characters used in July 1987).

That is **not** the full CJK block. `GetGlyphRangesChineseFull()` is the ~21 000-ideograph alternative (`0x4E00–0x9FAF`). Characters outside the baked range use `ImFont::FallbackChar` (typically `?` / U+FFFD). The original commit used this range so device names such as `麦克风` would render; the same atlas would cover Simplified-Chinese GUI copy **that stays inside those 2500 ideographs**. Rarer copy, or OS device names outside the list, still fall back.

### If `msyh.ttc` is missing

`AddFontFromFileTTF` does `IM_ASSERT_USER_ERROR(0, "Could not load font file!")` then returns `NULL`. `IM_ASSERT` is `assert()`. `main.cpp` does not check the pointer (the example `IM_ASSERT(font != nullptr)` is commented). In a Release build with `NDEBUG`, the assert is compiled out: `ConfigData` stays empty, the first atlas build adds ProggyClean, and CJK becomes `?` again. Debug hits the assert.

ProggyClean itself is Latin-only (default range U+0020–00FF), MIT, embedded in `imgui_draw.cpp` as a compressed Base85 blob (~13 px, pixel font).

## 2. What ImGui 1.90.9 requires for Simplified-Chinese GUI copy

Official FAQ, *How can I display and input non-Latin characters such as Chinese, Japanese, Korean, Cyrillic?* ([docs/FAQ.md @ v1.90.9](https://github.com/ocornut/imgui/blob/v1.90.9/docs/FAQ.md)):

- Pass custom Unicode ranges when loading the font.
- All strings must be UTF-8 (`u8"..."` and/or the compiler UTF-8 mode). A local code page is not accepted.
- Text input is `io.AddInputCharacter()`; the Win32 example already does this. IME positioning uses `PlatformHandleRaw`.

Official FONTS.md @ v1.90.9 (same constraints, with the Chinese helper named explicitly):

```cpp
// Default + Selection of 2500 Ideographs used by Simplified Chinese
io.Fonts->AddFontFromFileTTF("font.ttf", size_pixels, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
```

The Japanese sample in that file uses **Noto Sans CJK**:

```cpp
io.Fonts->AddFontFromFileTTF("NotoSansCJKjp-Medium.otf", 20.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
```

`imgui_demo.cpp` says CJK appears only if the font was loaded with the appropriate ranges, and suggests “Google Noto” or “Arial Unicode”.

### Glyph ranges

| Helper | What it bakes |
| --- | --- |
| `GetGlyphRangesDefault()` | Basic Latin + Latin Supplement |
| `GetGlyphRangesChineseSimplifiedCommon()` | Default + punctuation + kana/halfwidth + **2500** common Simplified ideographs |
| `GetGlyphRangesChineseFull()` | Default + punctuation + kana/halfwidth + **~21000** CJK Unified Ideographs |

Custom ranges: `ImFontGlyphRangesBuilder` (`AddText` / `AddChar` / `AddRanges` then `BuildRanges`). FONTS.md’s troubleshooting section says reducing ranges from localization data is the biggest atlas-size win. For **known GUI copy** that builder can feed every Chinese `AppUiText` string. OS device names are not a closed set; they still need a general CJK range (common or full) if those names must render.

The range **pointer is not copied**. `imgui.h`: the array must persist until `GetTexData*` / `Build()`. `GetGlyphRangesChineseSimplifiedCommon()` returns a function-local `static` buffer, so it is persistent. A builder `ImVector<ImWchar>` must stay alive until `Build()` returns.

### Atlas

1.90.9 packs every requested glyph into **one** texture ahead of time. `GetTexDataAsAlpha8()`, `GetTexDataAsRGBA32()`, or `Build()` rasterizes. The DX11 backend calls `GetTexDataAsRGBA32`. A range that is too large can fail the GPU upload; FONTS.md describes the failure mode as empty black/white rectangles, and suggests `OversampleH = 1`, `ImFontAtlasFlags_NoPowerOfTwoHeight`, or `TexDesiredWidth`.

Default oversampling is `OversampleH = 2`, `OversampleV = 1` (`ImFontConfig`). The current YaHei call passes `font_cfg = nullptr`, so those defaults apply.

OTF is supported despite the `*TTF` names (`imgui.h` ImFontAtlas comment).

### Font merging

`ImFontConfig::MergeMode`: merge into the previous `ImFont` (e.g. ASCII font + CJK glyphs, or icons). Not used today. Needed only if the spec wants ProggyClean (or another Latin face) for ASCII and a second face for Han. FONTS.md’s pre-1.92 recipe:

```cpp
ImFont* font = io.Fonts->AddFontDefault();
ImFontConfig config;
config.MergeMode = true;
io.Fonts->AddFontFromFileTTF("DroidSans.ttf", 18.0f, &config, io.Fonts->GetGlyphRangesJapanese());
io.Fonts->Build();
```

A CJK face that already includes ASCII can be the sole font, which is the current YaHei pattern.

### UTF-8 in this repo

`wa_set_project_warnings` already passes `/utf-8` (`cmake/CompilerWarnings.cmake`). That is the MSVC flag FONTS.md names for UTF-8 source and execution charset. `AppUiText.h` is still ASCII English; Chinese literals would need to stay UTF-8 (this note does not add them).

## 3. Candidate licenses (static redistribution)

“Static redistribution” here means shipping a font file next to `WinAudioGui.exe` or compiling it into the binary / a Windows resource. Runtime *use* of a font already installed on Windows is a different question.

### ProggyClean (embedded default)

- Source: `third_party/imgui/imgui_draw.cpp` (“Copyright (c) 2004, 2005 Tristan Grimmer”, MIT; FONTS.md Credits: MIT).
- CJK: no (default glyph range only).
- Already in the imgui static lib. Not a Chinese-copy solution by itself.

### Microsoft YaHei (`msyh.ttc`) — current path

Primary source: [Font redistribution FAQ for Windows](https://learn.microsoft.com/en-us/typography/fonts/font-faq).

- “Any application installed on your Windows computer has access to these fonts” (screen rendering).
- “Apart from the document embedding rights described previously, you may not redistribute the Windows fonts. You may not copy them to other computers or servers, and you may not convert them to other formats, including bitmap formats, or modify them.”
- “Can I embed the fonts into a game, application or device I’m developing based on the document font embedding permissions? **No**, document font embedding permissions relate to embedding fonts in documents only, not embedding fonts in games, apps and devices.”
- Converting to a bitmap font to put in an app is also **no**.

So: **runtime load of the installed `C:\Windows\Fonts\msyh.ttc` is using a system font. Copying `msyh.ttc` into the repo, the release zip, a sidecar, or a compiled resource is not allowed.** OpenType `fsType` “editable embedding” does not license app embedding.

ImGui FONTS.md also mentions “Arial Unicode or other Unicode fonts provided with Windows for full characters coverage (**not sure of their licensing**)”. The Microsoft FAQ above is the licensing text for Windows-supplied fonts; they are not a redistributable fallback.

### Noto Sans CJK (ImGui’s named CJK sample / Font Links)

- ImGui FONTS.md example file: `NotoSansCJKjp-Medium.otf`. Font Links: “Google Noto Fonts (worldwide languages)”.
- Project: [googlefonts/noto-cjk](https://github.com/googlefonts/noto-cjk). `Sans/LICENSE` is **SIL Open Font License 1.1**.
- Language-specific static OTFs include Simplified Chinese as `NotoSansCJKsc` ([Sans/README.md](https://github.com/googlefonts/noto-cjk/blob/main/Sans/README.md): zip `08_NotoSansCJKsc.zip`). Region subset OTFs are `NotoSansSC`. Variable and Super-OTC/TTC packs also exist; a TTC still needs `ImFontConfig::FontNo`.
- OFL 1.1 permission (same text in Noto `Sans/LICENSE` and Adobe Source Han `LICENSE.txt`): the fonts “can be bundled, embedded, redistributed and/or sold with any software provided that any reserved names are not used by derivative works.” Conditions: do not sell the font *by itself*; each copy must contain the copyright notice and this license (stand-alone text files, headers, or viewable metadata); derivatives keep OFL; reserved names only with permission.

**Sidecar next to the exe, or a compiled resource / compressed C array, is allowed** if the OFL notice and license are shipped with that copy.

### Source Han Sans (Adobe; same OFL family as Noto CJK)

- [adobe-fonts/source-han-sans](https://github.com/adobe-fonts/source-han-sans) README: OpenType/CFF fonts “covered under the terms of the SIL Open Font License, Version 1.1”.
- `LICENSE.txt`: copyright Adobe, Reserved Font Name `'Source'`. Same OFL 1.1 body as Noto.
- Language-specific Simplified Chinese resources are named `SourceHanSansSC-*` (OTF and TTF, static and variable).

Same redistribution conclusion as Noto Sans CJK. This note does not choose between Noto branding and Source branding.

### Other fonts in ImGui `misc/fonts/` (not vendored here)

FONTS.md lists Roboto (Apache 2.0), Cousine (OFL 1.1), DroidSans (Apache 2.0), Karla (OFL 1.1). They are Latin convenience files, not CJK. This repo does not vendor `misc/fonts/`.

## 4. How a load path attaches to the existing DX11 backend

No new renderer is required. The stock backend already uploads whatever atlas `io.Fonts` holds.

```595:601:third_party/imgui/backends/imgui_impl_dx11.cpp
void ImGui_ImplDX11_NewFrame()
{
    ImGui_ImplDX11_Data* bd = ImGui_ImplDX11_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplDX11_Init()?");

    if (!bd->pFontSampler)
        ImGui_ImplDX11_CreateDeviceObjects();
}
```

`ImGui_ImplDX11_CreateDeviceObjects()` ends with `ImGui_ImplDX11_CreateFontsTexture()`:

- `io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height)` (this `Build()`s if needed).
- `CreateTexture2D` + shader-resource view, `DXGI_FORMAT_R8G8B8A8_UNORM`.
- `io.Fonts->SetTexID((ImTextureID)bd->pFontTextureView)`.
- Linear sampler (bilinear required unless `ImFontAtlasFlags_NoBakedLines`).

So the attach point is **the existing `AddFont*` call in `src/gui/main.cpp`**, after `ImGui_ImplDX11_Init` and **before** the first `NewFrame`. Adding or replacing fonts after that first `NewFrame` requires rebuilding the atlas and recreating DX11 font objects (`InvalidateDeviceObjects` / `CreateDeviceObjects`). FONTS.md: do add/remove fonts before `NewFrame()`.

### Three mechanical load styles (ImGui API)

All three already exist in 1.90.9 (`imgui.h` / FONTS.md). None of them lives in `src/core`.

| Style | API | Fits this repo |
| --- | --- | --- |
| File path | `AddFontFromFileTTF` | Current YaHei (absolute Windows path). A shipped OFL file should not use CWD; FONTS.md warns relative paths follow the process working directory. Existing GUI sidecars resolve the exe directory via `GetModuleFileNameW` (`AppUi::exeDirectory`, `loadProductionPhrases` → `channel-idents\`). |
| Memory buffer | `AddFontFromMemoryTTF` | Windows resource (`RCDATA`) or a `std::vector` read from the sidecar. Default: atlas **takes ownership** and `free`s the buffer; set `FontDataOwnedByAtlas = false` to keep it. |
| Embedded C array | `AddFontFromMemoryCompressedTTF` / `Base85` | FONTS.md: `misc/fonts/binary_to_compressed_c.cpp` (that tool is **not** in the vendored tree). Same mechanism ProggyClean already uses. |

`src/gui/CMakeLists.txt` already copies `assets/channel-idents` next to the exe on `POST_BUILD`. A font sidecar would be the same kind of copy, from `third_party/` (constraint below) not from `src/core`.

FreeType (`#define IMGUI_ENABLE_FREETYPE` in `imconfig.h`) is commented out. Rasterizer is embedded stb_truetype. Enabling FreeType would mean vendoring FreeType under `third_party/` and compiling `misc/freetype/imgui_freetype.cpp`; that file is not in the vendored imgui snapshot. It is optional quality, not required for CJK coverage.

## 5. Runtime, CRT, `third_party/`, not core

- **Static CRT:** top-level `CMakeLists.txt` sets `CMAKE_MSVC_RUNTIME_LIBRARY` to `MultiThreaded$<$<CONFIG:Debug>:Debug>` (`/MT`, `/MTd`) so “distributed exes are self-contained (no VC++ Redistributable needed).” `third_party/CMakeLists.txt` notes imgui/gtest inherit that. A font file does not change the CRT.
- **No extra user-installed runtime** for the renderer: DX11 is OS, imgui is `add_library(imgui STATIC ...)`, stb_truetype is compiled into that lib. No font-hosting service, no DirectX redistributable beyond what Windows already provides for D3D11.
- **YaHei path:** the user does not install a *library*, but the process **does** require the Windows font file `C:\Windows\Fonts\msyh.ttc`. Missing file → Debug assert / Release ProggyClean fallback (see §1).
- **OFL sidecar or resource:** the user does not install a font. The app must ship the OFL text with the font files.
- **Where code and files go:** CLAUDE.md / this ticket: GUI-only third-party stays in `third_party/`, not in `src/core`. Font I/O belongs in `src/gui` (today: `main.cpp`). Core remains Win32 + STL + spdlog.

## Non-decisions (explicit)

- Which face WinAudio should ship or keep loading (YaHei vs Noto Sans CJK vs Source Han Sans vs merge-with-ProggyClean).
- Common 2500 vs Full vs `ImFontGlyphRangesBuilder` over `AppUiText`.
- Sidecar vs compiled resource vs continue using the Windows font.
- Atlas pixel size, oversampling, FreeType.
- Chinese string contents (parent map / later spec).

Those belong in the language spec, not this ticket.

## Sources

- Vendored Dear ImGui 1.90.9: `third_party/imgui/imgui.h`, `imgui_draw.cpp`, `imgui_demo.cpp`, `imgui.cpp`, `imconfig.h`, `backends/imgui_impl_dx11.cpp`, `examples/example_win32_directx11/main.cpp`, `LICENSE.txt`.
- Official docs for that version: [FONTS.md @ v1.90.9](https://github.com/ocornut/imgui/blob/v1.90.9/docs/FONTS.md), [FAQ.md @ v1.90.9](https://github.com/ocornut/imgui/blob/v1.90.9/docs/FAQ.md) (Fonts, Text: non-Latin / Chinese).
- WinAudio load path: `src/gui/main.cpp`, commit `d0349e0`; GUI exe-dir sidecar pattern: `src/gui/AppUi.cpp` (`exeDirectory`, `loadProductionPhrases`); `src/gui/CMakeLists.txt` `POST_BUILD` copy; `/utf-8`: `cmake/CompilerWarnings.cmake`; `/MT`: root `CMakeLists.txt`; imgui static target: `third_party/CMakeLists.txt`.
- Microsoft: [Font redistribution FAQ](https://learn.microsoft.com/en-us/typography/fonts/font-faq), [Windows 10 font list](https://learn.microsoft.com/en-us/typography/fonts/windows_10_font_list) (Microsoft YaHei → `Msyh.ttc`).
- Noto Sans CJK: [Sans/LICENSE (OFL 1.1)](https://github.com/googlefonts/noto-cjk/blob/main/Sans/LICENSE), [Sans/README.md (download formats, `NotoSansCJKsc` / `NotoSansSC`)](https://github.com/googlefonts/noto-cjk/blob/main/Sans/README.md).
- Source Han Sans: [LICENSE.txt (OFL 1.1, Reserved Font Name Source)](https://github.com/adobe-fonts/source-han-sans/blob/release/LICENSE.txt), [README.md](https://github.com/adobe-fonts/source-han-sans/blob/release/README.md).
