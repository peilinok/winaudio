# Discovery defines these tests before this file is included by CTest.
foreach(wa_test IN ITEMS
    CaptureTrackList.SlowDumpCloseKeepsBothCaptureTapsAndDevicesAdvancing
    CaptureTrackList.OptionalOverflowKeepsCapturingAndDrainsTheAcceptedPrefix
    CaptureTrackList.SlowDumpFatalAssertionDoesNotBlockTrackTeardown
)
    list(FIND WinAudioTests_TESTS "${wa_test}" wa_test_index)
    if(wa_test_index GREATER -1)
        set_tests_properties("${wa_test}" PROPERTIES TIMEOUT 30)
    endif()
endforeach()
