/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL client: the Windows clipboard, including file copy in both directions
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

#include <freerdp/freerdp.h>
#include <freerdp/client/cliprdr.h>

/* On Windows the clipboard channel is served by this module instead of sdlClip.
 *
 * WHY       SDL's Windows clipboard only knows text and images, and SDL notices a clipboard change
 *           only when one of its windows becomes the foreground window. An embedded session window
 *           is a child of another process's window, so it never is: copying on the device never
 *           reached the server, and files never crossed in either direction.
 * WHAT      the Windows client's clipboard design (client/Windows/wf_cliprdr.c), rewritten for this
 *           client: a thread of its own with a message-only window that listens for clipboard
 *           changes and owns everything this module puts on the clipboard.
 *           - device to server: every change is announced to the server; a copied file list
 *             (CF_HDROP) is announced as FileGroupDescriptorW, and the files' bytes are read from
 *             disk when the server asks for them.
 *           - server to device: formats are placed on the clipboard with delayed rendering and
 *             fetched from the server only when an application pastes. Copied files become an OLE
 *             data object offering FileGroupDescriptorW and FileContents, so pasting in Explorer
 *             downloads them through Explorer's own copy dialog.
 * THREADS   the channel thread never waits on the server: it posts to the clipboard thread and
 *           answers server requests from the local clipboard and the disk (reading the clipboard
 *           can wait on the application that owns it, as for any clipboard reader). Every request
 *           this module sends the server is sent and awaited on the clipboard thread, one at a time,
 *           with a timeout, and the wait also ends when the module stops or the connection is
 *           aborted.
 * TRUST     the server is not trusted: it is served only the formats announced to it, its format
 *           names cannot reach the standard formats, the NEW names it can add to the session's
 *           shared format table are capped per engine process, and its file names must be plain
 *           relative names. The user's own clipboard is only ever emptied by a server copy that
 *           replaces it, never by one that brings nothing this module can show.
 */
[[nodiscard]] bool sdl_wincliprdr_init(rdpContext* context, CliprdrClientContext* cliprdr);
[[nodiscard]] bool sdl_wincliprdr_uninit(CliprdrClientContext* cliprdr);
