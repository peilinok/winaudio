# GUI copy and repo guide language

**Status:** ready for implementation  
**Tracker:** [#146](https://github.com/peilinok/winaudio/issues/146)  
**Glossary:** `CONTEXT.md` (GUI copy, Repo guide)  
**Map:** [#132](https://github.com/peilinok/winaudio/issues/132)  
**ADRs:** none apply. Stream-init and Pipeline-intercept decisions stay as they are.

This document is the spec for two surfaces only: **GUI copy** (界面文案) and **Repo guide** (仓库说明). It records decisions already closed on the map. It does not reopen them.

## 1. 范围

### Problem Statement

WinAudio's window and its GitHub landing page are each in one language, and not the same one.

A person who runs the tool on a Chinese Windows desktop still reads an English window: tabs, Track controls, the log panel's own chrome, and the status lines the app itself writes. A person who reads Chinese can switch nothing in the window, and a choice cannot be remembered. Sentences that already have no Chinese wording have nowhere to fall back except by staying English forever, with no rule.

A visitor who opens the GitHub repo lands on a README that is entirely Chinese, while release notes stay English. There is no English landing page, no Chinese twin, and the screenshot page mixes Chinese captions with one set of images. The pictures do not show the window in the language of the page.

### Solution

The window speaks English or Chinese. English is the source language. Chinese is the second language. On first launch the language follows the Windows display language: a Chinese display language uses Chinese GUI copy, and any other display language uses English. A combo at the trailing end of the page tab bar switches the language for the whole window, immediately, and that choice is remembered in the existing exe-sibling `winaudio.ui.ini`. A sentence that has no Chinese wording shows its English sentence.

The GitHub landing page is an English `README.md`. The Chinese guide is `README.zh-CN.md`. Each links to the other and to its own screenshot page. Each page shows Monitor, Loopback, and Application Loopback in that page's language.

### User Stories

1. As an audio tester whose Windows display language is Chinese, I want the first launch to show Chinese GUI copy, so that I can read the window without changing a setting.
2. As an audio tester whose Windows display language is not Chinese, I want the first launch to show English GUI copy, so that the window matches the desktop I already use.
3. As an audio tester whose display language is Traditional Chinese or Hong Kong Chinese, I want the same simplified Chinese GUI copy, so that I still get the one Chinese catalog this tool ships.
4. As an audio tester whose regional format is Chinese while the Windows display language is English, I want English GUI copy, so that the window follows the display language rather than the number format.
5. As an audio tester, I want the first frame to already be in the resolved language, so that the window does not flash English and then jump.
6. As an audio tester, I want the language combo at the trailing end of the page tab bar, so that I can find it without hunting through panels.
7. As an audio tester, I want that combo on every page, so that I can switch from Monitor, Loopback, Application Loopback, Render, or Pipeline.
8. As an audio tester, I want the combo items to be English and 中文, in that order, so that I can recognize my language by name.
9. As an audio tester, I want no separate Language caption beside the combo, so that the tab bar stays a single row of pages plus the switch.
10. As an audio tester, I want the switch to stay out of the left panel, the log toolbar, and Options, so that those areas keep their current jobs.
11. As an audio tester, I want the combo's own labels to stay English and 中文 in both UI languages, so that I can always switch back.
12. As an audio tester, I want choosing 中文 to replace the window's sentences immediately, so that I do not restart the tool to read Chinese.
13. As an audio tester, I want choosing English to restore English sentences immediately, so that I can switch back the same way.
14. As an audio tester, I want a language change to leave Tracks, Monitor, and Pipeline running, so that I can translate the window during a measurement.
15. As an audio tester, I want a language change to keep Chart freeze, Channel pause, play-once, and volume as they are, so that the switch changes words and not the session.
16. As an audio tester, I want an open panel to stay open across the switch, so that the control I am using does not reset because its label changed.
17. As an audio tester, I want the choice I make in the combo to be the language on the next launch, so that I do not set it again every time.
18. As an audio tester, I want that remembered choice to win over a later change to the Windows display language, so that the desktop language does not override me after I have chosen.
19. As an audio tester who has never used the combo, I want the next launch to follow the Windows display language again, so that simply opening the tool does not lock a language in.
20. As an audio tester, I want collapsing or expanding the log before I ever use the combo to leave the language key unwritten, so that a log-panel click is not a language choice.
21. As an audio tester, I want a later log collapse toggle to keep the language I already chose, so that the two preferences do not erase each other.
22. As an audio tester, I want a language save to keep the log collapsed or expanded as it was, so that switching language does not pop the log open or shut.
23. As an audio tester with no preference file, I want an expanded log and the display-language default, so that a missing file is a fresh start.
24. As an audio tester whose preference file has a log bit and no language key, I want the log bit honored and the language taken from the display language, so that an older file still works.
25. As an audio tester whose language value is empty or some other word, I want the display-language default, so that a bad value does not blank the window.
26. As an audio tester whose preference file cannot be read, I want the display-language default and an expanded log, so that a broken file does not trap the window.
27. As an audio tester, I want a failed language save to keep the language I just picked on screen, so that a disk error does not snap the words back.
28. As an audio tester, I want that failure to be a warning in the diagnostic log only, so that the log panel and the window stay free of a dialog.
29. As an audio tester, I want the next combo change to try the save again, so that a transient disk error can heal.
30. As an audio tester who copies the exe folder to another machine, I want the language choice to travel in `winaudio.ui.ini` beside the exe, so that the tool stays portable.
31. As an audio tester, I want a Chinese sentence to keep glossary terms in English, so that Track, Monitor, Loopback, Capture, Render, WASAPI, PID, Shared, and Exclusive stay the words I match to docs and logs.
32. As an audio tester, I want the surrounding operation and explanation to be Chinese, so that the sentence is readable without renaming the domain.
33. As an audio tester, I want a sentence that has no Chinese wording to show its English sentence, so that a missing translation never becomes a blank control.
34. As an audio tester in English, I want every sentence in English, so that a filled Chinese slot stays hidden until I choose 中文.
35. As an audio tester, I want tab names, Render controls, Pipeline chrome, chart controls, Options, and capability dialogs to follow the same language, so that one switch covers the window.
36. As an audio tester, I want the log panel's Clear control and its empty placeholder to follow the language, so that the panel's own chrome is GUI copy.
37. As an audio tester, I want app-written status lines, including sound-settings launch failures, to follow the language, so that those lines are not stuck in English inside a Chinese window.
38. As an audio tester, I want core and spdlog lines in the log list to stay English, so that diagnostic text still matches the source and the issue tracker.
39. As an audio tester, I want the log level names to stay the English tokens, so that the filter matches the level I set.
40. As an audio tester, I want the log collapse control to stay + and -, so that the glyph does not become a translated word.
41. As an audio tester, I want device names, process names, PIDs, volumes, format numbers, channel indexes, and HRESULTs to stay as data, so that translation does not rewrite a measurement.
42. As an audio tester, I want the Format caption translated and the numbers left as numbers, so that I can read the label without losing the format.
43. As an audio tester, I want on-screen channel-phrase labels to follow the language, so that the Render page names the channel I am soloing.
44. As an audio tester, I want the spoken Channel ident to stay the English recording, so that the WAV set does not change with the window language.
45. As an audio tester, I want live-session tooltip and empty-call-log wording to follow the language while names and state values stay raw, so that the sentence is translated and the session is not.
46. As an audio tester, I want stream-option labels to follow the language, so that the Options combo is not a leftover English list.
47. As an audio tester, I want Chinese GUI copy drawn with Microsoft YaHei when that system font is present, so that the characters are the system font I already have.
48. As an audio tester, I want the same font load in English and in Chinese, so that switching language does not swap fonts.
49. As an audio tester whose machine has no YaHei file, I want the window to open on the default font and record a warning, so that a missing font is not a failed launch.
50. As a GitHub visitor, I want the repository landing page to be the English README, so that the default page matches the project's English release notes.
51. As a GitHub visitor who reads Chinese, I want a link to `README.zh-CN.md`, so that I can read the same guide in Chinese.
52. As a GitHub visitor reading the Chinese guide, I want a link back to the English README, so that I can move between the two pages.
53. As a GitHub visitor, I want the English README to show the English Monitor screenshot, so that the picture matches the page.
54. As a GitHub visitor, I want the Chinese README to show the Chinese Monitor screenshot, so that the picture matches that page.
55. As a GitHub visitor, I want the English screenshot page to use English captions and English images, and to link to the Chinese page, so that the page is one language.
56. As a GitHub visitor, I want the Chinese screenshot page to keep the Chinese captions and Chinese images, and to link to the English page, so that today's Chinese captions are not thrown away.
57. As a GitHub visitor, I want Loopback and Application Loopback screenshots in the same language as the page I am reading, so that those modes are shown the same way as Monitor.
58. As a GitHub visitor, I want the Monitor image on the README and the Monitor image on the screenshot page to be the same file for that language, so that the two pages do not drift.
59. As a GitHub visitor reading Chinese, I want glossary terms left in English inside the Chinese guide, so that the guide uses the same words as the window and the domain glossary.
60. As a GitHub visitor, I want the guide's CLI section in the language of the page I am reading, so that the landing page explains the tool in that language.
61. As an audio tester, I want the CLI program's own usage text to stay English, so that scripts and `--help` do not grow a second language.
62. As a GitHub visitor, I want release notes to stay the English PR titles, so that the release history stays one list.

## 2. GUI copy

### Catalog

The GUI copy catalog is one compile-time parallel table. Each sentence is one row with three fields: a stable identifier, the English literal, and a Chinese slot. The Chinese slot is empty when that sentence has no Chinese wording yet. There is no third language column.

The English identifier stays a compile-time pointer to the English literal for the life of the program. Callers that already compare those identifiers to English sentences keep doing that. Widgets that draw a sentence pass the lookup result. They do not pass the English pointer as the visible label.

Lookup is by pointer identity of that English literal. The current language is the GUI language resolved below. When the language is Chinese and the slot is present and not empty, lookup returns the Chinese pointer. Otherwise it returns the same English pointer. Lookup of a catalog sentence never returns null and never allocates. An unknown non-null pointer is returned unchanged. The audio thread never calls lookup.

Identical English literals share one Chinese slot, because the compiler may merge them. Two rows must not give the same English literal two different Chinese wordings.

Adding a sentence means adding one row. Combo entries, each channel-phrase label, and each fragment of a tooltip or empty-state line are rows in this same table. There is no second catalog, no gettext file, no JSON sidecar, and no Win32 string table.

The window builds a combo's pointer list on the GUI thread by looking up each row. That list is pointers, not newly allocated strings. The channel-phrase helper returns the lookup of that phrase's row. Tooltip and empty-call-log helpers look up their fragments and concatenate them on the GUI thread. Process names, device names, volumes, and state values are copied through as data. The tooltip helper may allocate one string on the GUI thread. Lookup itself still must not.

Catalog strings are visible wording only. They do not embed an ImGui identifier. A control whose identity is its label keeps a language-stable identity: the English identifier, either pushed as the id or appended after the visible lookup. Plain text that is not a control id uses the lookup alone.

These rows are the wording pattern. They are not a full table. Every other Chinese slot is filled on its row when the catalog is implemented, and reviewed in that PR. An empty slot is the fallback case.

| English | Chinese |
| --- | --- |
| Apply | 应用 |
| Create Track | 创建 Track |
| Application Loopback | Application Loopback |
| Create a Track to capture system audio. | 创建 Track 以采集系统音频。 |
| Pause charts | 暂停图表 |
| (empty) | （空） |
| mmsys.cpl: ShellExecute failed | mmsys.cpl：ShellExecute 失败 |

### Which sentences

GUI copy is every sentence a person reads in the WinAudio window, including the log panel's own chrome and the status lines the app writes. Sentences that are still bare literals in the window join this catalog: format captions, Close, refresh, status words, category names, chart-mode names, capability titles, and the same kind of leftover label.

These stay out of the catalog:

- Core and spdlog lines.
- Log level tokens Trace, Debug, Info, Warn, and Error.
- The characters + and -.
- The combo labels English and 中文, which stay those two words in both languages.
- Device names, process names, PIDs, volumes, format numbers, channel indexes, and HRESULTs.

The Format caption is a sentence. The numbers after it are data. On-screen channel-phrase labels are GUI copy. The Channel ident recordings stay the English WAVs.

### Wording

Chinese GUI copy keeps each `CONTEXT.md` glossary term in English. That includes Monitor, Track, Loopback, System Loopback, Application Loopback, Capture, Render, WASAPI, PID, Shared, Exclusive, Chart Host, Chart freeze, Channel pause, and Channel ident. The operation and the explanation around those terms are Chinese. The example rows above are the pattern.

### Language choice

There are two GUI languages: English (`en`) and simplified Chinese (`zh-CN`).

The Windows display language is the user UI language, read once at startup with the Win32 user-UI-language call, before the first frame. It is not the regional-format locale. Primary language Chinese selects `zh-CN`. That includes simplified, traditional, and Hong Kong display languages. Every other primary language selects `en`. The running tool does not subscribe to a later change of the display language.

The switch is the combo from the language-switch prototype, variant A: trailing end of the page tab bar, items English then 中文, no caption, visible on every page. The throwaway prototype branch is not merged. Choosing an item applies that language to the whole window on the next draw, without restarting a stream or rebuilding the audio graph.

The override is the key `language` under `[gui]` in the exe-sibling `winaudio.ui.ini`. The only stored values are `en` and `zh-CN`, matched exactly after trimming whitespace. The combo labels are not written to the file. The key is written only after the combo has been used. Using the combo writes the key even when the chosen language matches the current display language. After the key exists, startup uses the key and ignores the display language.

Missing file, missing key, unreadable file, empty value, or any other string follows the display language. When several `language` lines exist, the last one wins. If that last value is not exactly `en` or `zh-CN`, follow the display language.

`log_collapsed` stays in the same `[gui]` section. A write of either key keeps the other. A log write performed before the combo has ever been used must not create `language`. A file that already contains a bare `log_collapsed` line and no section header still loads that bit. Keys under `[gui]`, and keys that appear before any section header, both count. New writes emit a `[gui]` section and only the keys that are actually persisted.

A failed write keeps the in-memory language the user just chose, records a Warn, does not add a log-panel line, and does not open a dialog. The next combo change tries the write again. Startup records one Info line with the resolved language and whether it came from the override or the display language. Successful combo changes are not logged again. Lookup is not logged.

The preference file is not the registry, not `%AppData%`, not a second sidecar, and not `imgui.ini`.

### Font

Chinese GUI copy uses the system Microsoft YaHei face already loaded for the window: `C:\Windows\Fonts\msyh.ttc`, with the simplified-common glyph range. It is loaded once at startup, for both languages, and it is not switched when the language changes. The font file is not redistributed. If the file is missing, startup records a Warn and keeps the ImGui default font. Noto Sans CJK and Source Han Sans are not bundled.

## 3. Repo guide

Repo guide is the README prose on the GitHub landing page plus the screenshot page that README links to.

`README.md` is English and is the page GitHub opens. `README.zh-CN.md` is the Chinese guide. Each page links to the other near the top. The current Chinese README is the source for the Chinese file. The English file covers the same sections: screenshot, features, download and run, build, GUI use, CLI use, logging, known limits, and development. Do not add `README.en.md`, and do not move either guide under a language subdirectory.

Chinese repo-guide prose keeps glossary terms in English, same as GUI copy. The CLI section of each README is repo guide and follows that page's language. The CLI program's usage text is unchanged.

Screenshot pages match the READMEs:

- English page `docs/gui-screenshots.md`. The English README links only here.
- Chinese page `docs/gui-screenshots.zh-CN.md`. The Chinese README links only here.
- The two pages link to each other under the title.
- Each page embeds only that language's images and captions.
- The Chinese captions that are on the screenshot page today move to the Chinese page. The English page gets English captions.

Image files stay in `docs/images/`, with no language subdirectory. Both languages use a suffix. The README homepage Monitor image and the screenshot-page Monitor image are the same file for that language.

| Page | English file | Chinese file |
| --- | --- | --- |
| Monitor | `docs/images/winaudio-monitor-running.en.png` | `docs/images/winaudio-monitor-running.zh-CN.png` |
| Loopback | `docs/images/winaudio-loopback-running.en.png` | `docs/images/winaudio-loopback-running.zh-CN.png` |
| Application Loopback | `docs/images/winaudio-application-loopback-running.en.png` | `docs/images/winaudio-application-loopback-running.zh-CN.png` |

The English README and the English screenshot page reference only the `.en.png` files. The Chinese README and the Chinese screenshot page reference only the `.zh-CN.png` files. The three unsuffixed `winaudio-*-running.png` files are removed. Render and Pipeline screenshots are not part of this work.

GitHub About, the Settings description and website, is not repo guide. This spec does not fill it.

## 4. 测试

A good test asserts behavior a caller can see: which language is resolved, which sentence pointer comes back, and which keys remain in the preference file. It does not assert widget layout, draw order, or private table indexes.

One seam: **GUI language**. The same surface answers both questions the window asks. Given the preference text, or a missing file, plus an injected "display language is Chinese" flag, it reports the GUI language and the log-collapsed bit. Given an English catalog pointer and that language, it returns the sentence pointer. Given a save of either preference, the resulting file still contains the other key. Tests inject the display-language flag so they do not read the machine's real language. They use temp files. They include the GUI headers, they do not link the GUI executable, and they do not open WASAPI or a device.

Prior art is the temp-file tests that already round-trip the log-collapsed bit, and the tests that already compare each catalog constant to its English sentence. Extend that surface. Do not add a second preference writer that replaces the file with one key.

Cover at least:

- Every existing English catalog constant still equals its English sentence.
- English language returns the same pointer as the English constant.
- Chinese language with a filled slot returns the Chinese pointer for a row that has one, including the example rows that name Track and Application Loopback.
- A null slot and an empty slot return the same English pointer.
- Lookup does not return null for a catalog sentence.
- An unknown non-null pointer is returned unchanged.
- Missing file, missing key, empty value, and a non-matching value follow the injected display language.
- `en` and `zh-CN` win over the injected display language.
- The last language value wins. An illegal last value follows the display language.
- A bare `log_collapsed` line still loads. A language value does not drop it. An illegal language value does not drop a valid log bit.
- Saving the language keeps `log_collapsed`. Saving the log bit keeps `language` after the combo has been used.
- Saving the log bit before any language choice does not create `language`.
- An unreadable path reports failure and does not pretend the override was stored.
- Tests do not pin every Chinese sentence. They do not render ImGui, load YaHei, or snapshot pixels.

Repo guide is a file-name contract, checked by review of the README links and the image names above. It is not a gtest. Font fallback is the startup Warn when the face is absent. It is not a gtest. This work does not change a stream, so a device smoke is not a gate. A local look at the combo is enough for the window, and it is not a CI gate.

## 5. 非目标

- CLI usage text and CLI status lines.
- Core and spdlog runtime lines.
- Code comments and API identifiers.
- Channel ident recordings, and any new spoken-language take.
- Maintainer docs: `CLAUDE.md`, design docs, ADRs, and `CONTEXT.md`.
- A third GUI language, a Traditional Chinese catalog, and a third README.
- A combo item that means "follow the system", and a control that deletes a stored override.
- Live updates when the Windows display language changes while the tool is running.
- A different language per page.
- Render and Pipeline screenshots.
- GitHub About (Settings description and website).
- Filling About later is a separate choice: one English short description is enough, and Chinese readers use `README.zh-CN.md`.
- The full Chinese GUI-copy table inside this spec. The rows are filled in the catalog and reviewed in the PR.
- Bundling Noto Sans CJK, Source Han Sans, or any other CJK face.
- Translating log level tokens, or replacing + and -.
- Release notes. They stay auto-generated English PR titles.
- Changing capture, playback, loopback, latency, or the chart pipeline.
- Merging the `prototype/language-switch` branch.

### Further Notes

The map's closed decisions are the font survey (#134), the catalog-shape survey (#135), the GUI-copy inventory (#133), the combo placement (#136), the preference file (#137), the two screenshot pages (#138), the catalog (#139), the image names (#140), the chapter layout of this file (#141), GitHub About (#142), example rows rather than a full translation table (#143), YaHei (#144), and the `language` key (#145). Research notes stay on their throwaway branches.

`CONTEXT.md` already defines GUI copy and Repo guide. This spec uses those terms and does not edit the glossary.

A sensible delivery order is the GUI language seam first, then the window wiring (leftover sentences, combo, font warning), then the repo guide. The seam builds and tests on its own. The repo guide does not depend on the window. The prototype is reference only.

The user-facing preference filename, the README filenames, the screenshot filenames, and the YaHei path are part of the contract above. Other source paths are left to the implementer so this spec does not freeze them.
