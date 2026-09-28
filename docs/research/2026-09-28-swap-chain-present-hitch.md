# Present on the windowed blit swap chain

Research for the ticket "Does this swap chain add hitches a CPU breakdown would mis-read?" on the map "Main-window frame-time spec". Claims below come from Microsoft Learn DXGI and Direct3D pages, and from the `dxgi.h` those pages name. This note does not change the swap chain and does not choose a swap effect.

## What the main window does

`src/gui/main.cpp` creates a windowed `DXGI_SWAP_CHAIN_DESC` (`Windowed = TRUE`) with `BufferCount` 2, `DXGI_FORMAT_R8G8B8A8_UNORM`, `DXGI_SWAP_EFFECT_DISCARD`, and `DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH`. Each GUI frame calls `ui.draw()`, then `ImGui::Render` and the Direct3D 11 draw, then `g_pSwapChain->Present(1, 0)`. `Present` is not inside `AppUi::draw`.

The header the Present and swap-effect pages name is `dxgi.h`. The Windows SDK copy at `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared\dxgi.h` sets `DXGI_SWAP_EFFECT_DISCARD` to 0 and `DXGI_SWAP_EFFECT_FLIP_DISCARD` to 4, and declares `IDXGISwapChain::Present(UINT SyncInterval, UINT Flags)`. That header does not say what the call waits for.

## What `Present` with sync interval 1 waits for

[`IDXGISwapChain::Present`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present) describes `SyncInterval` as "an integer that specifies how to synchronize presentation of a frame with the vertical blank."

For the bit-block transfer (bitblt) model, which that page names as `DXGI_SWAP_EFFECT_DISCARD` or `DXGI_SWAP_EFFECT_SEQUENTIAL`:

- 0 — the presentation occurs immediately, there is no synchronization.
- 1 through 4 — synchronize presentation after the *n*th vertical blank.

`Present(1, 0)` on this chain therefore synchronizes presentation after the first vertical blank. Interval 0 is the path with no synchronization. The interval is not CPU work inside the draw.

[`DXGI_PRESENT_DO_NOT_WAIT`](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present) is the flag that changes a blocked call: the runtime returns `DXGI_ERROR_WAS_STILL_DRAWING` instead of sleeping until the dependency is resolved. This call passes flags 0, so it does not request that return. The same page does not name the dependency as "only the next vertical blank."

The method that does halt the thread until the next vertical blank is different. [`IDXGIOutput::WaitForVBlank`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-waitforvblank) says "Halt a thread until the next vertical blank occurs." A vertical blank is when the raster moves from the lower right corner to the upper left to begin the next frame. `Present` is not given that halt sentence.

Direct3D 9 is more explicit, and DXGI does not copy the sentence onto `IDXGISwapChain::Present`. The Present page analogizes only `DXGI_PRESENT_TEST` to `IDirect3DDevice9::TestCooperativeLevel`. [`D3DPRESENT_INTERVAL_ONE`](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpresent), for both windowed and full-screen chains, says the driver will wait for the vertical retrace period and that the runtime will complete at most one Present per adapter refresh period. `D3DPRESENT_DONOTWAIT` returns `D3DERR_WASSTILLDRAWING` when the hardware is busy processing or waiting for a vertical sync interval. That is the Direct3D statement that a blocked Present is vertical sync or unfinished GPU work, not the application's draw CPU. It is not the wording of the DXGI method this window calls.

