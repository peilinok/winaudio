# 8-channel GUI-frame optimization list

Admission: [What size admits a cited measurement onto the optimization list?](https://github.com/peilinok/winaudio/issues/168)

## heatmap-submit

Reference-scene frame probe, Release.

- scene: `Render-page;chartChannels=8;spectrograms=scrolling;log=default-expanded`
- machine: CPU=i7-10875H; GPU=RTX 2060; memory=63.91GB; refresh_hz=59; power_mode=Balanced; windows=Win11 25H2
- N=3; ticks=16825; qpc_freq=10000000; nanos=1682500; uncertainty_ticks=3
- command: `set WINAUDIO_FRAME_PROBE=1 & .\build\bin\Release\WinAudioGui.exe`

Attached device-free (not a point by itself): `refreshCharts`, Release, N=16, 177061600 ns, ±1 tick.

## waveform-envelope

Reference-scene frame probe, Release.

- scene: `Render-page;chartChannels=8;spectrograms=scrolling;log=default-expanded`
- machine: CPU=i7-10875H; GPU=RTX 2060; memory=63.91GB; refresh_hz=59; power_mode=Balanced; windows=Win11 25H2
- N=1; ticks=4862; qpc_freq=10000000; nanos=486200; uncertainty_ticks=1
- command: `set WINAUDIO_FRAME_PROBE=1 & .\build\bin\Release\WinAudioGui.exe`

## log-drain

Reference-scene frame probe, Release.

- scene: `Render-page;chartChannels=8;spectrograms=scrolling;log=default-expanded`
- machine: CPU=i7-10875H; GPU=RTX 2060; memory=63.91GB; refresh_hz=59; power_mode=Balanced; windows=Win11 25H2
- N=1; ticks=4; qpc_freq=10000000; nanos=400; uncertainty_ticks=1
- command: `set WINAUDIO_FRAME_PROBE=1 & .\build\bin\Release\WinAudioGui.exe`

## present

Reference-scene frame probe, Release.

- scene: `Render-page;chartChannels=8;spectrograms=scrolling;log=default-expanded`
- machine: CPU=i7-10875H; GPU=RTX 2060; memory=63.91GB; refresh_hz=59; power_mode=Balanced; windows=Win11 25H2
- N=1; ticks=133477; qpc_freq=10000000; nanos=13347700; uncertainty_ticks=1
- command: `set WINAUDIO_FRAME_PROBE=1 & .\build\bin\Release\WinAudioGui.exe`
