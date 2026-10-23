/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL client: embedding into a host window, and a local stats/control pipe for that host
 *
 * Copyright 2026 Hamed Ghorbani
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <SDL3/SDL.h>
#include <freerdp/freerdp.h>

/* Everything here is inert unless /parent-window:<hwnd> is given (FreeRDP_ParentWindowId != 0):
 * a standalone sdl-freerdp behaves exactly as upstream.
 *
 * EMBEDDING   the session window is a plain WS_CHILD window this process creates inside the host's
 *             window and hands to SDL as an EXTERNAL window (SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER).
 *             From then on the HOST owns its position and size: SdlWindow's state setters (border,
 *             resizable, fullscreen, minimise, raise, resize) do nothing for it, because SDL's Win32
 *             backend positions windows in SCREEN coordinates, which for a child window land in the
 *             wrong place, and re-applies its own frame styles. The session follows host resizes
 *             through SDL's normal resize events.
 * DISPLAYS    the session never goes fullscreen, so displays are measured from SDL's display API
 *             instead of upstream's probe (a borderless fullscreen window flashed on every monitor).
 * STATS PIPE  \\.\pipe\finestrardp.<pid>, one client: once a second it writes
 *               STATS <avgRtt ms> <bw kbps> <baseRtt ms> <resultCbCount> <syncCbCount> <touch> <rdpei>
 *             (the first five fields are the Windows client's line, unchanged; <touch> is 1 in touch
 *             mode, <rdpei> is 1 while the server's multi-touch channel is connected) and accepts, one
 *             per line, in order, within ~100 ms:
 *               PAUSE / RESUME     suppress-output on / off
 *               TOUCH ON / OFF     switch between touch mode and mouse mode
 * TOUCH MODE  touch contacts go to the server as real multi-touch input (MS-RDPEI, /multitouch);
 * MOUSE MODE  (the default when embedded) the window is not registered for WM_TOUCH, so Windows
 *             turns touch into mouse input with its own gestures - a tap clicks, a drag drags,
 *             press-and-hold is the right button - as for the Windows client's window; finger
 *             events are ignored. A pen is a mouse.
 * RENDERER    32-bit ARM builds default SDL to the direct3d (D3D9) renderer: on Windows RT's Tegra 3
 *             (feature level 9_1) the D3D11 renderer draws a black window. SDL_RENDER_DRIVER overrides. */

namespace finestra
{
	[[nodiscard]] bool embedded(const rdpSettings* settings);

	/* After the command line is parsed, before SDL_Init: SDL hints and the process-wide embed flag. */
	void prepareProcess(const rdpSettings* settings);

	/* When embedding, fills monitor for display id from SDL's display API and returns true;
	 * otherwise returns false and the caller measures the display its own way. */
	[[nodiscard]] bool queryDisplay(SDL_DisplayID id, bool forceAsPrimary, rdpMonitor& monitor);

	/* Start of window creation: remember the host window. Main thread. */
	void prepareWindows(const rdpSettings* settings);

	/* Just before SDL_CreateWindowWithProperties: when embedding, create the child window inside
	 * the host and put it into the creation properties. */
	void createProperties(SDL_PropertiesID props);

	/* Just after a window was created from those properties. */
	void windowCreated(SDL_Window* window);

	/* True for the embedded session window: its geometry and state belong to the host. */
	[[nodiscard]] bool isEmbedded(SDL_Window* window);

	/* True exactly once, on the embedded window's first frame. */
	[[nodiscard]] bool takeFirstDraw();

	/* Register the client-side autodetect callbacks that feed the STATS line. In PreConnect. */
	void preConnect(rdpContext* context);

	/* Start the stats/control pipe (dormant until the host connects). In PostConnect. */
	void postConnect(rdpContext* context);

	/* Commands from the pipe arrive as an SDL user event; handle one. Main thread.
	 * Returns true when the event was one of ours. */
	[[nodiscard]] bool handleEvent(rdpContext* context, const SDL_Event& ev);

	/* Whether finger events are to be sent as touch (true) or ignored in favour of the
	 * mouse events SDL synthesises from them (false). */
	[[nodiscard]] bool touchMode();

	/* The process exit code. Embedded: FreeRDP's last error (an ERRCONNECT_* value), as the Windows
	 * client returns it, so a host that maps those values keeps working; 0 when there is none,
	 * unless the client itself failed (a negative code, e.g. a command-line error). Standalone:
	 * sdlCode, upstream's own numbering. */
	[[nodiscard]] int exitCode(rdpContext* context, int sdlCode);

	/* True between the host's PAUSE and RESUME. Showing the window must not lift a pause the
	 * host asked for; hiding it still suppresses output, as upstream does for any window. */
	[[nodiscard]] bool outputPaused();
} // namespace finestra
