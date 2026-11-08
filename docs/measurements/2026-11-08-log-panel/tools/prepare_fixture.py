"""Inject fixed ordinary-log input into a copy of the opt-in GUI fixture."""
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
source = (root / 'src/gui/PerformanceFixture.cpp').read_text(encoding='utf-8')
header = (root / 'src/gui/AppUi.h').read_text(encoding='utf-8')
anchor = '    ui.logCollapsed_ = false;'
assert source.count(anchor) == 1
injection = '''
    if (const char* value = std::getenv("WINAUDIO_LOG_PANEL_LINES")) {
        for (int i = 0; i < std::atoi(value); ++i)
            ui.logLines_.push_back("23:00:00.000 [I] [capW] Wasapi::IAudioCaptureClient::ReleaseBuffer frames=480 flags=0 ret=S_OK " + std::to_string(i));
    }
'''
if 'char logFilter_' in header:
    injection += '''
    if (const char* value = std::getenv("WINAUDIO_LOG_PANEL_SEARCH"))
        std::snprintf(ui.logFilter_, sizeof(ui.logFilter_), "%s", value);
'''
source = source.replace(anchor, anchor + injection)
anchor = '    ui.frameProbe_.setStage(warming ? "warmup" : scenario.c_str());'
assert source.count(anchor) == 1
first_line = '            ui.logFirstLine_ += ui.logLines_.size();\n' if 'logFirstLine_' in header else ''
alternate = '''
    if (!warming && scenario == "idle" && std::getenv("WINAUDIO_LOG_PANEL_ALTERNATE")) {
        static int previousLogPhase = -1;
        static const char* logStages[] = {"log-empty-1", "log-full-1", "log-empty-2", "log-full-2"};
        const int logPhase = (std::min)(3, static_cast<int>((elapsed - warmup) / 20));
        if (logPhase != previousLogPhase) {
FIRST_LINE            ui.logLines_.clear();
            if (logPhase % 2)
                for (int i = 0; i < 5000; ++i)
                    ui.logLines_.push_back("23:00:00.000 [I] [capW] Wasapi::IAudioCaptureClient::ReleaseBuffer frames=480 flags=0 ret=S_OK " + std::to_string(i));
            std::ofstream stageFile("log-stages.csv", logPhase ? std::ios::app : std::ios::trunc);
            stageFile << logPhase << ',' << fixtureTick() << '\\n';
            previousLogPhase = logPhase;
        }
        ui.frameProbe_.setStage(logStages[logPhase]);
    }
'''.replace('FIRST_LINE', first_line)
source = source.replace(anchor, anchor + alternate)
Path(sys.argv[2]).write_text(source, encoding='utf-8')
