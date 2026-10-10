OpenXR core: session/views/swapchain, Win32/X11/Wayland binding, frame loop, mirror blit, FOV/projections, world lock/recenter, recovery, \[xr] config (50 keys), loader 1.1→1.0 fallback.

Input/HUD: controller actions + bindings, hand rays, controller/head mouse, overlay keyboard, VR menu + OSD toasts + ray ribbons, joystick/chords, mapper handlers, presets (XRCFG.COM, save/load/list).

Stereo/depth: modes (mono/anaglyph/alternating/interleaved/top-bottom/depth), Voodoo capture, autodepth + neural fusion, disparity/strength/divisor controls.

Neural: ONNX Runtime loader, model registry/downloader, worker thread, XRDEPTH tool.

VFX-1: VIP responder (16-wide ports, echo latch, HWID, IRQ), LPT4 conflict removed, CS:IP-tagged tracing, VFX1\_BUILD\_ID stamps. Error 27 root-caused to ACCESS.bus handshake waiter (0x4C38, 30k spins, waiter 01E7:4D49); still failing — parked.

Refactor: sdl\_gui.cpp 11664→3225 lines; new xr/osd/xrinput/xrstereo/xrneural.cpp + headers, hardware/vfx1.cpp; xrhud→osd; OSD options moved to new \[osd] section (osd\_desk split toggle, osd\_xr\*); README updated.

Hygiene: clean builds, dosbox-xr.exe shipped hash-verified, boot/exit smoke tests, no stray processes.

