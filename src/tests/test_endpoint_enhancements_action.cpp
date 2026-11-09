#include <gtest/gtest.h>
#include "EndpointEnhancementsAction.h"
#include <windows.h>
#include <shellapi.h>

namespace {
using namespace wa;
namespace action = wa::endpoint_enhancements_action;

std::optional<int> invoke(const std::wstring& parameters, bool elevated, int& calls,
                          DataFlow& flow, DeviceId& device, EnhancementsState& state,
                          int result = 0) {
    const std::wstring command = L"\"C:\\Program Files\\WinAudio\\WinAudioGui.exe\" " + parameters;
    int count = 0;
    auto args = CommandLineToArgvW(command.c_str(), &count);
    if (!args) return 99;
    const auto handled = action::handle(count, args, elevated,
        [&](DataFlow f, const DeviceId& d, EnhancementsState s) {
            ++calls;
            flow = f;
            device = d;
            state = s;
            return result;
        });
    LocalFree(args);
    return handled;
}
}

TEST(EndpointEnhancementsAction, NormalStartupDoesNotRunAnAction) {
    int calls = 0;
    DataFlow flow{};
    DeviceId device;
    EnhancementsState state{};
    EXPECT_FALSE(invoke(L"", false, calls, flow, device, state).has_value());
    EXPECT_FALSE(invoke(L"unrelated", true, calls, flow, device, state).has_value());
    EXPECT_EQ(calls, 0);
}

TEST(EndpointEnhancementsAction, ElevatedActionPreservesEndpointAndRequestedState) {
    for (const auto flow : {DataFlow::Capture, DataFlow::Render}) {
        for (const auto state : {EnhancementsState::Enabled, EnhancementsState::Disabled}) {
            for (const DeviceId device : {L"", L"{0.0.1.00000000}.{endpoint}",
                                         LR"(设备 name \"quoted\" tail\)"}) {
                int calls = 0;
                DataFlow actualFlow{};
                DeviceId actualDevice;
                EnhancementsState actualState{};
                const auto result = invoke(action::arguments(flow, device, state), true,
                    calls, actualFlow, actualDevice, actualState);
                ASSERT_TRUE(result.has_value());
                EXPECT_EQ(*result, 0);
                EXPECT_EQ(calls, 1);
                EXPECT_EQ(actualFlow, flow);
                EXPECT_EQ(actualDevice, device);
                EXPECT_EQ(actualState, state);
            }
        }
    }
}

TEST(EndpointEnhancementsAction, UnelevatedActionNeverWritesOrRetries) {
    int calls = 0;
    DataFlow flow{};
    DeviceId device;
    EnhancementsState state{};
    const auto result = invoke(action::arguments(DataFlow::Capture, L"endpoint",
        EnhancementsState::Enabled), false, calls, flow, device, state);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, ERROR_ACCESS_DENIED);
    EXPECT_EQ(calls, 0);
}

TEST(EndpointEnhancementsAction, MalformedInternalRequestsNeverWrite) {
    for (const auto parameters : {
        L"--internal-endpoint-enhancements",
        L"--internal-endpoint-enhancements capture on",
        L"--internal-endpoint-enhancements invalid on endpoint",
        L"--internal-endpoint-enhancements render invalid endpoint",
        L"--internal-endpoint-enhancements render off endpoint extra"}) {
        int calls = 0;
        DataFlow flow{};
        DeviceId device;
        EnhancementsState state{};
        const auto result = invoke(parameters, true, calls, flow, device, state);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, 2);
        EXPECT_EQ(calls, 0);
    }
}

TEST(EndpointEnhancementsAction, DeviceFailureIsReturnedToParent) {
    int calls = 0;
    DataFlow flow{};
    DeviceId device;
    EnhancementsState state{};
    const auto result = invoke(action::arguments(DataFlow::Render, L"endpoint",
        EnhancementsState::Disabled), true, calls, flow, device, state, 1234);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1234);
    EXPECT_EQ(calls, 1);
}
