# Spoken channel-ident recordings that can be bundled

Date: 2026-09-22. Ticket: [peilinok/winaudio#87](https://github.com/peilinok/winaudio/issues/87). Parent map: [peilinok/winaudio#86](https://github.com/peilinok/winaudio/issues/86).

This note does not pick a set. It only records which published recordings a public GitHub app can legally ship, and which well-known channel checks cannot.

## What was counted

A channel ident is a short spoken clip that names one channel ("left", "左声道", "center", "LFE", and so on) and is played only on that channel. It qualifies only if all of the following are true:

- The publisher's own page, the license text, or the file's own documentation says what it is. Claims are not taken from secondary write-ups.
- The phrases are already separable: one mono clip per role, or a multichannel file whose channels can be split into those phrases. One interleaved announcement that only visits each speaker in turn does not qualify unless the phrases can be cut out cleanly.
- The license allows redistributing the audio inside this public project. A personal-use, non-commercial, or "no copies" grant does not. If a license page could not be fetched, the set is not treated as bundlable.

No audio file was added to the repository. Sample rate and bit depth for the ALSA files below are the RIFF `fmt ` chunk of the files at the cited raw URLs, read on 2026-09-22. Those files contain only `fmt ` and `data` chunks: no embedded license, copyright string, or transcript.

## Qualify

### ALSA speaker-test voice samples

| Field | Record |
| --- | --- |
| Title | speaker-test pre-defined WAV samples (position-named files only) |
| URL | [speaker-test/samples](https://github.com/alsa-project/alsa-utils/tree/master/speaker-test/samples) in [alsa-project/alsa-utils](https://github.com/alsa-project/alsa-utils) |
| License | GNU General Public License, version 2 (GPL-2.0). Not "or later" for these files: the package `COPYING` is GPL version 2, and the repository is published as GPL-2.0. The `speaker-test.c` header's "version 2 or any later version" applies to that program, and is not repeated on the WAV files. |
| License URL | [COPYING](https://github.com/alsa-project/alsa-utils/blob/master/COPYING) in the same repository; canonical text [GPL-2.0](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html) |
| Language | English. One set of files. UI strings in `speaker-test` are gettext and are not extra recordings. |
| Sample rate | 48000 Hz |
| Bit depth | 16-bit little-endian PCM (format tag 1) |
| Channels of each file | 1 (mono) |
| Roles named | Front Left, Front Right, Front Center, Rear Left, Rear Right, Rear Center, Side Left, Side Right |
| Already split per channel | Yes. One mono file per role. |

Files, all under `speaker-test/samples/` ([Makefile.am](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Makefile.am) installs them as package data):

| File | Data bytes | Length at 48 kHz, 16-bit, mono |
| --- | --- | --- |
| [Front_Left.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Front_Left.wav) | 142084 | 1.480 s |
| [Front_Right.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Front_Right.wav) | 146946 | 1.531 s |
| [Front_Center.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Front_Center.wav) | 137090 | 1.428 s |
| [Rear_Left.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Rear_Left.wav) | 126020 | 1.313 s |
| [Rear_Right.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Rear_Right.wav) | 146436 | 1.525 s |
| [Rear_Center.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Rear_Center.wav) | 130052 | 1.355 s |
| [Side_Left.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Side_Left.wav) | 134824 | 1.404 s |
| [Side_Right.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Side_Right.wav) | 129922 | 1.353 s |

Length is `data-chunk bytes / (48000 × 2)`. The same header read shows [Noise.wav](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/samples/Noise.wav) is the same PCM shape (48000 Hz, 16-bit, mono, 135158 data bytes). It is not a role name and is not a channel ident.

Why these eight are speech that names a role:

- The ALSA project's own testing page says `-twav` is the mode "where speaker-test will tell you the speaker position in a nice soft voice" ([SoundcardTesting](https://www.alsa-project.org/wiki/SoundcardTesting)).
- The man page says `-t wav` plays "pre-defined" WAV files, or a file passed with `-w`. The default directory is `/usr/share/sounds/alsa` ([speaker-test.1](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/speaker-test.1)).
- `setup_wav_file` in [speaker-test.c](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/speaker-test.c) maps those filenames onto channel slots. The names are the roles above.

There is no LFE clip. The same function's file list uses `Rear_Center.wav` for the slot whose on-screen name is "LFE", with the comment `FIXME: should be "Bass" or so`. The samples directory does not contain `LFE.wav`, `Bass.wav`, or `Channel_9.wav` through `Channel_32.wav`, even though the C array names those later files. No Chinese (or other) language set is in the directory.

Why the license allows bundling here: GPL-2.0 section 1 allows verbatim copies in any medium if the recipient gets the license, existing warranty and license notices stay intact, and an appropriate copyright notice is published with the copy ([COPYING](https://github.com/alsa-project/alsa-utils/blob/master/COPYING)). Section 2's last paragraph says mere aggregation of another work with the Program on the same medium does not put that other work under the GPL. Shipping these files next to WinAudio does not relicense WinAudio, and it also does not relicense the clips: they stay GPL-2.0. The `samples/` directory has no separate license file, and the WAV metadata has no copyright string, so the notice that has to travel with a copy is the package `COPYING` plus a note that the files come from alsa-utils. GPL-2.0 section 4 says any copy that is not allowed by the license is void. Dropping the license, or relicensing the clips under a different grant, is not allowed.

### Speech Commands, the words "left" and "right" only

| Field | Record |
| --- | --- |
| Title | Speech Commands dataset, version 0.02, utterances of "left" and of "right" |
| URL | Author paper [arXiv:1804.03209](https://arxiv.org/abs/1804.03209) ([PDF](https://arxiv.org/pdf/1804.03209)); publisher announcement [Launching the Speech Commands Dataset](https://research.google/blog/launching-the-speech-commands-dataset/); archive used by the publisher's example [speech_commands_v0.02.tar.gz](https://storage.googleapis.com/download.tensorflow.org/data/speech_commands_v0.02.tar.gz) (default `--data_url` in [train.py](https://github.com/tensorflow/tensorflow/blob/master/tensorflow/examples/speech_commands/train.py)) |
| License | Creative Commons Attribution 4.0 International (CC BY 4.0) |
| License URL | [Deed](https://creativecommons.org/licenses/by/4.0/) and [legal code](https://creativecommons.org/licenses/by/4.0/legalcode) |
| Language | English |
| Sample rate | 16000 Hz |
| Bit depth | 16-bit linear PCM |
| Channels of each file | 1 (mono) |
| Roles named | "left" and "right" only |
| Already split per channel | Yes. One WAVE file per utterance. Many speakers, not one canonical take. |

The paper states the release terms and the format: the dataset "has been released under the Creative Commons BY 4.0 license", and "Each utterance is stored as a one-second (or less) WAVE format file, with the sample data encoded as linear 16-bit single-channel PCM values, at a 16 KHz rate" ([arXiv:1804.03209](https://arxiv.org/pdf/1804.03209)). The same paper's word counts include Left (3801) and Right (3778) and do not include center, LFE, surround, side, height, or any Chinese phrase. The 2017 Google Research post says the same CC BY 4.0 grant for the first release and that the words include directions ([announcement](https://research.google/blog/launching-the-speech-commands-dataset/)). Version 0.02 adds Backward, Forward, Follow, Learn, and Visual ([paper](https://arxiv.org/pdf/1804.03209)). Those are not speaker roles. Background-noise files in the dataset are not speech.

These are keyword recordings of the words "left" and "right", not a produced surround ident ("front left", "side right", "LFE"). They still match a clip that names the channel, and each file is already one word.

Why the license allows bundling here: CC BY 4.0 section 2(a)(1) grants a worldwide, royalty-free license to reproduce and Share the material, in whole or in part, for any purpose, and to Share Adapted Material ([legal code](https://creativecommons.org/licenses/by/4.0/legalcode)). "Share" includes distribution to the public. Section 3(a) requires attribution: keep the creator identification, copyright notice, license reference, warranty disclaimer, and a URI when the licensor supplied them; say if the material was modified; and say that it is under this license. Section 2(a)(6) says the license is not an endorsement. Credit Pete Warden / the Speech Commands dataset, link the dataset and the CC BY 4.0 deed, and do not present the clips as endorsed by Google. CC BY 4.0 is not ShareAlike: the rest of the app does not become CC BY.

## Do not qualify

**EBU Tech 3304 BLITS (Black and Lane's Ident Tones for Surround).** Tones, not speech. The EBU document specifies tone bursts: L/R 880 Hz, C 1320 Hz, LFE 82.5 Hz, Ls/Rs 660 Hz, then a 1 kHz stereo-ident section, then 2 kHz on all legs ([Tech 3304 PDF](https://tech.ebu.ch/docs/tech/tech3304.pdf), [publication page](https://tech.ebu.ch/publications/tech3304)). The first section visits channels in turn with tones. There is no spoken phrase to cut out.

**EBU multichannel ident in the same document.** Tones, not speech. Section 4.2 is 1 kHz bursts: 3 s on the main channels, then 0.5 s per channel in clockwise order starting at front left, with silence gaps ([Tech 3304 PDF](https://tech.ebu.ch/docs/tech/tech3304.pdf)). It names Front Left, Centre, Front Right, surrounds, and LFE only by which channel the tone is on. Not separable speech.

**EBU R 49 stereo ident, as specified in Tech 3304 §2.1.** A 1 kHz tone at alignment level, interrupted for 250 ms every 3 seconds on the left channel ([Tech 3304 PDF](https://tech.ebu.ch/docs/tech/tech3304.pdf)). Not speech. EBU QC item 0014B lists the stereo reference as [EBU R 049](https://tech.ebu.ch/docs/r/r049.pdf) under "Audio Test Tones" ([0014B](https://qc.ebu.io/items/0014B/)).

**GLITS.** Tones, not speech. EBU QC item 0014B groups "GLITS and BLITS" as optional test-tone patterns under "Audio Test Tones", and lists "BBC GLITS" beside BLITS ([0014B](https://qc.ebu.io/items/0014B/)). That page does not publish a spoken file or a redistribution license. No BBC page publishing a spoken GLITS recording was found.

**SMPTE ST 2095-1, as generated by `speaker-test -t st2095`.** Noise, not speech. [speaker-test.c](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/speaker-test.c) says the option was "added ... based on generator specified by SMPTE ST-2095:1-2015" and describes the signal as "Band-Limited Pink Noise, per SMPTE ST 2095-1". The man page lists `st2095` next to `pink` and `sine`, separate from `wav` ([speaker-test.1](https://github.com/alsa-project/alsa-utils/blob/master/speaker-test/speaker-test.1)). Nothing to bundle; the program synthesizes noise.

**Windows speaker configuration test.** A tone, not a spoken recording, and not a file Microsoft offers for third-party apps. Microsoft's published speaker-setup steps say: "You should hear a tone play in each speaker" ([5.1 surround sound does not work](https://learn.microsoft.com/en-us/answers/questions/2431905/5-1-surround-sound-does-not-work)).

**macOS Audio MIDI Setup speaker test (current guide).** A test tone, not speech. Apple's guide says to click the speaker icon "to test the speaker" and calls the signal the "Speaker configuration test tone" / "a speaker's test tone" ([Set up external speakers](https://support.apple.com/en-gb/guide/audio-midi-setup/ams1005/3.6/mac/26)). No redistributable spoken recording is published there.

**Dolby demo discs and Dolby Access.** No Dolby-published spoken channel-ident set is licensed for this app. Dolby's own support page says demo discs "are only available for our licensees for retail demonstrations" and "we can't distribute demo discs to consumers, due to our agreements with the content rights holders" ([Dolby demo content support](https://www.dolby.com/about/support/dolby-demo-content/)). The Dolby Access end user license says "You may not publish, transfer, or otherwise make the Software available for others to copy" ([Dolby Access terms](https://www.dolby.com/about/legal/terms-of-service/dolby-access/)).

**AudioCheck.** The publisher's homepage says "These tests are for personal and educational use only. No commercial use is allowed without permission" and describes sound tests, test tones, and tone generators ([audiocheck.net](https://www.audiocheck.net/)). That is not a grant to ship the audio inside this public app. The page does not publish a spoken per-channel ident set under any other license.

**Freesound "Surround test_5.1_L R C LFE Ls Rs.wav" by cabled_mess.** The license would allow copying: the file page applies [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/), whose waiver is "for any purpose whatsoever, including without limitation commercial" ([CC0 legal code](https://creativecommons.org/publicdomain/zero/1.0/legalcode)). The content does not qualify. The publisher's description says it is a "surround test file created from a click sound", one Wave file, 48000 Hz, 24-bit, 6 channels, 5.5 s ([sound 591775](https://freesound.org/people/cabled_mess/sounds/591775/)). Splitting the channels yields clicks, not spoken names.

**channeltest.com `left.mp3` and `right.mp3`.** [channeltest/channeltest](https://github.com/channeltest/channeltest) has no GitHub license (API `license` is null). The [README](https://github.com/channeltest/channeltest/blob/master/readme.md) describes a left/right speaker tester and grants nothing. It also does not document sample rate, bit depth, or that the files are speech. No redistribution grant, so not bundlable.

**Realtek HD Audio speaker prompts.** No Realtek page that publishes the voice files, their format, or a license to redistribute them was fetched. Pages retrieved from realtek.com (corporate and security notes) do not contain that grant. Because the license page could not be fetched, these prompts are not bundlable. The same applies to any other OEM or console voice ("前置左声道", Creative Sound Blaster speaker check, and similar) for which this pass did not fetch a publisher license that allows shipping the audio here. No such Chinese set was found.

**ALSA `Noise.wav`.** Same package and GPL-2.0 terms as the voice files, but the filename is not a speaker role. Not a channel ident. See the ALSA section.

## Lists

### Qualify

- ALSA speaker-test position WAVs (eight mono English files, 48 kHz, 16-bit) under GPL-2.0, license text kept with the files. No LFE phrase and no Chinese.
- Speech Commands utterances of the English words "left" and "right" (16 kHz, 16-bit mono) under CC BY 4.0, with attribution. No other speaker role.

### Do not

- EBU Tech 3304 BLITS: tones, not speech.
- EBU Tech 3304 multichannel ident: 1 kHz tone bursts, not speech.
- EBU R 49 stereo ident: interrupted 1 kHz tone, not speech.
- GLITS: EBU QC lists it as a test-tone pattern, not a spoken file.
- SMPTE ST 2095-1 / `speaker-test -t st2095`: generated band-limited pink noise, not a recording of speech.
- Windows speaker setup: Microsoft documents a tone per speaker, not a redistributable spoken file.
- macOS Audio MIDI Setup (current guide): Apple documents a test tone, not speech.
- Dolby demo discs and Dolby Access: Dolby does not let the public redistribute that material.
- AudioCheck: personal and educational use only; no commercial use without permission.
- Freesound 5.1 click file (cabled_mess, CC0): not speech, and it is one 6-channel file of clicks.
- channeltest.com left/right files: no license on the repository.
- Realtek and other OEM spoken prompts: license page not fetched, so not bundlable.
- ALSA `Noise.wav`: not a spoken role name.
