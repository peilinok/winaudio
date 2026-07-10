#include <gtest/gtest.h>
#include "CliOptions.h"

TEST(CliOptions, LoopbackSilentRenderDefaultsEnabled) {
    const wchar_t* argv[] = {L"WinAudioCli", L"capture", L"--loopback"};

    wa::LoopbackOptions opts =
        wa::cli::parseLoopbackOptions(3, const_cast<wchar_t**>(argv));

    EXPECT_TRUE(opts.silentRender);
}

TEST(CliOptions, NoSilentRenderDisablesHelper) {
    const wchar_t* argv[] = {
        L"WinAudioCli", L"monitor", L"--loopback", L"--no-silent-render"
    };

    wa::LoopbackOptions opts =
        wa::cli::parseLoopbackOptions(4, const_cast<wchar_t**>(argv));

    EXPECT_FALSE(opts.silentRender);
}
