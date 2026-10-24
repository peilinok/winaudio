# Discovery defines these tests before this file is included by CTest.
foreach(wa_test IN ITEMS
    CaptureTrackList.SlowDumpCloseKeepsBothCaptureTapsAndDevicesAdvancing
    CaptureTrackList.OptionalOverflowKeepsCapturingAndDrainsTheAcceptedPrefix
    CaptureTrackList.SlowDumpFatalAssertionDoesNotBlockTrackTeardown
)
    if("${wa_test}" IN_LIST WinAudioTests_TESTS)
        set_tests_properties("${wa_test}" PROPERTIES TIMEOUT 30)
    endif()
endforeach()
