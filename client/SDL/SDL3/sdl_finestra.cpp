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

#include "sdl_finestra.hpp"
#include "sdl_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <freerdp/autodetect.h>
#include <freerdp/client.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/log.h>
#include <freerdp/scancode.h>

#if defined(_WIN32)
#include <windows.h>
#include <windowsx.h>
#include <tpcshrd.h>
#endif

#define TAG CLIENT_TAG("SDL.finestra")

namespace
{
	enum Command : Sint32
	{
		CMD_PAUSE = 1,
		CMD_RESUME = 2,
		CMD_TOUCH_ON = 3,
		CMD_TOUCH_OFF = 4
	};

	/* Embedded: mouse mode until the host asks for touch. Standalone: upstream behaviour, i.e.
	 * finger events are always sent as touch. */
	std::atomic<bool> g_touch{ true };
	std::atomic<Uint32> g_eventType{ 0 };
	std::atomic<bool> g_embedProcess{ false }; /* /parent-window given: set before SDL_Init */
	std::atomic<bool> g_paused{ false };       /* the host's PAUSE, until its RESUME */
	std::atomic<bool> g_drawn{ false };        /* the embedded window had its first frame */

	/* The embedded session window (one per process) and the host window it lives in. */
	SDL_Window* g_window = nullptr;
#if defined(_WIN32)
	HWND g_parent = nullptr;
	HWND g_child = nullptr;
	constexpr wchar_t kChildClass[] = L"FinestraSessionWindow";

	/* KEYBOARD. SDL decides it has keyboard focus only when GetForegroundWindow() is its own
	 * window (SDL_windowsevents.c WIN_UpdateFocus), which is never true for a child window inside
	 * another process's window. Without SDL keyboard focus the key events carry no window, the
	 * client drops them, and nothing releases held keys or syncs the lock keys on focus changes
	 * (measured: no keystroke reached the server). Instead the embedded window is sub-classed on
	 * top of SDL, and keys go to the server as scancodes, the way the Windows client's keyboard
	 * hook sends them, with focus-in sync and release-all on focus loss. Mouse, touch and
	 * everything else stay on SDL's path. Main thread only (the window's own thread). */
	WNDPROC g_sdlProc = nullptr;
	std::atomic<rdpContext*> g_context{ nullptr };
	bool g_keyDown[256] = {};
	bool g_packetPending = false;

	void focusIn(rdpInput* input)
	{
		UINT16 flags = 0;
		if (GetKeyState(VK_NUMLOCK) & 1)
			flags |= KBD_SYNC_NUM_LOCK;
		if (GetKeyState(VK_CAPITAL) & 1)
			flags |= KBD_SYNC_CAPS_LOCK;
		if (GetKeyState(VK_SCROLL) & 1)
			flags |= KBD_SYNC_SCROLL_LOCK;
		if (GetKeyState(VK_KANA) & 1)
			flags |= KBD_SYNC_KANA_LOCK;
		(void)freerdp_input_send_focus_in_event(input, flags);
	}