A call can return before the blank, because frames may be queued. [`IDXGIDevice1::SetMaximumFrameLatency`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgidevice1-setmaximumframelatency) says the maximum number of back-buffer frames a driver can queue defaults to 3, and can range from 1 to 16. This window does not call that method. [`IDXGISwapChain2::SetMaximumFrameLatency`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency) is a different control: its default is 1, and it is valid only for a swap chain created with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`. This chain sets only `DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH`, so that API does not apply. The [DXGI overview](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi) says that after `Present1` the application can render the next image. The sources therefore do not say that every `Present(1, 0)` occupies the thread until the next blank. They say the presentation is synchronized after the first vertical blank, and that a blocked `Present` sleeps until its dependency is resolved.

[`IDXGISwapChain2::GetMaximumFrameLatency`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getmaximumframelatency) calls one vertical refresh "typically about 16ms". That is the documented length of a refresh, not a measurement of this window. The same page lists `D3DERR_*` failure codes, which are Direct3D 9 names, so the 16 ms figure is used here only as that page's description of one vertical refresh.

[`IDXGISwapChain2::GetFrameLatencyWaitableObject`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject) exists so an application can wait until the previous frame is presented to the display before drawing the next frame. That wait is defined as presentation, separate from drawing. This chain is not created with the flag that method requires.

**Answer.** On this bitblt chain, sync interval 1 is synchronization of the presentation with the vertical blank (after the first one), not CPU work inside `AppUi::draw`. DXGI does not document the call as `WaitForVBlank`. When the thread is blocked, `Present` sleeps until the dependency is resolved; Direct3D 9 describes that blocked state as the hardware busy processing or waiting for a vertical sync interval. A sleep on the order of one refresh matches the vertical refresh those pages describe (typically about 16 ms). The device-level queue default of 3 means a call can also return without waiting out the blank.

## What `DXGI_SWAP_EFFECT_DISCARD` does on a windowed chain today

The current [`DXGI_SWAP_EFFECT`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect) page still defines `DXGI_SWAP_EFFECT_DISCARD` as the bitblt model, and as discarding the back-buffer contents after `Present1`. With more than one buffer, the application has read and write access only to buffer 0. In that model, used with `DISCARD` and `SEQUENTIAL`, each `Present1` copies the back buffer into the redirection surface. The flip model, which that page names with `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`, shares the back buffers with the Desktop Window Manager, so DWM can compose without that extra copy. The flip model is described as the more efficient one, and as the one that provides enhanced present statistics.

The same page separates Win32 from UWP. UWP is forced into flip swap modes even if another mode was set, because that avoids the memory copies of the older bitblt model. Win32 `DISCARD` is the case that is not forced. This window is Win32 (`DXGI_SWAP_CHAIN_DESC`, `Windowed = TRUE`, `D3D11CreateDeviceAndSwapChain`), not a UWP core window. Direct3D 12 never supports `DISCARD`; this process is Direct3D 11.

[For best performance, use DXGI flip model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model) still calls `DXGI_SWAP_EFFECT_DISCARD` and `DXGI_SWAP_EFFECT_SEQUENTIAL` the blt present model. It does not say that current Windows rewrites a Win32 `DISCARD` chain into `FLIP_DISCARD`.

The [DXGI presentation path](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/dxgi-presentation-path) for windowed mode with DWM on matches the copy. DXGI opens a shared resource in addition to the application's back buffers and calls the driver's `BltDXGI` to move pixels onto that surface. The blit is marked Present and is atomic, to reduce the chance of tearing while DWM reads the surface for composition. That driver page does not mention `FLIP_DISCARD`.

One sentence on the Present page does not match the enumeration. It says a successful presentation unbinds back buffer 0 for flip-model swap chains created with `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL` or `DXGI_SWAP_EFFECT_DISCARD`. The SyncInterval section of that same page still classifies `DISCARD` as bitblt, and the enumeration page does not list `DISCARD` as a flip effect. This note follows the enumeration and the flip-model article for which model `DISCARD` is.

`DXGI_SWAP_EFFECT_FLIP_DISCARD` is the flip presentation model. For Direct3D 11 it is supported starting with Windows 10. It also discards the back buffer after `Present1`, allows read and write access only to buffer 0, and cannot be used with multisampling or partial presentation. `FLIP_SEQUENTIAL` guarantees that each back buffer's contents are preserved across `Present`. `FLIP_DISCARD` does not. DirectFlip, where the compositor uses the application's back buffer as the display buffer and skips the copy into the desktop buffer, can happen for both flip effects when the application is the only visible item. When it is not the only visible item, the enumeration page says the compositor can still do that for `FLIP_DISCARD`, by drawing the other content onto the application's back buffer. The performance article calls that reverse composition: the DWM scribbles on the application buffers instead of copying them into its own swap chain.

Windowed cost, from the table in [DXGI flip model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model): the app writes its frame in both models. Bitblt then has the runtime copy the surface to a DWM redirection surface (a read and a write) before DWM renders it to the screen. Flip passes the app surface to DWM with no additional copy, then DWM renders. The performance article says moving from the blt model to `FLIP_SEQUENTIAL` or `FLIP_DISCARD` gives better performance, lower power, and more features. Its "down to 1 frame of latency" line is for Independent Flip on a flip-model chain created with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`. This chain does not set that flag, and DirectFlip is described for flip-model chains. The sources do not give a millisecond difference between `DISCARD` and `FLIP_DISCARD`.

