# What catalog shapes fit a C++ ImGui two-language GUI?

Date: 2026-10-09.
Ticket: [peilinok/winaudio#135](https://github.com/peilinok/winaudio/issues/135).
Parent map: [GUI copy and repo guide language spec](https://github.com/peilinok/winaudio/issues/132).

This note compares catalog shapes that can hold **exactly two languages** (English original + Chinese) for WinAudio GUI copy. It does not pick a shape. Picking is [issue 139](https://github.com/peilinok/winaudio/issues/139), which is blocked by this ticket and the CJK font ticket.

Sources are this repo (`AppUiText.h`, copy tests, CMake, vendored Dear ImGui 1.90.9), Dear ImGui docs shipped with that tree, GNU gettext / glibc gettext pages, nlohmann JSON docs (as a typical C++ JSON library; the tree has none), and Microsoft Learn Win32 STRINGTABLE / LoadString / MUI pages.

## Short answer

All four shapes can express two languages, a missing-Chinese → English rule, and a gtest that the key sets match. They differ in **who owns a new sentence**, **whether lookup allocates on the GUI thread**, and **how close they sit to today's `constexpr const char*` + `EXPECT_STREQ` tests**.

| Shape | Who adds a sentence | Key-parity test | GUI lookup heap? | English fallback | Fit with today's gtests |
| --- | --- | --- | --- | --- | --- |
| Compile-time parallel tables | Programmer adds an English `kXxx` (and a Chinese slot, possibly empty) in headers | `static_assert` on counts; gtest walks both tables | No, if lookup returns `const char*` into `.rdata` | Empty/null Chinese pointer → English pointer | Direct: keep `EXPECT_STREQ(kXxx, "English")`; add Chinese / fallback cases |
| gettext / libintl | Programmer marks English `msgid`; translator fills `zh_CN.po` `msgstr` | `msgcmp def.po ref.pot`; gtest can parse `.po` or call `gettext` | No after `.mo` load: `gettext` returns a pointer into the catalog (or the `msgid`) | Missing/untranslated `msgstr` → `msgid` (English) | English tests still pin `msgid`; Chinese needs a loaded catalog or a `.po` parser |
| JSON / sidecar | Programmer adds a key in the English file (or header); translator edits the Chinese sidecar | gtest loads both files and compares key sets | Parse/load allocates once; **per-frame `json::value` copies `std::string`**. Intern to `const char*` at load if frames must not allocate | Missing or empty Chinese key → English | Tests can read the files without ImGui; English literals in `AppUiText.h` can stay the fallback table |
| Windows resources / MUI | Programmer adds `IDS_*` + English `STRINGTABLE`; translator fills a Chinese table or `.mui` | Compare ID lists in `.rc` / `resource.h`; runtime `LoadString` ≠ 0 | `LoadStringW` copies UTF-16 into a buffer; UTF-8 for ImGui needs `WideCharToMultiByte` (or a one-time cache) | Missing ID returns 0 → app English. OS MUI fallback is **file/language**, not per-string | Harder: tests must compile resources or parse `.rc`. Numeric IDs are not today's `kXxx` pointers |

Audio threads must not touch the catalog. GUI-thread allocation is allowed. No third language.

## Constraints already in this repo

### Catalog surface today

[`src/gui/AppUiText.h`](../../src/gui/AppUiText.h) is a C++17 header in `namespace wa::ui_text`. Almost every user-visible GUI sentence that has been centralized lives there as `inline constexpr const char*` pointing at a string literal (about 85 named pointers, plus `kAdvancedStreamOptions[]` of length 5). Two helpers are not literals:

- `channelPhraseText(ChannelPhrase)` — a `switch` returning English literals (or `kRenderNoPhrase`).
- `formatLiveSessionTooltip` — builds a `std::string` (heap) from catalog words plus process/device/volume/state.
- `pipelineCallLogEmptyText` — picks among three already-catalogued pointers.

Call sites in `AppUi.cpp` pass those `const char*` straight into Dear ImGui (`Button`, `BeginTabItem`, `TextUnformatted`, `Combo` item lists, and so on). Some window sentences are still literals in `AppUi.cpp`; inventory of those is a different ticket.

### Copy tests today

gtest pins the **English original** with `EXPECT_STREQ` against the same literals:

- [`src/tests/test_audio_session_enumerator.cpp`](../../src/tests/test_audio_session_enumerator.cpp) — Application Loopback, advanced options, OS-sound, log chrome, charts, loopback buttons.
- [`src/tests/test_live_session_list.cpp`](../../src/tests/test_live_session_list.cpp) — `PipelineUiText` and tooltip fragments.
- [`src/tests/test_render_track_list.cpp`](../../src/tests/test_render_track_list.cpp) — `RenderStrings` plus `channelPhraseText`.

`WinAudioTests` includes `src/gui` headers and does **not** link `WinAudioGui` ([`src/tests/CMakeLists.txt`](../../src/tests/CMakeLists.txt)). A catalog that is only reachable through ImGui, a `.mo` next to the GUI exe, or a PE resource compiled only into `WinAudioGui` needs an extra test seam.

### Threads, encoding, dependencies

- Audio hot path: no heap, no blocking lock ([`CLAUDE.md`](../../CLAUDE.md)). Catalog lookup belongs on the GUI thread only.
- GUI thread may allocate (`formatLiveSessionTooltip` already does).
- Core never prints; core / spdlog runtime logs stay English (map #132). Log-panel chrome and app-authored status lines are GUI copy.
- Own targets compile `/utf-8` ([`cmake/CompilerWarnings.cmake`](../../cmake/CompilerWarnings.cmake)). Top-level CMake sets `UNICODE` / `_UNICODE` ([`CMakeLists.txt`](../../CMakeLists.txt)).
- Third-party GUI libraries go in `third_party/`; core stays Win32 + STL + spdlog. There is **no** JSON parser and **no** libintl in the tree. spdlog is the only git submodule.
- Portable sidecar precedent: `winaudio.ui.ini` next to the exe ([`src/gui/LogRegionPrefs.h`](../../src/gui/LogRegionPrefs.h)), not `%AppData%`, not ImGui `imgui.ini`. Channel-ident assets are copied next to the exe in a `POST_BUILD` step ([`src/gui/CMakeLists.txt`](../../src/gui/CMakeLists.txt)).

Map #132 already locks: English original is the existing `AppUiText.h` sentences; Chinese is the second language; missing Chinese shows the English original; in-window language switch (storage is another ticket); domain words stay English.

## Dear ImGui facts that apply to every shape

Vendored tree is Dear ImGui **1.90.9** (`IMGUI_VERSION_NUM 19090` in [`third_party/imgui/imgui.h`](../../third_party/imgui/imgui.h)). Official docs for that version:

- [FAQ: non-Latin characters](https://github.com/ocornut/imgui/blob/v1.90.9/docs/FAQ.md#q-how-can-i-display-and-input-non-latin-characters-such-as-chinese-japanese-korean-cyrillic): strings passed to ImGui must be **UTF-8**. Local code-page literals will not work. C++11 `u8"..."` or a compiler UTF-8 mode. This repo already passes `/utf-8` to own targets.
- [FONTS.md — About UTF-8 Encoding](https://github.com/ocornut/imgui/blob/v1.90.9/docs/FONTS.md#about-utf-8-encoding): `ImGui::DebugTextEncoding` to verify literals. Glyph ranges are a **font** problem (ticket 134). `GetGlyphRangesChineseSimplifiedCommon` is in `imgui.h`. `src/gui/main.cpp` already loads `msyh.ttc` with that range so CJK *device names* can rasterize; that does not choose a catalog.
- [FAQ: ID stack](https://github.com/ocornut/imgui/blob/v1.90.9/docs/FAQ.md#q-about-the-id-stack-system): widget IDs are hashes of labels. `###id` keeps the ID while the visible label changes. In this vendored `imgui.cpp`:

  ```text
  // IMPORTANT: ###xxx suffixes must be same in ALL languages
  ```

  ImGui's own loc table is a **compile-time parallel table** keyed by `ImGuiLocKey`, registered with `LocalizeRegisterEntries`, read with `LocalizeGetMsg` (pointer from `g.LocalizationTable[key]`, or `"*Missing Text*"`). That is ImGui's pattern for its few built-in strings, not an app catalog API.

- FAQ on C++ strings: ImGui takes `char*`. `std::string` on a busy UI can heap-allocate; literals and interned pointers are the cheap path.

So every shape must hand ImGui **UTF-8 `const char*`**, and labels used as IDs need a stable `###` suffix (or `PushID`) that does **not** change with language.

## 1. Compile-time parallel tables

Second header, dual arrays, X-macro, or enum + two `const char*` tables, all keyed the same. This is the shape ImGui uses for `GLocalizationEntriesEnUS[]`, and the closest extension of `wa::ui_text`.

**Possible layouts (not a pick):**

- `AppUiText.h` stays English; `AppUiText.zh.h` repeats the same identifiers with Chinese literals or `nullptr` / `""`.
- One header, X-macro: `WA_UI_TEXT(kApply, "Apply", "应用")` expanding to two tables.
- `enum class UiText : int` plus `kEn[]` / `kZh[]` of equal length (same idea as `ImGuiLocKey` + `LocalizationTable`).

Lookup on the GUI thread:

```text
const char* zh = kZh[i];
return (zh && zh[0]) ? zh : kEn[i];
```

That is a pointer choose. No heap. Combo arrays (`kAdvancedStreamOptions`) need a parallel array of the same count (`kAdvancedStreamOptionCount` is already `sizeof` / `sizeof`). `channelPhraseText` becomes an index into two tables, or a second `switch`.

**Ownership.** The programmer who adds a control adds the English `kXxx` (today's rule) **and** a Chinese slot. The slot may be empty until a translator fills it. There is no separate runtime file. Changing a sentence requires a rebuild. Chinese in source needs `/utf-8` (already on) or `u8"..."` / `\uXXXX` escapes.

**Key-parity tests.**

- Compile-time: `static_assert` that both tables have the same length; X-macro makes a missing pair a compile error.
- gtest: keep today's `EXPECT_STREQ(kEnApply, "Apply")` as the English original lock. Walk every key: Chinese pointer may be empty; if non-empty, `EXPECT_STREQ` against the intended Chinese sentence. A dedicated test: `lookup(Zh, kMissing)` returns the English pointer.

**GUI-thread lookup / alloc.** None, if the API stays `const char*`. Building tooltips can keep allocating, as `formatLiveSessionTooltip` does now.

**English fallback.** Empty or null Chinese cell → English cell. That is the map rule ("某一句还没有中文时，显示英文原文") with no extra library.

**Alignment with existing gtests.** Strongest. Tests already `#include "AppUiText.h"` and compare pointers to literals. English tests do not need a language runtime. Chinese tests include the second table the same way.

**Two-language limit.** A third table is possible but out of scope; nothing in the shape requires it.

**Cost relative to this repo.** No new third_party. No sidecar packaging. Fits C++17, static CRT, and device-free gtest. The whole catalog is in the binary (small: a few tens of KB of UTF-8).

## 2. gettext / libintl

GNU gettext is a `msgid` → `msgstr` catalog. C/C++ usage from the [GNU gettext manual, C / C++ language table](https://www.gnu.org/software/gettext/manual/html_node/C.html): `#include <libintl.h>`, `setlocale`, `bindtextdomain` / `wbindtextdomain` (Windows), `textdomain`, `_("English sentence")` as `gettext`. Extractor: `xgettext -k_`.

[Interface to gettext](https://www.gnu.org/software/gettext/manual/html_node/Interface-to-gettext.html): `gettext(msgid)` returns the translation if present in the current domain; **if it is not available, the argument itself is returned**. [glibc: charset conversion in gettext](https://www.gnu.org/software/libc/manual/html_node/Charset-conversion-in-gettext.html): the `msgid` is **not** charset-converted; when no translation is found, `msgid` is returned unchanged; **all msgids should be US-ASCII**; `bind_textdomain_codeset(domain, "UTF-8")` makes translations UTF-8 for ImGui.

Catalog files: one PO per language (`msgid` / `msgstr`). [PO format](https://www.gnu.org/software/gettext/manual/html_node/PO-Files.html): untranslated entries have an empty `msgstr`. `msgfmt` compiles PO → binary `.mo`. Layout after `bindtextdomain`: `dirname/locale/LC_MESSAGES/domain.mo` ([gettext(3)](https://man7.org/linux/man-pages/man3/gettext.3.html)). `LANGUAGE` is a colon-separated locale list tried in order, then the `msgid`.

**Ownership.** Programmer writes English in source as `msgid` (can stay the current `AppUiText.h` literals wrapped in `gettext`, or `_("Apply")` at the call site). Translator edits `po/zh_CN.po`. English does not live in the Chinese file except as keys. New sentence: add English in source, run `xgettext` / `msgmerge`, translator fills `msgstr` (may stay empty).

**Key-parity tests.**

- GNU `msgcmp def.po ref.pot` "compares two Uniforum style .po files to check that both contain the same set of msgid strings" ([msgcmp invocation](https://www.gnu.org/software/gettext/manual/html_node/msgcmp-Invocation.html)). That is the official key-parity tool.
- `msgfmt --check` / `--check-format` for printf mismatches (positional `%2$d` is the gettext way to reorder arguments; WinAudio copy is mostly whole sentences plus a few `ImGui::Text("%s: %u", …)` concatenations).
- gtest on Windows CI: `windows-2022` does not ship GNU gettext tools. Either vendor `msgcmp`/`msgfmt`, parse PO in a small C++ test, or link libintl and call `gettext` after loading a test `.mo`.

**GUI-thread lookup / alloc.** Loading and hashing the `.mo` allocates once (startup or language switch). After that, `gettext` returns `char*` into the catalog or the original `msgid`. No per-frame heap if call sites keep using the pointer that frame (do not wrap every lookup in `std::string`). In-window language switch is **not** `LANGUAGE=`; the app would bind another domain path or reload catalogs, then keep using `gettext`. Audio threads must not call it (libintl is not documented as real-time / allocation-free on first miss).

**English fallback.** Built in: missing or untranslated `msgstr` → `msgid`. That matches "缺中文时回退英文" **if and only if** every `msgid` is the English original from `AppUiText.h`. glibc warns msgids should be ASCII; current copy is ASCII English, so that fits.

**Alignment with existing gtests.** English `EXPECT_STREQ(ui_text::kApply, "Apply")` still works if `kApply` remains the `msgid` literal. It does **not** prove that `gettext(kApply)` is Chinese after a switch, unless tests load a catalog. Wrapping every call site in `_()` without keeping `kApply` would break the current tests.

**Two-language limit.** gettext is built for N locales. Using it for exactly two is fine; the extra machinery (plural forms, `LANGUAGE` lists, `LC_MESSAGES` tree) is unused.

**Cost relative to this repo.** MSVC CRT has **no** `libintl.h`. Need GNU gettext-runtime (typically LGPL) in `third_party/` or a static libintl, plus `msgfmt` in the build to ship `.mo` next to the exe (same class of `POST_BUILD` copy as `channel-idents`). `wbindtextdomain` exists because Windows paths are wide. Static `/MT` must match the libintl build. This is a new runtime + tool dependency the other compile-time shape does not need.

## 3. JSON or other sidecar files

A file next to the exe (or two files) mapping keys → sentences, loaded on the GUI thread. JSON is the named option; this repo already has a tiny **INI** sidecar loader for UI prefs, which is the same *packaging* idea with a smaller parser.

**JSON (typical C++ library: nlohmann JSON).** Not in the tree; would be a header-only `third_party/` addition if chosen. Official docs:

- [parse](https://json.nlohmann.me/api/basic_json/parse/): deserializes from `istream` / `string` / iterators; UTF-8 BOM ignored; iterator `value_type` size 1/2/4 → UTF-8/16/32.
- [FAQ: Unicode](https://json.nlohmann.me/home/faq/): **only UTF-8** input (RFC 8259). Stored strings are UTF-8 `std::string`. Latin-1 / CP1252 will fail parse. That matches ImGui and this repo's `/utf-8`.
- [value(key, default)](https://json.nlohmann.me/api/basic_json/value/): Python `dict.get`; missing key returns the default **by copy**. Logarithmic in object size. `contains` / `find` for presence.
- FAQ: `basic_json` is **not** thread-safe for mixed read/write. Concurrent reads of an already-built object are fine. Load on the GUI thread, then treat as immutable.

A sidecar can be:

- `ui-text.zh-CN.json` of `{ "kApply": "应用", … }` with English remaining in `AppUiText.h`, or
- two JSON files, or one file `{ "en": {…}, "zh": {…} }`.

**Other sidecars.** INI in the style of `winaudio.ui.ini` (`kApply=应用`) needs no third-party parser; it is a poor fit for `kAdvancedStreamOptions` (five ordered strings) and multi-line help (`kAdvancedOptionsHelp`, pipeline waiting text) unless values can span lines. CSV is easy to diff and easy to break on commas inside sentences. None of these exist as copy catalogs today.

**Ownership.** Programmer adds the key to the English source of truth (header or `en.json`) and a possibly empty Chinese entry. Translator edits the Chinese file without a C++ rebuild **if** the GUI reloads the sidecar. Shipping still has to copy the file next to `WinAudioGui.exe` (same pattern as `channel-idents`). A missing file at runtime must fall back to compiled English, same as missing `winaudio.ui.ini` falls back to expanded log.

**Key-parity tests.** gtest reads both objects (nlohmann `parse` on a string, or the INI parser) and asserts:

- every English key exists in Chinese (value may be `""`);
- no extra Chinese keys;
- `value(zh, key, en[key])` equals English when Chinese is missing/empty;
- selected sentences still `EXPECT_STREQ` against the locked English originals.

That can run device-free without linking ImGui. It does **not** replace today's header tests unless English moves out of the header.

**GUI-thread lookup / alloc.**

- **Load / parse:** heap, once per language switch. Allowed.
- **Per-frame:** `json::value("kApply", english)` returns `std::string` **by copy** (allocates). `operator[]` on a string node yields a reference into the DOM, but converting to `const char*` via `get<std::string>()` copies. To keep ImGui's cheap `char*` path, intern at load: walk keys, `strdup` or store in `std::vector<std::string>` and keep `.c_str()` for the session. After intern, lookup is a pointer table or `unordered_map` find — still GUI-thread only.
- Do not parse JSON on the audio thread.

**English fallback.** `j.value(key, englishLiteral)` or `contains` then empty-string check. That is application policy; JSON has no i18n fallback of its own.

**Alignment with existing gtests.** Good if English stays in `AppUiText.h` and JSON is only Chinese overlays: existing `EXPECT_STREQ` stays. Add file-based parity tests like `test_log_region_prefs.cpp` (temp files, missing → fallback). If English *moves* into JSON, every current copy test must read the file or a compiled-in default.

**Two-language limit.** A sidecar can grow a third object; the loader can refuse any language other than `en` / `zh`.

**Cost relative to this repo.** JSON: new header-only dependency + UTF-8 file next to the exe + intern step. INI: no new dependency, weaker structure. Either way, CI must ship the sidecar or tests will see "missing file → all English". Users can edit the file; that is optional, not a map requirement.

## 4. Windows resources / MUI

Win32 string resources: numeric IDs in a `STRINGTABLE`, loaded with `LoadString`. Optional MUI split: language-neutral PE + `language-name\file.mui`.

**STRINGTABLE** ([Learn](https://learn.microsoft.com/en-us/windows/win32/menurc/stringtable-resource)): unsigned 16-bit `stringID`; string ≤ 4097 characters; `LANGUAGE language, sublanguage` on the table; Unicode via `L"\x5e2e\x52a9"`. RC packs 16 strings per section.

**LoadStringW** ([Learn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadstringw)): copies into a `wchar_t` buffer, or with `cchBufferMax == 0` returns a **read-only pointer** into the resource (UTF-16, **not** guaranteed NUL-terminated; length is the return value). Return 0 → string does not exist. This repo already compiles `UNICODE`, so `LoadString` is `LoadStringW`.

ImGui wants UTF-8, not UTF-16. Every loaded string needs `WideCharToMultiByte(CP_UTF8, …)` unless a UTF-8 `RCDATA` blob is used instead of `STRINGTABLE`.

**FindResourceExW** ([Learn](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-findresourceexw)): find by type, name, **language**. `MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)` uses the thread UI language. String resources live in 16-string blocks.

**MUI** ([resource management](https://learn.microsoft.com/en-us/windows/win32/intl/mui-resource-management), [Hello MUI tutorial](https://learn.microsoft.com/en-us/windows/win32/intl/creating-a-multilingual-user-interface-application)):

- LN file = code + language-neutral resources; `*.mui` per language in a folder named for the language (`en-US\WinAudioGui.exe.mui`).
- Checksums bind LN and `.mui`; loader will not load a mismatched file.
- `LoadLibraryEx(..., LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE)` + `LoadStringW` is the Vista+ recipe. `LoadMUILibraryW` ([muiload.h](https://learn.microsoft.com/en-us/windows/win32/api/muiload/nf-muiload-loadmuilibraryw)) is the down-level helper.
- `GetUserDefaultUILanguage` ([Learn](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getuserdefaultuilanguage)) is the OS display-language ID the map wants at first launch. `SetThreadPreferredUILanguages` / `SetThreadUILanguage` change what the resource loader prefers for the thread.

OS fallback is a **language list** (user UI → system UI → LN file), not "this one ID is missing, use English copy of that ID". If the Chinese `.mui` loads but `IDS_APPLY` is absent, `LoadString` returns 0 and the **application** must substitute English (from the LN table or from `AppUiText.h`).

**Ownership.** Programmer adds `IDS_APPLY` to `resource.h` and the English `STRINGTABLE`. Translator edits the Chinese `STRINGTABLE` (same `.rc` with a second `LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED` block, or a satellite `.mui`). Tools: Windows `rc.exe` (already on the MSVC box / `windows-2022`); MUI split uses `muirct.exe` / RC MUI switches. IDs are numbers; names exist only in the header.

**Key-parity tests.**

- Parse `resource.h` + both `STRINGTABLE`s: every `IDS_*` present in both languages (Chinese may be empty — but an empty STRINGTABLE entry loads as empty, which is **not** "missing", so fallback must treat `""` as missing too).
- Runtime: `LoadStringW` for each ID in English and Chinese modules; 0 means missing.
- Today's `EXPECT_STREQ(kApply, "Apply")` does not talk to the resource compiler unless tests also compile the `.rc` into `WinAudioTests` or keep `AppUiText.h` as the English duplicate.

**GUI-thread lookup / alloc.**

- `LoadStringW` into a stack `wchar_t[n]` + `WideCharToMultiByte` into a stack UTF-8 buffer: no heap, but a **copy every call**. Fine on GUI thread; wasteful every frame.
- `cchBufferMax == 0` gives a UTF-16 pointer into the PE; still not ImGui UTF-8.
- Practical: on language switch, load all IDs once into interned UTF-8 `const char*` (heap once). Then lookup matches the compile-time table.

Do not `LoadString` on the WASAPI thread.

**English fallback.** Application policy on `LoadString == 0` or empty. MUI file fallback will serve a whole Chinese table if `zh-CN\*.mui` exists, even when some IDs are blank. Per-sentence English fallback is **not** free; it must be coded. Two `STRINGTABLE`s in one exe (no MUI split) plus `FindResourceEx` with `LANG_CHINESE` / `LANG_ENGLISH` can implement per-ID fallback without satellite DLLs.

**Alignment with existing gtests.** Weakest native fit. Keys are `UINT`, not `kApply`. Tests that must not depend on a window can still compile a `.rc` into `WinAudioTests` (add a resource to that target) or parse text. Duplicating English in both `AppUiText.h` and `.rc` means two sources unless the header is generated from the `.rc` or vice versa.

**Two-language limit.** Win32 resources are designed for many `LANGUAGE` blocks / many `.mui` folders. Using only `en-US` and `zh-CN` is a subset.

**Cost relative to this repo.** No extra third_party. Uses the SDK the MSVC build already has. Adds `.rc` to CMake (`target_sources(... foo.rc)`), UTF-16 ↔ UTF-8, and either MUI folder layout in the zip or two tables in one PE. Release packaging (`release.yml` zips build output) would need to include `zh-CN\WinAudioGui.exe.mui` if split. Numeric IDs fight the current named-pointer style.

## How each lines up with the current `EXPECT_STREQ` tests

The existing tests lock **wording**, not a lookup API:

```cpp
EXPECT_STREQ(wa::ui_text::kApply, "Apply");
EXPECT_STREQ(wa::ui_text::channelPhraseText(ChannelPhrase::Channel1), "channel 1");
```

Whatever shape is chosen later should keep that English lock (map: English original is the current `AppUiText.h` sentences) and add:

1. **Parity:** every English key has a Chinese cell (cell may be empty).
2. **Fallback:** empty Chinese cell displays the English original (`EXPECT_STREQ(lookup(Zh, kNew), "Apply")` while Chinese is still empty).
3. **Filled Chinese:** once a sentence exists, `EXPECT_STREQ(lookup(Zh, kApply), "<chinese>")` — the exact Chinese wording is a spec/translator job, not this ticket.
4. **Helpers:** `channelPhraseText`, `pipelineCallLogEmptyText`, `formatLiveSessionTooltip` must go through the same lookup so Pipeline / Render tests keep meaning.

`formatLiveSessionTooltip` already heap-allocates on the caller (GUI) thread; language lookup inside it does not change the audio-thread rule.

## Cross-cutting: IDs, arrays, and what is not a catalog

- ImGui `###` suffixes and `PushID` must be language-stable. A catalog entry that is both label and ID should store visible text separately from the ID tail, or the English `###id` must be appended after lookup.
- Ordered lists (`kAdvancedStreamOptions`, table column titles) need **the same length** in both languages, not only the same key set.
- Device names, PIDs, HRESULT text, and spdlog lines are not GUI copy (map #132). A catalog that wraps `_()` around `wa::log` would violate that lock.
- Channel-ident **speech** WAV filenames stay English; `channelPhraseText` is on-screen GUI copy and is in scope for a catalog.
- `+` / `-` log chrome (`kLogExpand` / `kLogCollapse`) are symbols; they can occupy keys without translation.

## What this note does not decide

- Which of the four shapes the spec locks (issue 139).
- Exact C++ types (`enum` vs `kXxx` names vs `gettext` macros vs `IDS_*`).
- Whether English stays in `AppUiText.h` forever or is generated.
- Font / atlas / YaHei vs a bundled face (issue 134).
- Where the language override is stored (issue 137) or where the switch control sits (issue 136).
- Which currently hardcoded `AppUi.cpp` literals must enter the catalog (issue 133).
- A third language, CLI copy, or core log translation.
)