	void releaseAll(rdpInput* input)
	{
		for (size_t i = 0; i < 256; i++)
		{
			if (!g_keyDown[i])
				continue;
			g_keyDown[i] = false;
			(void)freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE,
			                                           MAKE_RDP_SCANCODE(i & 0x7F, (i & 0x80) != 0));
		}
	}

	/* One WM_(SYS)KEYDOWN/UP. Returns true when it was sent to the server. */
	bool sendKey(rdpInput* input, UINT msg, WPARAM vk, LPARAM lp)
	{
		const bool down = (msg == WM_KEYDOWN) || (msg == WM_SYSKEYDOWN);
		if (vk == VK_PACKET)
		{
			/* A character the layout cannot produce (on-screen keyboards): it follows as WM_CHAR. */
			g_packetPending = down;
			return true;
		}
		if (vk == VK_PROCESSKEY)
			return false; /* IME composition: leave it to the system */

		BYTE scan = static_cast<BYTE>((lp >> 16) & 0xFF);
		const bool extended = ((lp >> 24) & 1) != 0;
		if (scan == 0)
			scan = static_cast<BYTE>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
		if (scan == 0)
			return false;

		DWORD rdp = MAKE_RDP_SCANCODE(scan, extended);
		if (rdp == RDP_SCANCODE_NUMLOCK_EXTENDED)
			rdp = RDP_SCANCODE_NUMLOCK; /* Windows reports NumLock as extended; RDP does not */
		else if (rdp == RDP_SCANCODE_NUMLOCK)
		{
			/* Windows reports Pause as a non-extended NumLock; RDP wants a one-shot Ctrl+NumLock. */
			if (down)
			{
				(void)freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_LCONTROL);
				(void)freerdp_input_send_keyboard_event_ex(input, TRUE, FALSE, RDP_SCANCODE_NUMLOCK);
				(void)freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_LCONTROL);
				(void)freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, RDP_SCANCODE_NUMLOCK);
			}
			return true;
		}
		else if (rdp == RDP_SCANCODE_RSHIFT_EXTENDED)
			rdp = RDP_SCANCODE_RSHIFT;

		const size_t idx = (scan & 0x7F) | (extended ? 0x80 : 0);
		const bool repeat = down && g_keyDown[idx];
		g_keyDown[idx] = down;
		(void)freerdp_input_send_keyboard_event_ex(input, down ? TRUE : FALSE, repeat ? TRUE : FALSE,
		                                           rdp);
		return true;
	}

	/* MOUSE MODE. The window is not registered for WM_TOUCH, so Windows itself turns touch into
	 * mouse input, with its own gestures - a tap clicks, a drag drags, press-and-hold is the right
	 * button - exactly what the Windows client's window received. SDL ignores those promoted
	 * messages (they carry the touch signature in GetMessageExtraInfo, SDL_windowsevents.c
	 * GetMouseMessageSource), so they are queued here as the SDL mouse events the client handles.
	 * Main thread only. */
	float g_lastX = 0.0f;
	float g_lastY = 0.0f;

	bool promoted()
	{
		constexpr ULONG_PTR signatureMask = 0xFFFFFF00;
		constexpr ULONG_PTR signature = 0xFF515700; /* MI_WP_SIGNATURE: touch or pen */
		return (static_cast<ULONG_PTR>(GetMessageExtraInfo()) & signatureMask) == signature;
	}

	void queueMouse(UINT msg, LPARAM lp)
	{
		SDL_Window* window = g_window;
		if (!window)
			return;
		const float x = static_cast<float>(GET_X_LPARAM(lp));
		const float y = static_cast<float>(GET_Y_LPARAM(lp));
		SDL_Event e = {};
		e.common.timestamp = SDL_GetTicksNS();
		Uint8 button = 0;
		bool down = false;
		switch (msg)
		{
			case WM_MOUSEMOVE:
				e.type = SDL_EVENT_MOUSE_MOTION;
				e.motion.windowID = SDL_GetWindowID(window);
				e.motion.x = x;
				e.motion.y = y;
				e.motion.xrel = x - g_lastX;
				e.motion.yrel = y - g_lastY;
				break;
			case WM_LBUTTONDOWN:
			case WM_LBUTTONDBLCLK:
				button = SDL_BUTTON_LEFT;
				down = true;
				break;
			case WM_LBUTTONUP:
				button = SDL_BUTTON_LEFT;
				break;
			case WM_RBUTTONDOWN:
			case WM_RBUTTONDBLCLK:
				button = SDL_BUTTON_RIGHT;
				down = true;
				break;
			case WM_RBUTTONUP:
				button = SDL_BUTTON_RIGHT;
				break;
			case WM_MBUTTONDOWN:
			case WM_MBUTTONDBLCLK:
				button = SDL_BUTTON_MIDDLE;
				down = true;
				break;
			case WM_MBUTTONUP:
				button = SDL_BUTTON_MIDDLE;
				break;
			default:
				return;
		}
		if (button != 0)
		{
			/* SDL captures the mouse for the buttons it tracks; these it does not, so a drag
			 * that leaves the window would lose its moves and its release. */
			if (down)
				(void)SetCapture(g_child);
			else if (GetCapture() == g_child)
				(void)ReleaseCapture();
			e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			e.button.windowID = SDL_GetWindowID(window);
			e.button.button = button;
			e.button.down = down;
			e.button.clicks = 1;
			e.button.x = x;
			e.button.y = y;
		}
		g_lastX = x;
		g_lastY = y;
		(void)SDL_PushEvent(&e);
	}

	LRESULT CALLBACK childProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		rdpContext* ctx = g_context;
		rdpInput* input = ctx ? ctx->input : nullptr;
		if (!g_touch)
		{
			/* Windows' own touch gestures, as for any window - but no flicks, whose fallback is
			 * keystrokes that would go to the session. SDL answers with everything disabled. */
			if (msg == WM_TABLET_QUERYSYSTEMGESTURESTATUS)
				return TABLET_DISABLE_FLICKS | TABLET_DISABLE_FLICKFALLBACKKEYS;
			if ((msg >= WM_MOUSEFIRST) && (msg <= WM_MBUTTONDBLCLK) && promoted())
				queueMouse(msg, lp); /* and on to SDL, which keeps its own state but sends nothing */
		}
		switch (msg)
		{
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYUP:
				/* Consumed, so system keys (Alt+F4, Alt+Space, F10) reach the session rather than
				 * the host's top-level window - as with the Windows client's keyboard hook. */
				if (input && sendKey(input, msg, wp, lp))
					return 0;
				break;
			case WM_CHAR:
				if (g_packetPending && input)
				{
					const UINT16 ch = static_cast<UINT16>(wp);
					(void)freerdp_input_send_unicode_keyboard_event(input, 0, ch);
					(void)freerdp_input_send_unicode_keyboard_event(input, KBD_FLAGS_RELEASE, ch);
					g_packetPending = false;
					return 0;
				}
				break;
			case WM_SETFOCUS:
				if (input)
					focusIn(input);
				break; /* SDL sees it too */
			case WM_KILLFOCUS:
				if (input)
					releaseAll(input); /* no key stays held in the session while we lack focus */
				break;
			case WM_LBUTTONDOWN:
			case WM_RBUTTONDOWN:
			case WM_MBUTTONDOWN:
			case WM_XBUTTONDOWN:
			case WM_TOUCH:
			case 0x0246: /* WM_POINTERDOWN (Windows 8+; the x64 build targets the Windows 7 API) */
				if (GetFocus() != hwnd)
					(void)SetFocus(hwnd); /* a click into the session gives it the keyboard */
				break;
			default:
				break;
		}
		return g_sdlProc ? CallWindowProcW(g_sdlProc, hwnd, msg, wp, lp)
		                 : DefWindowProcW(hwnd, msg, wp, lp);
	}

	/* The class procedure, under SDL's sub-class (SDL passes every message on to it). Two
	 * defaults are wrong for the session window: DefWindowProc answers WM_SETCURSOR by asking the
	 * host's window first and then setting the class cursor, either of which replaces the remote
	 * cursor SDL has just set; and it answers WM_CLOSE by destroying the window, which SDL has
	 * already turned into a quit request of its own. */
	LRESULT CALLBACK classProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		if ((msg == WM_SETCURSOR) && (LOWORD(lp) == HTCLIENT))
			return TRUE;
		if (msg == WM_CLOSE)
			return 0;
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
#endif

	/* Filled by the client-side autodetect callbacks: in the client role these are the ONLY
	 * place the server-measured RTT and bandwidth arrive (rdpAutoDetect's netChar* fields are
	 * filled in the server role only). */
	std::atomic<UINT32> g_rtt{ 0 };
	std::atomic<UINT32> g_base{ 0 };
	std::atomic<UINT32> g_bw{ 0 };
	std::atomic<long> g_resultCount{ 0 };
	std::atomic<long> g_syncCount{ 0 };

	BOOL netcharResult(rdpAutoDetect* ad, RDP_TRANSPORT_TYPE transport, UINT16 seq,
	                   const rdpNetworkCharacteristicsResult* r)
	{
		(void)ad;
		(void)transport;
		(void)seq;
		g_resultCount++;
		if (r)
		{
			if (r->averageRTT)
				g_rtt = r->averageRTT;
			if (r->baseRTT)
				g_base = r->baseRTT;
			if (r->bandwidth)
				g_bw = r->bandwidth;
		}
		return TRUE;
	}

	BOOL netcharSync(rdpAutoDetect* ad, RDP_TRANSPORT_TYPE transport, UINT16 seq, UINT32 bandwidth,
	                 UINT32 rtt)
	{
		(void)ad;
		(void)transport;
		(void)seq;
		g_syncCount++;
		if (rtt)
			g_rtt = rtt;
		if (bandwidth)
			g_bw = bandwidth;
		return TRUE;
	}

	void setTouchMode(bool touch)
	{
		g_touch = touch;
		/* Pen: read live by SDL - in mouse mode the pen is a mouse, in touch mode upstream's
		 * setting (pen arrives as touch). Touch: SDL never synthesises mouse input from fingers;
		 * in mouse mode Windows promotes them instead (see childProc). */
		(void)SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
		(void)SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, touch ? "0" : "1");
