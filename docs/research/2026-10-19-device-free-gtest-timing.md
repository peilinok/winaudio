# Which clock makes a device-free gtest timing citeable?

**Date:** 2026-10-19
**Issue:** https://github.com/peilinok/winaudio/issues/160
**Status:** research note. It does not choose which functions are timed, and it does not choose how a full GUI frame is probed. It does not add a dependency.

The two sources this note is allowed to treat as evidence are Microsoft's documentation of `QueryPerformanceCounter` and GoogleTest's documentation of timing inside a test. Repo build and CI files are constraints only. They are not a measurement.

## Answer

| Piece | What to cite | What not to cite |
| --- | --- | --- |
| Clock | `QueryPerformanceCounter` for the two ends of one interval, on one thread. Cache `QueryPerformanceFrequency` once. | `RDTSC` / `RDTSCP`. `GetTickCount64`. GoogleTest's printed time, XML `time`, or `elapsed_time()`. |
| Warmup | None. Neither source defines a discarded iteration count or a warmup duration. | `--gtest_repeat`, and any warmup protocol taken from another library. |
| Repetition | Not a fixed count. Call the work `N` times inside that one bracket. `N` has to be large enough that the bracket is many ticks, and `N` is part of the citation. | `--gtest_repeat=N`. That flag repeats the test, including global set-up, to flush out flakes. |
| Statistic | The bracket's elapsed tick count, turned into time by multiplying before dividing by the cached frequency. Uncertainty is ±1 tick on that bracket (one clock period). Per-call time is that result divided by `N`, so the same ±1 tick is divided by `N`. | A mean, median, minimum, or standard deviation of several runs. GoogleTest's millisecond duration of the whole test. |

Those two documents do not say that GoogleTest is the wrong host for a citeable timing, and they do not name another library. The alternative of adding a benchmark dependency is therefore not required. The number is computed inside an existing `WinAudioTests` test body. GoogleTest only runs the test and must not be the clock.

## Why GoogleTest's own timer is not the number

GoogleTest's user's guide describes the time it prints as the time to run each test, and says that time can be turned off:

- By default it prints the time it takes to run each test. `--gtest_print_time=0` or `GTEST_PRINT_TIME=0` suppresses that print. Source: [Advanced GoogleTest Topics, "Suppressing the Elapsed Time"](https://google.github.io/googletest/advanced.html#suppressing-the-elapsed-time). The same paragraph is in the v1.14.0 guide this repo vendors (`third_party/googletest`, `GOOGLETEST_VERSION` 1.14.0).

- The XML report "contains the duration of each test, and thus can help you identify slow tests." The `time` attribute "expresses the duration of the test, test suite, or entire test program in seconds." The sample report uses values such as `time="0.035"` and `time="0.007"`. Source: [Advanced GoogleTest Topics, "Generating an XML Report"](https://google.github.io/googletest/advanced.html#generating-an-xml-report).

- `TestResult::elapsed_time()` "Returns the elapsed time, in milliseconds." `testing::TimeInMillis` is "An integer type representing time in milliseconds." Source: [Testing Reference](https://google.github.io/googletest/reference/testing.html). The vendored header matches that unit: `using TimeInMillis = int64_t;  // Represents time in milliseconds.` in `third_party/googletest/googletest/include/gtest/internal/gtest-port.h`, and `elapsed_time()` is commented "Returns the elapsed time, in milliseconds." in `gtest/gtest.h`.

A whole-test duration in integer milliseconds, published so a human can spot a slow test, is not a CPU-interval measurement. One tick of that clock is already 1 ms. The XML sample does not show a finer unit. `RecordProperty` cannot be used to smuggle the result into the reserved `time` attribute: the key must be a valid XML attribute name and "cannot conflict with" `name`, `status`, `time`, `classname`, `type_param`, or `value_param`, and only the last value for a key is kept. Source: [Advanced GoogleTest Topics, "Logging Additional Information"](https://google.github.io/googletest/advanced.html#logging-additional-information).

`--gtest_repeat` is also the wrong repetition. The guide says it repeats test methods "many times" so that "a flaky test will eventually fail." If the program has global set-up and tear-down, "it will be repeated in each iteration as well, as the flakiness may be in it." Source: [Advanced GoogleTest Topics, "Repeating the Tests"](https://google.github.io/googletest/advanced.html#repeating-the-tests). That is a flake hunt, not a timed bracket.

GoogleTest can still host the test. The primer says it "supports any kind of tests, not just unit tests," and that "Tests should be fast." Source: [GoogleTest Primer](https://google.github.io/googletest/primer.html). The timing therefore stays inside one ordinary test: a finite loop between two counter reads, not an open-ended soak and not a second test runner.

Nothing in those pages says to stop using GoogleTest, and nothing in them names another library. This note does not add one.

## Clock

Microsoft's page for the function says it "Retrieves the current value of the performance counter, which is a high resolution (<1µs) time stamp that can be used for time-interval measurements," and points at the conceptual article for how to use it. On Windows XP and later, with valid parameters, the call succeeds and does not return zero. Source: [`QueryPerformanceCounter`](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter).

The conceptual article names `QueryPerformanceCounter` as "the primary API for native code" when measuring time intervals. The fact used here:

- Use it when the stamps need a resolution of 1 microsecond or better and do not need to be synchronized to an external time reference. `GetTickCount64` is the alternative only when 10 to 16 milliseconds is enough. Source: [Acquiring high-resolution time stamps, "Guidance for acquiring time stamps"](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps).

- It is a difference clock. It is not synchronized to UTC, and it is not affected by daylight saving, leap seconds, time zones, or an administrator changing the system clock. For a short interval on one machine it is the measurement Microsoft prefers over an absolute clock, because synchronizing an absolute clock can shift phase and frequency. Same article, "General FAQ" and "Absolute Clocks and Difference Clocks".

- Do not time the interval with `RDTSC` or `RDTSCP`. Microsoft "strongly encourage[s]" `QueryPerformanceCounter` instead, because the raw TSC is not reliable across Windows versions, live migration, or hardware without an invariant or tightly synchronized TSC. Same article, "Direct TSC usage" and the FAQ "Should I use QPC or call the RDTSC/RDTSCP instructions directly?".

- The counter is monotonic: "QPC does not go backward." Same article, "General FAQ".

- The frequency is fixed at boot and is the same on every processor. Call `QueryPerformanceFrequency` once and cache it. Do not assume the returned frequency is the hardware oscillator. Under a hypervisor that implements the hypervisor version 1.0 interface, "or always in some newer versions of Windows," the documented frequency is 10 MHz. The citation therefore includes the frequency that call returned on that run, not an assumed tick size. Same article, "Guidance" and "Resolution, Precision, Accuracy, and Stability".

- The counter keeps counting through standby, hibernate, and connected standby. A CPU bracket must not sleep inside the two reads, or the ticks include that sleep. Same article, the paragraph under `QueryPerformanceFrequency` in "Resolution, Precision, Accuracy, and Stability".

- Results are consistent across processors. Do not pin the thread to one core. The FAQ answer is "No," and pinning "might adversely affect your application's performance." Same article, "FAQ about programming with QPC and TSC".

- Take both reads on the same thread. Across threads, two stamps that differ by ±1 tick have an ambiguous order. On one thread that ordering uncertainty does not apply. That sentence is about ordering, not about the separate ±1 tick quantization of an interval, which still applies. Same article, "Guidance for acquiring time stamps".

- The counter is expected to work on a guest of a correctly implemented hypervisor. A hypervisor that surfaces the reference-time enlightenment can make the read cheaper. It does not change which API to call. Same article, "Virtualization".

`QueryPerformanceCounter` is `Kernel32.dll` (`profileapi.h` / `Windows.h`). It is not a third-party dependency. Static CRT does not replace it.

## Warmup

Neither document defines a warmup.

Microsoft does not say to discard the first iterations, and it does not say that the first call is a special untimed pass. What it does say is a floor on a single short interval:

- Resolution is one tick, `1 / QueryPerformanceFrequency`. A digital counter adds a quantization uncertainty of ±1 tick. When the measured period is much larger than one tick, that error "can often be ignored." When the period approaches the resolution, the error is one clock period and must be kept. Same article, "Resolution, Precision, Accuracy, and Stability".

- Precision is `MAX(resolution, access time)`. If the TSC is the basis, a call costs tens to a few hundred cycles. If a platform timer is the basis, the cost is "frequently in the vicinity of 0.8–1.0 microseconds." If access time is larger than the resolution, "don't try to improve the precision by guessing" where inside the call the stamp was taken. Same article, "General FAQ" ("What is the computational cost of calling QPC?") and the precision section.

So the citeable interval is the long bracket, not a single call that sits on that floor. There is still no discarded prefix to count. Calling the work once before the bracket and throwing that pass away would be an extra rule these documents do not state.

GoogleTest's repeat flag is not a warmup either. It re-runs the test to find a failure, and it re-runs global set-up unless `--gtest_recreate_environments_when_repeating=false` is set. Source: [Repeating the Tests](https://google.github.io/googletest/advanced.html#repeating-the-tests).

## Repetition

Microsoft does not publish a repetition count. The count comes only from the quantization rule above: put `N` calls to the work between one pair of `QueryPerformanceCounter` reads, on one thread, and choose `N` so the elapsed tick count is large beside 1 tick. Report `N` with the ticks. Dividing a short bracket by a large `N` does not remove the ±1 tick; it only spreads that one tick across `N` calls, and it does not beat the access-time floor of the two reads.

There is no documented value for "large." A citation that omits `N`, the tick count, and the cached frequency is not using the rule the article states. A citation that picks a constant such as 1000 or 1e9 and treats it as Microsoft's number is inventing one.

GoogleTest does not supply the missing constant. `--gtest_repeat` repeats tests for flakes. The primer's "tests should be fast" only says the bracket has to finish as one test, not how many iterations that is. Source: [GoogleTest Primer](https://google.github.io/googletest/primer.html).

Integer conversion is part of making `N` honest. Microsoft's native example subtracts the two counts, multiplies by 1,000,000, and only then divides by the frequency, "to guard against loss-of-precision." The programming FAQ adds that integer division drops the remainder, that a `double` cannot represent every 64-bit integer, and that a 64-bit multiply can overflow, so those steps should be delayed. Source: the "Using QPC in native code" example and "FAQ about programming with QPC and TSC" in [Acquiring high-resolution time stamps](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps). Prefer publishing ticks, frequency, and `N`. Convert only when the multiply cannot overflow, and multiply before dividing.

For a short bracket, crystal frequency offset is not the statistic. The article's ±10 ppm table is ±10 nanoseconds on a 1 millisecond interval, and it says that for small intervals this offset "can often be ignored." The offset matters for long intervals and for comparing machines. Same article, the ppm table in "Resolution, Precision, Accuracy, and Stability".

## Statistic

Cite one number: the elapsed ticks of that single same-thread bracket, together with the cached frequency and `N`.

The uncertainty the article attaches to that number is ±1 tick of quantization on the bracket, equal to one clock period. It is not a mean, a median, a minimum, or a standard deviation. Those aggregates are not in the article. Taking the minimum of several brackets would drop the ±1 tick model the article gives and keep the run that happened to read low. The article does not say to do that.

The per-call figure, if one is stated, is bracket elapsed time divided by `N`. Its quantization uncertainty is one tick divided by `N`. It is not a new measurement with a tighter clock.

GoogleTest's `elapsed_time()` and the XML `time` attribute are a different statistic: integer milliseconds, or decimal seconds, for the whole test, so that slow tests can be seen. They are not this bracket.

## Repo constraints, not evidence

These only check that the bracket fits the runner. They do not choose the clock.

- CI is pinned to `windows-2022` and runs `ctest --test-dir build -C Debug` and `-C Release` (`.github/workflows/ci.yml`). A cited number has to name which of those two configs produced it. The two documents above do not say that; the matrix does, because the two binaries are not the same compile.
- The top-level `CMAKE_MSVC_RUNTIME_LIBRARY` is `MultiThreaded$<$<CONFIG:Debug>:Debug>` (`/MT` and `/MTd`). `QueryPerformanceCounter` stays in `Kernel32.dll` either way.
- `WinAudioTests` is the gtest runner (`gtest_main`, `gtest_discover_tests` in `src/tests/CMakeLists.txt`). The bracket is a test in that binary. It does not open an audio device.
- No third-party timing library is added. The vendored GoogleTest is already the runner, not the clock.

## Out of scope

Which function sits inside the bracket, how large a result must be before it is an optimization item, and how a GUI frame or `Present` is probed are not decided here.