A frame that misses vblank is specified for the flip model, not for this windowed bitblt chain:

- [`DXGI_FRAME_STATISTICS`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics) `PresentRefreshCount` is the running count of v-blanks at which the last image was presented to the monitor. `IDXGISwapChain::GetFrameStatistics` is only for a flip-model swap chain or for full screen. The flip-model article says that for a bitblt chain in windowed mode, every `DXGI_FRAME_STATISTICS` value is zero. This windowed `DISCARD` chain does not get a missed-vblank count from that structure.
- On the flip model, a glitch is an actual `PresentRefreshCount` later than the count the application expected. The documented example is an expected count of 5 and an actual count of 8, three v-blanks late. The same article's composed timeline has DWM compose at the next vblank and the DAC show that composition on the following vblank. If that compose is late, the DAC shows the previous frame for two intervals and then the new frame.
- `PresentCount` is not the number of `Present` calls. The structure page says the number of times an image was presented to the monitor is not necessarily the number of times `Present` was called.
- For `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`, sync interval 0 cancels the remaining time on the previously presented frame and discards this frame if a newer frame is queued. The flip-queue example on the Present page shows a sync-interval 0 frame ending the previous frame early and dropping queued frames. Bitblt sync interval 0 is different: the presentation occurs immediately, with no synchronization. This window calls interval 1, not 0. The Present page states the flip interval list for `FLIP_SEQUENTIAL` and does not give a separate list for `FLIP_DISCARD`.
- [`DXGI_PRESENT_RESTART`](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present) discards outstanding queued presents. It is valid only for flip-model swap chains and full screen, so it is not the documented recovery for this windowed `DISCARD` chain.

The sources do not say that a missed vblank makes this `Present(1, 0)` sleep for longer than one vertical blank. They say bitblt interval 1 synchronizes presentation after the first vertical blank, and that a late flip-model frame can reach the monitor several v-blanks after the blank the application expected. Windowed bitblt does not report that lateness.

## Whether a timing pass must record `Present` apart from `AppUi::draw`

Yes. `AppUi::draw` returns before `Present(1, 0)`. Time inside that `Present` is the vertical-blank synchronization above. If the thread is blocked, it is a sleep until the dependency is resolved, not CPU work inside the draw. One vertical refresh is typically about 16 ms, so a `Present` of about 16 ms is that refresh wait. A CPU breakdown that folds the call into `AppUi::draw`, or that reads it as a 16 ms interface, mis-reads the sync.

The same sources allow the runtime to queue frames (device default 3) and say that after `Present` the application can render the next image. A later timing pass should keep `Present` as its own sample even when the sample is near zero, and should not assume every call lasts one refresh. A sample of about 16 ms is still not the interface.

Not in these primary sources: a measured hitch for this window, a rule that this `Present` always blocks until the next vblank, and a rule that a missed vblank makes the `DISCARD` call sleep through extra refreshes.

## Sources

- IDXGISwapChain::Present (dxgi.h) — https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
- DXGI_SWAP_EFFECT (dxgi.h) — https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect
- DXGI_PRESENT (DXGI.h) — https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present
- DXGI_FRAME_STATISTICS (dxgi.h) — https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics
- IDXGIOutput::WaitForVBlank (dxgi.h) — https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-waitforvblank
- DXGI flip model — https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
- For best performance, use DXGI flip model — https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
- DXGI overview — https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi
- IDXGIDevice1::SetMaximumFrameLatency (dxgi.h) — https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgidevice1-setmaximumframelatency
- IDXGISwapChain2::GetMaximumFrameLatency (dxgi1_3.h) — https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getmaximumframelatency
- IDXGISwapChain2::SetMaximumFrameLatency (dxgi1_3.h) — https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency
- IDXGISwapChain2::GetFrameLatencyWaitableObject (dxgi1_3.h) — https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
- DXGI Presentation Path — https://learn.microsoft.com/en-us/windows-hardware/drivers/display/dxgi-presentation-path
- D3DPRESENT (D3d9.h) — https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpresent
- Header named by those pages: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared\dxgi.h`