#if defined(_WIN32)
		/* The window's own thread only (RegisterTouchWindow refuses others): windowCreated and
		 * handleEvent both run on it. The flags are the ones SDL registers with. */
		if (g_child)
		{
			if (touch)
				(void)RegisterTouchWindow(g_child, TWF_FINETOUCH | TWF_WANTPALM);
			else
				(void)UnregisterTouchWindow(g_child);
		}
#endif
	}

	void post(Sint32 code)
	{
		SDL_Event ev = {};
		ev.type = g_eventType;
		ev.user.code = code;
		(void)SDL_PushEvent(&ev);
	}

#if defined(_WIN32)
	DWORD WINAPI pipeThread(LPVOID param)
	{
		auto cctx = static_cast<rdpClientContext*>(param);
		WCHAR name[64] = {};
		(void)_snwprintf_s(name, _countof(name), _TRUNCATE, L"\\\\.\\pipe\\finestrardp.%lu",
		                   static_cast<unsigned long>(GetCurrentProcessId()));
		HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
		                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096,
		                               0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE)
			return 0;
		if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
		{
			CloseHandle(pipe);
			return 0;
		}
		/* One STATS line a second (the host's liveness heartbeat); commands are polled every
		 * 100 ms so a toggle takes effect at once, and run one line at a time, in arrival order. */
		char pending[256] = {};
		size_t used = 0;
		for (unsigned tick = 0;; tick++)
		{
			if ((tick % 10) == 0)
			{
				char line[128] = {};
				const int n = snprintf(line, sizeof(line), "STATS %u %u %u %ld %ld %d %d\n",
				                       g_rtt.load(), g_bw.load(), g_base.load(), g_resultCount.load(),
				                       g_syncCount.load(), g_touch.load() ? 1 : 0,
				                       (cctx && cctx->rdpei) ? 1 : 0);
				DWORD written = 0;
				if (n <= 0 || !WriteFile(pipe, line, static_cast<DWORD>(n), &written, nullptr))
					break;
			}
			DWORD avail = 0;
			if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr))
				break;
			if (avail > 0)
			{
				DWORD rd = 0;
				const DWORD room = static_cast<DWORD>(sizeof(pending) - 1 - used);
				if (!ReadFile(pipe, pending + used, std::min<DWORD>(avail, room), &rd, nullptr))
					break;
				used += rd;
				pending[used] = '\0';
				char* start = pending;
				for (char* nl = strchr(start, '\n'); nl; nl = strchr(start, '\n'))
				{
					*nl = '\0';
					if (nl > start && nl[-1] == '\r')
						nl[-1] = '\0';
					if (strcmp(start, "PAUSE") == 0)
						post(CMD_PAUSE);
					else if (strcmp(start, "RESUME") == 0)
						post(CMD_RESUME);
					else if (strcmp(start, "TOUCH ON") == 0)
						post(CMD_TOUCH_ON);
					else if (strcmp(start, "TOUCH OFF") == 0)
						post(CMD_TOUCH_OFF);
					start = nl + 1;
				}
				used = strlen(start);
				memmove(pending, start, used + 1);
				if (used == sizeof(pending) - 1)
					used = 0; /* an over-long line is not a command: drop it */
			}
			Sleep(100);
		}
		CloseHandle(pipe);
		return 0;
	}
#endif
} // namespace

namespace finestra
{
	bool embedded(const rdpSettings* settings)
	{
#if defined(_WIN32)
		return settings && (freerdp_settings_get_uint64(settings, FreeRDP_ParentWindowId) != 0);
#else
		(void)settings;
		return false;
#endif
	}

	void prepareProcess(const rdpSettings* settings)
	{
#if defined(_M_ARM)
		/* Windows RT: D3D11 at feature level 9_1 draws black; D3D9 is the device-proven path.
		 * Set before SDL_Init, so no renderer in this process is ever created with D3D11. */
		if (!SDL_GetHint(SDL_HINT_RENDER_DRIVER))
			(void)SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d");
#endif
		g_embedProcess = embedded(settings);
		if (g_embedProcess)
		{
			/* Showing or raising the session window must not activate anything: the host's
			 * window is the active one. Alt+F4 is the session's, not a local close. */
			(void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
			(void)SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
			(void)SDL_SetHint(SDL_HINT_WINDOWS_CLOSE_ON_ALT_F4, "0");
		}
	}

	bool queryDisplay(SDL_DisplayID id, bool forceAsPrimary, rdpMonitor& monitor)
	{
		if (!g_embedProcess)
			return false;
		SDL_Rect bounds = {};
		if (!SDL_GetDisplayBounds(id, &bounds) || (bounds.w <= 0) || (bounds.h <= 0))
			return false; /* let the caller try its own way */
		int w = bounds.w;
		int h = bounds.h;
		const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
		if (mode && (mode->w > 0) && (mode->h > 0))
		{
			const float density = (mode->pixel_density > 0.0f) ? mode->pixel_density : 1.0f;
			w = static_cast<int>(std::lroundf(static_cast<float>(mode->w) * density));
			h = static_cast<int>(std::lroundf(static_cast<float>(mode->h) * density));
		}
		float scale = SDL_GetDisplayContentScale(id);
		if (scale <= 0.0f)
			scale = 1.0f;

		monitor = {};
		monitor.orig_screen = id;
		monitor.x = forceAsPrimary ? 0 : bounds.x;
		monitor.y = forceAsPrimary ? 0 : bounds.y;
		monitor.width = w;
		monitor.height = h;
		monitor.is_primary = forceAsPrimary || (id == SDL_GetPrimaryDisplay());
		monitor.attributes.desktopScaleFactor = static_cast<UINT32>(std::lroundf(scale * 100.0f));
		monitor.attributes.deviceScaleFactor = 100;
		monitor.attributes.orientation =
		    sdl::utils::orientaion_to_rdp(SDL_GetCurrentDisplayOrientation(id));
		monitor.attributes.physicalWidth = static_cast<UINT32>(w);
		monitor.attributes.physicalHeight = static_cast<UINT32>(h);
		WLog_DBG(TAG, "display %" PRIu32 ": %dx%d at %d,%d, scale %" PRIu32 "%%", id, w, h,
		         monitor.x, monitor.y, monitor.attributes.desktopScaleFactor);
		return true;
	}

	void prepareWindows(const rdpSettings* settings)
	{
#if defined(_WIN32)
		g_parent = nullptr;
		if (!embedded(settings))
			return;
		auto parent = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(
		    freerdp_settings_get_uint64(settings, FreeRDP_ParentWindowId)));
		if (!IsWindow(parent))
		{
			WLog_ERR(TAG, "/parent-window %p is not a window; running unembedded",
			         static_cast<void*>(parent));
			return;
		}
		g_parent = parent;
#else
		(void)settings;
#endif
	}

	void createProperties(SDL_PropertiesID props)
	{
#if defined(_WIN32)
		if (!g_parent || g_child)
			return; /* not embedding, or the one embedded window already exists */
		HINSTANCE inst = GetModuleHandleW(nullptr);
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		if (!GetClassInfoExW(inst, kChildClass, &wc))
		{
			wc = {};
			wc.cbSize = sizeof(wc);
			wc.style = CS_DBLCLKS;
			wc.lpfnWndProc = classProc; /* SDL sub-classes it */
			wc.hInstance = inst;
			wc.hCursor = nullptr; /* the cursor is the remote one, set by SDL */
			wc.lpszClassName = kChildClass;
			if (!RegisterClassExW(&wc))
			{
				WLog_ERR(TAG, "RegisterClassExW failed (%lu); running unembedded", GetLastError());
				return;
			}
		}
		RECT rc = {};
		(void)GetClientRect(g_parent, &rc);
		const int w = std::max<int>(1, rc.right - rc.left);
		const int h = std::max<int>(1, rc.bottom - rc.top);
		/* Created hidden: upstream's first draw shows it (SdlContext::drawToWindows). */
		g_child = CreateWindowExW(0, kChildClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0,
		                          0, w, h, g_parent, nullptr, inst, nullptr);
		if (!g_child)
		{
			WLog_ERR(TAG, "CreateWindowExW in host window %p failed (%lu); running unembedded",
			         static_cast<void*>(g_parent), GetLastError());
			return;
		}
		(void)SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, g_child);
		WLog_INFO(TAG, "session window %p created inside host window %p (%dx%d)",
		          static_cast<void*>(g_child), static_cast<void*>(g_parent), w, h);
#else
		(void)props;
#endif
	}

	void windowCreated(SDL_Window* window)
	{
#if defined(_WIN32)
		if (!window || !g_child || g_window)
			return;
		auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(
		    SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
		if (hwnd != g_child)
			return;
		g_window = window;
		/* On top of SDL's own sub-class (installed while SDL adopted the window). */
		g_sdlProc = reinterpret_cast<WNDPROC>(
		    SetWindowLongPtrW(g_child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(childProc)));
		setTouchMode(false); /* the host's default is mouse mode */
#else
		(void)window;
#endif
	}

	bool isEmbedded(SDL_Window* window)
	{
		return window && (window == g_window);
	}

	bool takeFirstDraw()
	{
		return !g_drawn.exchange(true);
	}

	void preConnect(rdpContext* context)
	{
		if (!context || !embedded(context->settings))
			return;
		rdpAutoDetect* ad = autodetect_get(context);
		if (ad)
		{
			ad->NetworkCharacteristicsResult = netcharResult;
			ad->NetworkCharacteristicsSync = netcharSync;
		}
	}

	void postConnect(rdpContext* context)
	{
#if defined(_WIN32)
		if (!context || !embedded(context->settings))
			return;
		g_context = context; /* the embedded window's keys go to the server from now on */
		/* The window was focused before the session existed: WM_SETFOCUS found no context, so
		 * sync the lock keys now. Called on the connection thread, so ask the window's own thread. */
		GUITHREADINFO gti = {};
		gti.cbSize = sizeof(gti);
		if (g_child && context->input &&
		    GetGUIThreadInfo(GetWindowThreadProcessId(g_child, nullptr), &gti) &&
		    (gti.hwndFocus == g_child))
			focusIn(context->input);
		if (g_eventType == 0)
		{
			const Uint32 type = SDL_RegisterEvents(1);
			if (type == 0)
			{
				WLog_ERR(TAG, "SDL_RegisterEvents failed; no stats/control pipe");
				return;
			}
			g_eventType = type;
		}
		/* rdpContext is the first member of rdpClientContext; the pipe reads its rdpei pointer. */
		HANDLE h = CreateThread(nullptr, 0, pipeThread, context, 0, nullptr);
		if (h)
			CloseHandle(h);
#else
		(void)context;
#endif
	}

	bool handleEvent(rdpContext* context, const SDL_Event& ev)
	{
		const Uint32 type = g_eventType;
		if ((type == 0) || (ev.type != type))
			return false;
		switch (ev.user.code)
		{
			case CMD_PAUSE:
			case CMD_RESUME:
				g_paused = (ev.user.code == CMD_PAUSE);
				if (context && context->gdi)
					(void)gdi_send_suppress_output(context->gdi, g_paused);
				break;
			case CMD_TOUCH_ON:
			case CMD_TOUCH_OFF:
				setTouchMode(ev.user.code == CMD_TOUCH_ON);
				WLog_INFO(TAG, "%s mode", touchMode() ? "touch" : "mouse");
				break;
			default:
				break;
		}
		return true;
	}

	bool touchMode()
	{
		return g_touch;
	}

	bool outputPaused()
	{
		return g_paused;
	}

	int exitCode(rdpContext* context, int sdlCode)
	{
		if (!context || !embedded(context->settings))
			return sdlCode;
		const UINT32 error = freerdp_get_last_error(context);
		if (error != FREERDP_ERROR_SUCCESS)
			return static_cast<int>(error);
		return (sdlCode < 0) ? sdlCode : 0;
	}
} // namespace finestra
