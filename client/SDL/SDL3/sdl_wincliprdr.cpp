/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL client: the Windows clipboard, including file copy in both directions
 *
 * Copyright 2026 Hamed Ghorbani
 *
 * Based on the design of the Windows client's clipboard, client/Windows/wf_cliprdr.c:
 * Copyright 2012 Jason Champion
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2015 Thincast Technologies GmbH
 * Copyright 2015 DI (FH) Martin Haimberger <martin.haimberger@thincast.com>
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

#include "sdl_wincliprdr.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <winpr/assert.h>
#include <winpr/string.h>

#include <freerdp/log.h>
#include <freerdp/settings.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/utils/cliprdr_utils.h>

#include <ole2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>

#define TAG CLIENT_TAG("sdl.wincliprdr")

namespace
{
	constexpr UINT WM_CLIP_SERVER_FORMATS = WM_APP + 0x51; /* lParam: format list generation */
	constexpr UINT WM_CLIP_ANNOUNCE = WM_APP + 0x52;       /* send the local format list */
	constexpr UINT WM_CLIP_QUIT = WM_APP + 0x53;
	constexpr UINT WM_CLIP_SERVER_EMPTY = WM_APP + 0x54; /* lParam: format list generation */

	constexpr DWORD kFormatTimeoutMs = 15000;   /* one clipboard format from the server */
	constexpr DWORD kFileListTimeoutMs = 30000; /* the server's file list */
	constexpr DWORD kContentsTimeoutMs = 60000; /* one block of a file */
	constexpr DWORD kStopTimeoutMs = 10000;
	/* How long a local copy is left to settle before the server's request reads it. An app that
	 * copies often puts delayed formats up first and renders them a moment later (.NET's
	 * Clipboard.SetText does, retrying every 100 ms without answering messages). Reading in
	 * between would hold the clipboard open waiting on that app while it waits on us. */
	constexpr ULONGLONG kLocalSettleMs = 400;
	constexpr ULONG kReadBlock = 1024 * 1024;           /* bytes asked of the server per request */
	constexpr UINT32 kMaxServeBlock = 8 * 1024 * 1024;  /* most read from disk for one server request */
	constexpr size_t kMaxFiles = 100000;
	constexpr int kMaxDepth = 64;
	/* Registered clipboard format names come from one table shared by every program in the session
	 * (with window classes and window messages, about 16k entries), and they are never freed until
	 * logoff. A server picks the names, so it must not be able to fill that table. */
	constexpr size_t kMaxNamedFormatsPerList = 64;
	constexpr size_t kMaxNamedFormats = 256; /* NEW names one engine process may add to the table */

	/* The server format names this process knows, shared by every connection it makes: a name it
	 * registered stays in the table after the connection ends. Names already in the table (read
	 * once, see registerServerName) cost nothing and are not counted. */
	struct NameTable
	{
		std::mutex lock;
		bool scanned = false;
		std::unordered_map<std::wstring, UINT> ids; /* key: atomKey(name) */
		size_t added = 0;
	};

	NameTable& nameTable()
	{
		static NameTable table;
		return table;
	}

	/* The table compares names ignoring the case of ASCII letters only. */
	std::wstring atomKey(const std::wstring& name)
	{
		std::wstring key(name);
		for (auto& c : key)
		{
			if ((c >= L'a') && (c <= L'z'))
				c = static_cast<wchar_t>(c - L'a' + L'A');
		}
		return key;
	}

	const wchar_t kFileGroupName[] = L"FileGroupDescriptorW";
	const wchar_t kFileContentsName[] = L"FileContents";
	const wchar_t kDropEffectName[] = L"Preferred DropEffect";

	/* Standard formats whose clipboard data is a plain memory block, so the bytes can travel as they
	 * are. The others are GDI or private handles (CF_BITMAP, CF_ENHMETAFILE, CF_PALETTE, ...), which
	 * mean nothing in another process; Windows synthesizes CF_BITMAP from CF_DIB on its own. */
	bool standardFormatTransferable(UINT32 id)
	{
		switch (id)
		{
			case CF_TEXT:
			case CF_OEMTEXT:
			case CF_UNICODETEXT:
			case CF_LOCALE:
			case CF_DIB:
			case CF_DIBV5:
			case CF_SYLK:
			case CF_DIF:
			case CF_TIFF:
			case CF_RIFF:
			case CF_WAVE:
				return true;
			default:
				return false;
		}
	}

	bool isTextFormat(UINT32 id)
	{
		return (id == CF_TEXT) || (id == CF_OEMTEXT) || (id == CF_UNICODETEXT);
	}

	/* Registered formats that are never mirrored: file transfer travels through FileGroupDescriptorW
	 * only, and the OLE and shell bookkeeping formats describe objects inside one process. */
	bool deniedFormatName(const std::wstring& name)
	{
		static const wchar_t* const denied[] = {
			L"FileContents",         L"FileGroupDescriptor",     L"FileGroupDescriptorW",
			L"FileName",             L"FileNameW",               L"FileNameMap",
			L"FileNameMapW",         L"Shell IDList Array",      L"Preferred DropEffect",
			L"Performed DropEffect", L"Paste Succeeded",         L"Logical Performed DropEffect",
			L"Shell Object Offsets", L"DataObject",              L"Ole Private Data",
			L"Object Descriptor",    L"Link Source Descriptor",  L"Embed Source",
			L"Link Source",          L"Native",                  L"OwnerLink",
			L"ObjectLink",           L"OleClipboardPersistOnFlush"
		};
		for (const auto* d : denied)
		{
			if (_wcsicmp(name.c_str(), d) == 0)
				return true;
		}
		return false;
	}

	/* CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$ and CONOUT$ (also followed by an extension, and
	 * COM and LPT also with a superscript digit) open a device instead of a file. */
	bool reservedDeviceName(const wchar_t* part, size_t plen)
	{
		size_t base = 0;
		while ((base < plen) && (part[base] != L'.'))
			base++;
		while ((base > 0) && (part[base - 1] == L' '))
			base--;
		if (base == 3)
		{
			for (const wchar_t* n : { L"CON", L"PRN", L"AUX", L"NUL" })
			{
				if (_wcsnicmp(part, n, 3) == 0)
					return true;
			}
		}
		if ((base == 4) && ((_wcsnicmp(part, L"COM", 3) == 0) || (_wcsnicmp(part, L"LPT", 3) == 0)))
		{
			const wchar_t d = part[3];
			return ((d >= L'0') && (d <= L'9')) || (d == 0x00B9) || (d == 0x00B2) || (d == 0x00B3);
		}
		return ((base == 6) && (_wcsnicmp(part, L"CONIN$", 6) == 0)) ||
		       ((base == 7) && (_wcsnicmp(part, L"CONOUT$", 7) == 0));
	}

	/* A name the server sent in its file list becomes a path below the folder the user pastes into.
	 * Accept relative names only: no drive, no root, no '..' or '.' component, no stream (':'), no
	 * control characters, no device name, and no component ending in a dot or a space (Windows
	 * strips those, so the name on disk would not be the name that was checked). */
	bool safeRelativeName(WCHAR* name, size_t capacity)
	{
		name[capacity - 1] = L'\0';
		const size_t len = wcsnlen(name, capacity);
		if (len == 0)
			return false;
		for (size_t x = 0; x < len; x++)
		{
			if (name[x] == L'/')
				name[x] = L'\\';
			if ((name[x] < 0x20) || (name[x] == L':') || (name[x] == L'*') || (name[x] == L'?') ||
			    (name[x] == L'"') || (name[x] == L'<') || (name[x] == L'>') || (name[x] == L'|'))
				return false;
		}
		if (name[0] == L'\\')
			return false;
		const wchar_t* part = name;
		while (part)
		{
			const wchar_t* end = wcschr(part, L'\\');
			const size_t plen = end ? static_cast<size_t>(end - part) : wcslen(part);
			/* also covers "." and ".." */
			if ((plen == 0) || (part[plen - 1] == L'.') || (part[plen - 1] == L' ') ||
			    reservedDeviceName(part, plen))
				return false;
			part = end ? end + 1 : nullptr;
		}
		return true;
	}

	std::wstring baseName(const WCHAR* path)
	{
		const wchar_t* slash = wcsrchr(path, L'\\');
		return slash ? std::wstring(slash + 1) : std::wstring(path);
	}

	/* Waits up to ms while still answering messages other threads SEND to this one. A reader that
	 * holds the clipboard open may be waiting on our WM_RENDERFORMAT; sleeping through it would
	 * keep the clipboard locked for both of us. Posted messages stay queued. */
	void sleepAnsweringSent(DWORD ms)
	{
		(void)MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_SENDMESSAGE, 0);
		MSG msg = {};
		(void)PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
	}

	/* Another process often has the clipboard open for a moment (clipboard history, a viewer, the
	 * app that just copied), so keep trying for about a second. */
	constexpr ULONGLONG kClipboardBusyMs = 1000;

	bool openClipboardRetry(HWND owner)
	{
		const ULONGLONG start = GetTickCount64();
		for (;;)
		{
			if (OpenClipboard(owner))
				return true;
			if (GetTickCount64() - start >= kClipboardBusyMs)
				return false;
			sleepAnsweringSent(15);
		}
	}

	HRESULT oleSetClipboardRetry(IDataObject* obj)
	{
		const ULONGLONG start = GetTickCount64();
		for (;;)
		{
			const HRESULT hr = OleSetClipboard(obj);
			if ((hr != CLIPBRD_E_CANT_OPEN) || (GetTickCount64() - start >= kClipboardBusyMs))
				return hr;
			sleepAnsweringSent(15);
		}
	}

	struct Mapping
	{
		UINT32 remoteId = 0;
		UINT localId = 0;
	};

	struct LocalFile
	{
		std::wstring path;
		FILEDESCRIPTORW fd = {};
	};
} // namespace

class WinClip;

/** One file the server offered, read on demand. Explorer reads it through COM, on the clipboard
 *  thread; every block is one FILECONTENTS_RANGE request. */
class ServerFileStream final : public IStream
{
  public:
	ServerFileStream(WinClip* clip, UINT32 generation, UINT32 index, UINT64 size, std::wstring name)
	    : _clip(clip), _gen(generation), _index(index), _size(size), _name(std::move(name))
	{
	}

	ServerFileStream(const ServerFileStream&) = delete;
	ServerFileStream& operator=(const ServerFileStream&) = delete;

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(ISequentialStream)) ||
		    IsEqualIID(riid, __uuidof(IStream)))
		{
			*ppv = static_cast<IStream*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override
	{
		return static_cast<ULONG>(InterlockedIncrement(&_ref));
	}
	STDMETHODIMP_(ULONG) Release() override
	{
		const LONG ref = InterlockedDecrement(&_ref);
		if (ref == 0)
			delete this;
		return static_cast<ULONG>(ref);
	}

	/* COM methods must not throw: these wrap the parts that allocate */
	STDMETHODIMP Read(void* pv, ULONG cb, ULONG* pcbRead) override
	{
		try
		{
			return read(pv, cb, pcbRead);
		}
		catch (const std::exception&)
		{
			return E_OUTOFMEMORY;
		}
	}
	HRESULT read(void* pv, ULONG cb, ULONG* pcbRead);
	STDMETHODIMP Write(const void*, ULONG, ULONG*) override
	{
		return STG_E_ACCESSDENIED;
	}
	STDMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPosition) override
	{
		LONGLONG base = 0;
		switch (origin)
		{
			case STREAM_SEEK_SET:
				base = 0;
				break;
			case STREAM_SEEK_CUR:
				base = static_cast<LONGLONG>(_pos);
				break;
			case STREAM_SEEK_END:
				base = static_cast<LONGLONG>(_size);
				break;
			default:
				return STG_E_INVALIDFUNCTION;
		}
		const LONGLONG target = base + move.QuadPart;
		if (target < 0)
			return STG_E_INVALIDFUNCTION;
		/* A read-only stream may be positioned at or past its end: reads there return nothing. */
		_pos = static_cast<UINT64>(target);
		if (newPosition)
			newPosition->QuadPart = _pos;
		return S_OK;
	}
	STDMETHODIMP SetSize(ULARGE_INTEGER) override
	{
		return E_NOTIMPL;
	}
	STDMETHODIMP CopyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead,
	                    ULARGE_INTEGER* pcbWritten) override
	{
		try
		{
			return copyTo(pstm, cb, pcbRead, pcbWritten);
		}
		catch (const std::exception&)
		{
			return E_OUTOFMEMORY;
		}
	}
	HRESULT copyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead,
	               ULARGE_INTEGER* pcbWritten);
	STDMETHODIMP Commit(DWORD) override
	{
		return S_OK;
	}
	STDMETHODIMP Revert() override
	{
		return E_NOTIMPL;
	}
	STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
	{
		return STG_E_INVALIDFUNCTION;
	}
	STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override
	{
		return STG_E_INVALIDFUNCTION;
	}
	STDMETHODIMP Stat(STATSTG* pst, DWORD flags) override
	{
		if (!pst)
			return STG_E_INVALIDPOINTER;
		ZeroMemory(pst, sizeof(*pst));
		pst->type = STGTY_STREAM;
		pst->cbSize.QuadPart = _size;
		pst->grfMode = STGM_READ;
		if ((flags & STATFLAG_NONAME) == 0)
		{
			const size_t bytes = (_name.size() + 1) * sizeof(WCHAR);
			pst->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
			if (!pst->pwcsName)
				return STG_E_INSUFFICIENTMEMORY;
			memcpy(pst->pwcsName, _name.c_str(), bytes);
		}
		return S_OK;
	}
	STDMETHODIMP Clone(IStream** ppstm) override
	{
		if (!ppstm)
			return STG_E_INVALIDPOINTER;
		try /* the name is copied too: COM methods must not throw */
		{
			auto copy = new ServerFileStream(_clip, _gen, _index, _size, _name);
			copy->_pos = _pos;
			*ppstm = copy;
			return S_OK;
		}
		catch (const std::exception&)
		{
			*ppstm = nullptr;
			return E_OUTOFMEMORY;
		}
	}

  private:
	~ServerFileStream() = default;

	LONG _ref = 1;
	WinClip* _clip;
	UINT32 _gen;
	UINT32 _index;
	UINT64 _size;
	std::wstring _name;
	UINT64 _pos = 0;
	std::vector<BYTE> _block; /* the last block the server sent, starting at _blockPos */
	UINT64 _blockPos = 0;
};

/** The server's copied files, as Explorer sees them: FileGroupDescriptorW (the list), FileContents
 *  (one stream per file) and Preferred DropEffect (copy). Nothing is fetched until it is asked for. */
class FileDataObject final : public IDataObject, public IDataObjectAsyncCapability
{
  public:
	FileDataObject(WinClip* clip, UINT32 generation, UINT32 fileGroupRemoteId)
	    : _clip(clip), _gen(generation), _fileGroupRemoteId(fileGroupRemoteId),
	      _cfDescriptor(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(kFileGroupName))),
	      _cfContents(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(kFileContentsName))),
	      _cfDropEffect(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(kDropEffectName)))
	{
	}

	FileDataObject(const FileDataObject&) = delete;
	FileDataObject& operator=(const FileDataObject&) = delete;

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDataObject)))
			*ppv = static_cast<IDataObject*>(this);
		else if (IsEqualIID(riid, __uuidof(IDataObjectAsyncCapability)))
			*ppv = static_cast<IDataObjectAsyncCapability*>(this);
		else
		{
			*ppv = nullptr;
			return E_NOINTERFACE;
		}
		AddRef();
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef() override
	{
		return static_cast<ULONG>(InterlockedIncrement(&_ref));
	}
	STDMETHODIMP_(ULONG) Release() override
	{
		const LONG ref = InterlockedDecrement(&_ref);
		if (ref == 0)
			delete this;
		return static_cast<ULONG>(ref);
	}

	/* IDataObject */
	STDMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override
	{
		try
		{
			return getData(format, medium);
		}
		catch (const std::exception&) /* COM methods must not throw */
		{
			return E_OUTOFMEMORY;
		}
	}
	HRESULT getData(FORMATETC* format, STGMEDIUM* medium);
	STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override
	{
		return E_NOTIMPL;
	}
	STDMETHODIMP QueryGetData(FORMATETC* format) override
	{
		if (!format)
			return E_INVALIDARG;
		if (((format->cfFormat == _cfDescriptor) || (format->cfFormat == _cfDropEffect)) &&
		    (format->tymed & TYMED_HGLOBAL))
			return S_OK;
		if ((format->cfFormat == _cfContents) && (format->tymed & TYMED_ISTREAM))
			return S_OK;
		return DV_E_FORMATETC;
	}
	STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override
	{
		if (!in || !out)
			return E_INVALIDARG;
		*out = *in;
		out->ptd = nullptr;
		return DATA_S_SAMEFORMATETC;
	}
	STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override
	{
		return E_NOTIMPL;
	}
	STDMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** ppenum) override
	{
		if (!ppenum)
			return E_INVALIDARG;
		if (direction != DATADIR_GET)
			return E_NOTIMPL;
		FORMATETC formats[3] = {
			{ _cfDescriptor, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
			{ _cfContents, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM },
			{ _cfDropEffect, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL },
		};
		return SHCreateStdEnumFmtEtc(ARRAYSIZE(formats), formats, ppenum);
	}
	STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
	{
		return OLE_E_ADVISENOTSUPPORTED;
	}
	STDMETHODIMP DUnadvise(DWORD) override
	{
		return OLE_E_ADVISENOTSUPPORTED;
	}
	STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override
	{
		return OLE_E_ADVISENOTSUPPORTED;
	}

	/* IDataObjectAsyncCapability: lets Explorer copy on a thread of its own, so its window stays
	 * responsive during a long download. */
	STDMETHODIMP SetAsyncMode(BOOL doOpAsync) override
	{
		_async = doOpAsync;
		return S_OK;
	}
	STDMETHODIMP GetAsyncMode(BOOL* isOpAsync) override
	{
		if (!isOpAsync)
			return E_INVALIDARG;
		*isOpAsync = _async;
		return S_OK;
	}
	STDMETHODIMP StartOperation(IBindCtx*) override
	{
		_inOperation = TRUE;
		return S_OK;
	}
	STDMETHODIMP InOperation(BOOL* inAsyncOp) override
	{
		if (!inAsyncOp)
			return E_INVALIDARG;
		*inAsyncOp = _inOperation;
		return S_OK;
	}
	STDMETHODIMP EndOperation(HRESULT, IBindCtx*, DWORD) override
	{
		_inOperation = FALSE;
		return S_OK;
	}

  private:
	~FileDataObject() = default;
	bool ensureFiles();

	LONG _ref = 1;
	WinClip* _clip;
	UINT32 _gen;
	UINT32 _fileGroupRemoteId;
	CLIPFORMAT _cfDescriptor;
	CLIPFORMAT _cfContents;
	CLIPFORMAT _cfDropEffect;
	bool _loaded = false;
	std::vector<FILEDESCRIPTORW> _files;
	BOOL _async = TRUE;
	BOOL _inOperation = FALSE;
};

class WinClip
{
  public:
	WinClip(rdpContext* rdp, CliprdrClientContext* ctx)
	    : _rdp(rdp), _ctx(ctx), _log(WLog_Get(TAG)),
	      _cfFileGroup(RegisterClipboardFormatW(kFileGroupName)),
	      _cfFileContents(RegisterClipboardFormatW(kFileContentsName))
	{
	}

	~WinClip()
	{
		for (HANDLE h : { _ready, _stopEvent, _formatEvent, _contentsEvent })
		{
			if (h)
				(void)CloseHandle(h);
		}
	}

	WinClip(const WinClip&) = delete;
	WinClip& operator=(const WinClip&) = delete;

	[[nodiscard]] bool start();
	void stop();
	[[nodiscard]] bool leaked() const
	{
		return _leaked;
	}

	[[nodiscard]] UINT32 generation() const
	{
		return _generation.load();
	}
	[[nodiscard]] wLog* log() const
	{
		return _log;
	}

	/* clipboard thread: the server's file list for a format list generation */
	[[nodiscard]] bool fetchFileList(UINT32 gen, UINT32 remoteFormatId,
	                                 std::vector<FILEDESCRIPTORW>& files);
	/* clipboard thread: FILECONTENTS_SIZE or FILECONTENTS_RANGE for one file */
	[[nodiscard]] bool fetchFileContents(UINT32 gen, UINT32 index, UINT32 flags, UINT64 position,
	                                     UINT32 cb, std::vector<BYTE>& out);

	/* channel callbacks */
	[[nodiscard]] UINT onMonitorReady();
	[[nodiscard]] UINT onServerCapabilities(const CLIPRDR_CAPABILITIES* caps);
	[[nodiscard]] UINT onServerFormatList(const CLIPRDR_FORMAT_LIST* list);
	[[nodiscard]] UINT onServerFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST* request);
	[[nodiscard]] UINT onServerFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE* response);
	[[nodiscard]] UINT onServerFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST* request);
	[[nodiscard]] UINT onServerFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE* response);

  private:
	static DWORD WINAPI threadMain(LPVOID arg);
	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	DWORD run();
	LRESULT handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	/* clipboard thread */
	void applyServerFormats(UINT32 gen);
	void releaseServerData(UINT32 gen);
	void dropServerData();
	void forgetLocal();
	void announceLocal();
	void renderFormat(UINT localId);
	void shutdownOnThread();
	void releaseDataObject();
	[[nodiscard]] bool requestServerFormat(UINT32 gen, UINT32 remoteId, DWORD timeoutMs,
	                                       std::vector<BYTE>& out);
	[[nodiscard]] bool waitFor(HANDLE event, DWORD timeoutMs) const;

	/* channel thread */
	[[nodiscard]] UINT registerServerName(const std::wstring& name);
	[[nodiscard]] bool clipboardHoldsServerData() const;
	[[nodiscard]] bool readLocalFormat(UINT format, std::vector<BYTE>& out) const;
	[[nodiscard]] bool buildLocalFileList(std::vector<BYTE>& out);
	[[nodiscard]] bool addLocalEntry(const std::wstring& path, const std::wstring& relative,
	                                 int depth, std::vector<LocalFile>& files) const;
	[[nodiscard]] static bool readLocalFile(const std::wstring& path, UINT64 position, UINT32 cb,
	                                        std::vector<BYTE>& out);
	[[nodiscard]] UINT sendCapabilities();
	[[nodiscard]] UINT sendFormatListResponse(bool ok);
	[[nodiscard]] UINT sendFormatDataResponse(bool ok, const std::vector<BYTE>& data);
	[[nodiscard]] UINT sendFileContentsResponse(UINT32 streamId, bool ok,
	                                            const std::vector<BYTE>& data);

	rdpContext* _rdp;
	CliprdrClientContext* _ctx;
	wLog* _log;
	const UINT _cfFileGroup;
	const UINT _cfFileContents;

	HANDLE _thread = nullptr;
	DWORD _threadId = 0;
	std::atomic<HWND> _hwnd = nullptr;
	HANDLE _ready = nullptr;
	HANDLE _stopEvent = nullptr;
	bool _leaked = false;
	std::atomic<bool> _sync = false;
	std::atomic<UINT32> _serverCaps = 0;
	std::atomic<ULONGLONG> _localChangeTick = 0; /* GetTickCount64() of the last local copy */

	/* the latest server format list (guarded by _mapLock) */
	std::mutex _mapLock;
	std::vector<Mapping> _map;
	UINT32 _fileGroupRemoteId = 0;
	std::atomic<UINT32> _generation = 0;

	/* clipboard thread only */
	IDataObject* _dataObject = nullptr;

	/* the outstanding format data request: format data responses carry no id, so they are matched
	 * by count (guarded by _formatLock) */
	std::mutex _formatLock;
	HANDLE _formatEvent = nullptr;
	UINT64 _formatSent = 0;
	UINT64 _formatReceived = 0;
	UINT64 _formatAwaited = 0;
	bool _formatOk = false;
	std::vector<BYTE> _formatData;

	/* the outstanding file contents request, matched by stream id (guarded by _contentsLock) */
	std::mutex _contentsLock;
	HANDLE _contentsEvent = nullptr;
	std::atomic<UINT32> _nextStreamId = 0;
	UINT32 _contentsAwaited = 0;
	UINT32 _contentsRequested = 0; /* bytes asked for: a longer answer is cut to this */
	bool _contentsOk = false;
	std::vector<BYTE> _contentsData;

	/* what was last announced to the server (guarded by _localLock): the format ids it may ask for,
	 * and the files behind FileGroupDescriptorW once it asked for the list */
	std::mutex _localLock;
	std::vector<UINT32> _offered;
	std::vector<LocalFile> _localFiles;
	UINT64 _localEpoch = 0; /* bumped whenever the two above are reset */
};

/* ---------------------------------------------------------------------------------------------- */

HRESULT ServerFileStream::read(void* pv, ULONG cb, ULONG* pcbRead)
{
	if (!pv)
		return STG_E_INVALIDPOINTER;

	auto out = static_cast<BYTE*>(pv);
	ULONG total = 0;
	while ((total < cb) && (_pos < _size))
	{
		if (!_block.empty() && (_pos >= _blockPos) && (_pos < _blockPos + _block.size()))
		{
			const size_t offset = static_cast<size_t>(_pos - _blockPos);
			const size_t n = std::min<size_t>(_block.size() - offset, cb - total);
			memcpy(out + total, _block.data() + offset, n);
			total += static_cast<ULONG>(n);
			_pos += n;
			continue;
		}

		/* Ask for a block at least kReadBlock long: Explorer reads in small pieces, and every
		 * request is a round trip to the server. */
		const UINT64 remaining = _size - _pos;
		const UINT64 wanted = std::max<UINT64>(cb - total, kReadBlock);
		const UINT32 request = static_cast<UINT32>(std::min<UINT64>(wanted, remaining));
		std::vector<BYTE> data;
		if (!_clip->fetchFileContents(_gen, _index, FILECONTENTS_RANGE, _pos, request, data))
		{
			if (total > 0)
				break;
			if (pcbRead)
				*pcbRead = 0;
			return E_FAIL;
		}
		if (data.empty())
			break; /* the server has no more: report what was read */
		if (data.size() > request)
			data.resize(request);
		_block.swap(data);
		_blockPos = _pos;
	}

	if (pcbRead)
		*pcbRead = total;
	return S_OK;
}

HRESULT ServerFileStream::copyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead,
                                 ULARGE_INTEGER* pcbWritten)
{
	if (!pstm)
		return STG_E_INVALIDPOINTER;

	std::vector<BYTE> buffer(kReadBlock);
	UINT64 left = cb.QuadPart;
	UINT64 readTotal = 0;
	UINT64 writtenTotal = 0;
	HRESULT hr = S_OK;
	while (left > 0)
	{
		ULONG got = 0;
		const ULONG want = static_cast<ULONG>(std::min<UINT64>(left, buffer.size()));
		hr = Read(buffer.data(), want, &got);
		if (FAILED(hr) || (got == 0))
			break;
		readTotal += got;
		ULONG written = 0;
		hr = pstm->Write(buffer.data(), got, &written);
		writtenTotal += written;
		if (FAILED(hr))
			break;
		if (written != got)
		{
			hr = STG_E_MEDIUMFULL;
			break;
		}
		left -= got;
	}
	if (pcbRead)
		pcbRead->QuadPart = readTotal;
	if (pcbWritten)
		pcbWritten->QuadPart = writtenTotal;
	return FAILED(hr) ? hr : S_OK;
}

bool FileDataObject::ensureFiles()
{
	if (_clip->generation() != _gen)
		return false; /* the server clipboard changed since this object was published */
	if (_loaded)
		return true;
	if (!_clip->fetchFileList(_gen, _fileGroupRemoteId, _files))
		return false;
	_loaded = true;
	return true;
}

HRESULT FileDataObject::getData(FORMATETC* format, STGMEDIUM* medium)
{
	if (!format || !medium)
		return E_INVALIDARG;
	ZeroMemory(medium, sizeof(*medium));

	if ((format->cfFormat == _cfDropEffect) && (format->tymed & TYMED_HGLOBAL))
	{
		HGLOBAL h = GlobalAlloc(GHND, sizeof(DWORD));
		if (!h)
			return E_OUTOFMEMORY;
		auto effect = static_cast<DWORD*>(GlobalLock(h));
		if (effect)
			*effect = DROPEFFECT_COPY;
		GlobalUnlock(h);
		medium->tymed = TYMED_HGLOBAL;
		medium->u.hGlobal = h;
		return S_OK;
	}

	if ((format->cfFormat == _cfDescriptor) && (format->tymed & TYMED_HGLOBAL))
	{
		if (!ensureFiles())
			return E_FAIL;
		/* A new block for every call: the caller owns and frees what it receives. */
		const size_t bytes =
		    offsetof(FILEGROUPDESCRIPTORW, fgd) + _files.size() * sizeof(FILEDESCRIPTORW);
		HGLOBAL h = GlobalAlloc(GHND, bytes);
		if (!h)
			return E_OUTOFMEMORY;
		auto group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(h));
		if (!group)
		{
			GlobalFree(h);
			return E_OUTOFMEMORY;
		}
		group->cItems = static_cast<UINT>(_files.size());
		if (!_files.empty())
			memcpy(group->fgd, _files.data(), _files.size() * sizeof(FILEDESCRIPTORW));
		GlobalUnlock(h);
		medium->tymed = TYMED_HGLOBAL;
		medium->u.hGlobal = h;
		return S_OK;
	}

	if ((format->cfFormat == _cfContents) && (format->tymed & TYMED_ISTREAM))
	{
		if (!ensureFiles())
			return E_FAIL;
		if ((format->lindex < 0) || (static_cast<size_t>(format->lindex) >= _files.size()))
			return DV_E_LINDEX;
		const auto index = static_cast<UINT32>(format->lindex);
		const FILEDESCRIPTORW& fd = _files[index];
		if ((fd.dwFlags & FD_ATTRIBUTES) && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			return DV_E_LINDEX;

		UINT64 size = 0;
		if (fd.dwFlags & FD_FILESIZE)
			size = (static_cast<UINT64>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
		else
		{
			std::vector<BYTE> data;
			if (!_clip->fetchFileContents(_gen, index, FILECONTENTS_SIZE, 0, 8, data) ||
			    (data.size() < sizeof(UINT64)))
				return E_FAIL;
			memcpy(&size, data.data(), sizeof(UINT64));
		}

		/* A new stream for every call, so two readers never share a position. */
		medium->tymed = TYMED_ISTREAM;
		medium->u.pstm = new ServerFileStream(_clip, _gen, index, size, baseName(fd.cFileName));
		return S_OK;
	}

	return DV_E_FORMATETC;
}

/* ---------------------------------------------------------------------------------------------- */

bool WinClip::start()
{
	_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	_formatEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	_contentsEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!_ready || !_stopEvent || !_formatEvent || !_contentsEvent)
		return false;

	_thread = CreateThread(nullptr, 0, WinClip::threadMain, this, 0, nullptr);
	if (!_thread)
		return false;

	const HANDLE wait[2] = { _ready, _thread };
	(void)WaitForMultipleObjects(ARRAYSIZE(wait), wait, FALSE, kStopTimeoutMs);
	if (!_hwnd.load())
	{
		WLog_Print(_log, WLOG_ERROR, "clipboard thread failed to start");
		return false;
	}
	return true;
}

void WinClip::stop()
{
	_sync = false;
	if (_stopEvent)
		(void)SetEvent(_stopEvent); /* ends any wait for the server */
	const HWND hwnd = _hwnd.load();
	if (hwnd)
		(void)PostMessageW(hwnd, WM_CLIP_QUIT, 0, 0);
	if (_thread)
	{
		if (WaitForSingleObject(_thread, kStopTimeoutMs) != WAIT_OBJECT_0)
		{
			/* Never free what a running thread still uses: leak it instead. */
			WLog_Print(_log, WLOG_ERROR, "clipboard thread did not stop, leaving it");
			_leaked = true;
		}
		(void)CloseHandle(_thread);
		_thread = nullptr;
	}
}

DWORD WINAPI WinClip::threadMain(LPVOID arg)
{
	auto self = static_cast<WinClip*>(arg);
	WINPR_ASSERT(self);
	return self->run();
}

DWORD WinClip::run()
{
	const HRESULT ole = OleInitialize(nullptr);

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WinClip::wndProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"FreeRDP_SDL_Cliprdr";
	(void)RegisterClassExW(&wc); /* fails harmlessly once the class exists (a reconnect) */

	_threadId = GetCurrentThreadId();
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"rdpclip", 0, 0, 0, 0, 0, HWND_MESSAGE,
	                            nullptr, wc.hInstance, this);
	if (!hwnd || !AddClipboardFormatListener(hwnd))
	{
		WLog_Print(_log, WLOG_ERROR, "clipboard window: %s failed [%" PRIu32 "]",
		           hwnd ? "AddClipboardFormatListener" : "CreateWindowEx",
		           static_cast<UINT32>(GetLastError()));
		if (hwnd)
			(void)DestroyWindow(hwnd);
		(void)SetEvent(_ready);
		if (SUCCEEDED(ole))
			OleUninitialize();
		return 1;
	}
	_hwnd = hwnd;
	(void)SetEvent(_ready);

	MSG msg = {};
	while (GetMessageW(&msg, nullptr, 0, 0) > 0)
	{
		(void)TranslateMessage(&msg);
		(void)DispatchMessageW(&msg);
	}

	/* Also disconnects every COM proxy Explorer may still hold: our objects are released here,
	 * on this thread, before the module is freed. */
	if (SUCCEEDED(ole))
		OleUninitialize();
	return 0;
}

LRESULT CALLBACK WinClip::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_NCCREATE)
	{
		auto cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		(void)SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	auto self = reinterpret_cast<WinClip*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (!self)
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	/* An exception must not unwind through the window manager: out of memory fails the message. */
	try
	{
		return self->handle(hwnd, msg, wParam, lParam);
	}
	catch (const std::exception&)
	{
		WLog_Print(self->log(), WLOG_ERROR, "clipboard message %" PRIu32 " failed: out of memory",
		           static_cast<UINT32>(msg));
		return 0;
	}
}

LRESULT WinClip::handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_CLIPBOARDUPDATE:
			if (!_sync)
				return 0;
			/* Our own changes: delayed server formats (owned by this window) or the file object
			 * (owned by OLE's window, current while it is ours). */
			if (GetClipboardOwner() == hwnd)
				return 0;
			if (_dataObject && (OleIsCurrentClipboard(_dataObject) == S_OK))
				return 0;
			_localChangeTick = GetTickCount64();
			announceLocal();
			return 0;

		case WM_RENDERFORMAT:
			renderFormat(static_cast<UINT>(wParam));
			return 0;

		case WM_RENDERALLFORMATS:
			/* This window is going away while it still owns delayed formats: they cannot be
			 * rendered without the session, so give the clipboard up. */
			if (OpenClipboard(hwnd))
			{
				if (GetClipboardOwner() == hwnd)
					(void)EmptyClipboard();
				(void)CloseClipboard();
			}
			return 0;

		case WM_CLIP_SERVER_FORMATS:
			applyServerFormats(static_cast<UINT32>(lParam));
			return 0;

		case WM_CLIP_SERVER_EMPTY:
			releaseServerData(static_cast<UINT32>(lParam));
			return 0;

		case WM_CLIP_ANNOUNCE:
			announceLocal();
			return 0;

		case WM_CLIP_QUIT:
			shutdownOnThread();
			return 0;

		default:
			return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

void WinClip::shutdownOnThread()
{
	const HWND hwnd = _hwnd.load();
	dropServerData();
	if (_dataObject && (OleIsCurrentClipboard(_dataObject) == S_OK))
		WLog_Print(_log, WLOG_WARN, "the server's files are still on the clipboard at shutdown");
	releaseDataObject();
	if (hwnd)
	{
		(void)RemoveClipboardFormatListener(hwnd);
		_hwnd = nullptr;
		(void)DestroyWindow(hwnd);
	}
	PostQuitMessage(0);
}

void WinClip::releaseDataObject()
{
	if (_dataObject)
	{
		_dataObject->Release();
		_dataObject = nullptr;
	}
}

void WinClip::applyServerFormats(UINT32 gen)
{
	std::vector<UINT> locals;
	UINT32 fileGroup = 0;
	{
		/* checked in the same hold as the copy, so the copy is of this generation's list */
		std::scoped_lock lock(_mapLock);
		if (gen != _generation.load())
			return; /* a newer list is already queued */
		fileGroup = _fileGroupRemoteId;
		for (const auto& m : _map)
			locals.push_back(m.localId);
	}
	if (locals.empty() && (fileGroup == 0))
	{
		dropServerData(); /* never empty the user's own clipboard for nothing */
		return;
	}

	const HWND hwnd = _hwnd.load();
	if (fileGroup != 0)
	{
		auto obj = new FileDataObject(this, gen, fileGroup);
		const HRESULT hr = oleSetClipboardRetry(obj);
		if (FAILED(hr))
		{
			/* the previous object, if it is still there, stays known so it can be taken back */
			WLog_Print(_log, WLOG_WARN, "OleSetClipboard failed [0x%08" PRIX32 "]",
			           static_cast<UINT32>(hr));
			obj->Release();
			return;
		}
		releaseDataObject(); /* the one it replaced */
		_dataObject = obj;
		forgetLocal();
		return;
	}

	if (!openClipboardRetry(hwnd))
	{
		WLog_Print(_log, WLOG_WARN, "could not open the clipboard for the server's formats");
		return;
	}
	if (EmptyClipboard())
	{
		for (UINT id : locals)
			(void)SetClipboardData(id, nullptr); /* delayed: fetched on WM_RENDERFORMAT */
	}
	(void)CloseClipboard();
	if (_dataObject && (OleIsCurrentClipboard(_dataObject) != S_OK))
		releaseDataObject();
	forgetLocal();
}

/* The user's copy is no longer on the clipboard: nothing of it may be served any more. */
void WinClip::forgetLocal()
{
	std::scoped_lock lock(_localLock);
	_offered.clear();
	_localFiles.clear();
	_localEpoch++;
}

/* The server's clipboard now holds nothing this module can show. The user's own local clipboard is
 * left alone; only server data from an earlier list, which can no longer be fetched, is taken back. */
void WinClip::releaseServerData(UINT32 gen)
{
	if (gen != _generation.load())
		return; /* a newer list is already queued */
	dropServerData();
}

/* Takes this module's own server data off the clipboard: delayed formats owned by hwnd, or the
 * file object, whose OLE window also lives on this thread. Ownership is checked again while the
 * clipboard is held open, where it cannot change, so a copy the user has just made is never
 * emptied. EmptyClipboard also makes OLE let go of a file object (WM_DESTROYCLIPBOARD). */
void WinClip::dropServerData()
{
	const HWND hwnd = _hwnd.load();
	if (hwnd && clipboardHoldsServerData() && openClipboardRetry(hwnd))
	{
		if (clipboardHoldsServerData())
			(void)EmptyClipboard();
		(void)CloseClipboard();
	}
	if (_dataObject && (OleIsCurrentClipboard(_dataObject) != S_OK))
		releaseDataObject();
}

void WinClip::renderFormat(UINT localId)
{
	UINT32 remoteId = 0;
	UINT32 gen = 0;
	{
		std::scoped_lock lock(_mapLock);
		gen = _generation.load();
		for (const auto& m : _map)
		{
			if (m.localId == localId)
			{
				remoteId = m.remoteId;
				break;
			}
		}
	}
	if (remoteId == 0)
		return;

	std::vector<BYTE> data;
	if (!requestServerFormat(gen, remoteId, kFormatTimeoutMs, data))
		return;

	/* Text keeps a terminator even if the server's data has none. UTF-16 text of an odd byte count
	 * is padded to a whole character first, so its terminator is a whole WCHAR. */
	size_t size = data.size();
	if (localId == CF_UNICODETEXT)
		size = (size + 1) & ~static_cast<size_t>(1);
	const size_t extra = isTextFormat(localId) ? sizeof(WCHAR) : 0;
	HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, std::max<size_t>(size + extra, 1));
	if (!h)
		return;
	auto p = static_cast<BYTE*>(GlobalLock(h));
	if (p && !data.empty())
		memcpy(p, data.data(), data.size());
	GlobalUnlock(h);
	if (!SetClipboardData(localId, h))
		GlobalFree(h);
}

void WinClip::announceLocal()
{
	if (!_sync)
		return;

	/* Only reached for our own data through WM_CLIP_ANNOUNCE, i.e. after Monitor Ready: a
	 * (re)started server clipboard, which cannot serve the old list. Drop it rather than echo the
	 * server's formats back to it; the empty clipboard is then announced. */
	if (clipboardHoldsServerData())
		dropServerData();

	struct Entry
	{
		UINT32 id;
		std::string name;
	};
	std::vector<Entry> entries;
	const HWND hwnd = _hwnd.load();
	if (!openClipboardRetry(hwnd))
	{
		WLog_Print(_log, WLOG_WARN, "could not open the clipboard to announce it");
		return;
	}
	if (IsClipboardFormatAvailable(CF_HDROP))
	{
		entries.push_back({ _cfFileGroup, "FileGroupDescriptorW" });
		entries.push_back({ _cfFileContents, "FileContents" });
	}
	else
	{
		UINT format = 0;
		while ((format = EnumClipboardFormats(format)) != 0)
		{
			if (format >= 0xC000)
			{
				WCHAR name[256] = {};
				const int len = GetClipboardFormatNameW(format, name, ARRAYSIZE(name));
				if (len <= 0)
					continue;
				const std::wstring wname(name, static_cast<size_t>(len));
				if (deniedFormatName(wname))
					continue;
				char* utf8 = ConvertWCharNToUtf8Alloc(name, static_cast<size_t>(len), nullptr);
				if (!utf8)
					continue;
				try
				{
					entries.push_back({ format, utf8 });
				}
				catch (...)
				{
					free(utf8);
					(void)CloseClipboard(); /* never left open on the way out */
					throw;
				}
				free(utf8);
			}
			else if (standardFormatTransferable(format))
				entries.push_back({ format, std::string() });
		}
	}
	(void)CloseClipboard();

	/* The server may ask for exactly what reaches it. The channel filters this list by the clipboard
	 * feature mask (cliprdr_filter_format_list): file formats pass only with LOCAL_TO_REMOTE_FILES,
	 * every other format only with LOCAL_TO_REMOTE. */
	const UINT32 mask = freerdp_settings_get_uint32(_rdp->settings, FreeRDP_ClipboardFeatureMask);
	std::vector<UINT32> offered;
	for (const auto& e : entries)
	{
		const bool file = (e.id == _cfFileGroup) || (e.id == _cfFileContents);
		if (mask & (file ? CLIPRDR_FLAG_LOCAL_TO_REMOTE_FILES : CLIPRDR_FLAG_LOCAL_TO_REMOTE))
			offered.push_back(e.id);
	}
	{
		std::scoped_lock lock(_localLock);
		_offered.swap(offered);
		_localFiles.clear(); /* rebuilt when the server asks for the list */
		_localEpoch++;
	}

	std::vector<CLIPRDR_FORMAT> formats;
	formats.reserve(entries.size());
	for (auto& e : entries)
		formats.push_back({ e.id, e.name.empty() ? nullptr : e.name.data() });

	CLIPRDR_FORMAT_LIST list = {};
	list.common.msgType = CB_FORMAT_LIST;
	list.numFormats = static_cast<UINT32>(formats.size());
	list.formats = formats.empty() ? nullptr : formats.data();
	const UINT rc = _ctx->ClientFormatList(_ctx, &list);
	if (rc != CHANNEL_RC_OK)
		WLog_Print(_log, WLOG_WARN, "ClientFormatList failed [%" PRIu32 "]", rc);
}

bool WinClip::waitFor(HANDLE event, DWORD timeoutMs) const
{
	HANDLE handles[3] = { _stopEvent, event, freerdp_abort_event(_rdp) };
	const DWORD count = handles[2] ? 3 : 2;
	const DWORD rc = WaitForMultipleObjects(count, handles, FALSE, timeoutMs);
	return rc == WAIT_OBJECT_0 + 1;
}

bool WinClip::requestServerFormat(UINT32 gen, UINT32 remoteId, DWORD timeoutMs,
                                  std::vector<BYTE>& out)
{
	out.clear();
	{
		/* Checked, numbered and sent in one hold (sending only queues the PDU), so a Monitor
		 * Ready, which starts a new generation and then resets the count under this lock, comes
		 * wholly before this request (which then stops here) or wholly after it. */
		std::scoped_lock lock(_formatLock);
		if (gen != _generation.load())
			return false;
		_formatAwaited = ++_formatSent;
		_formatOk = false;
		_formatData.clear();
		(void)ResetEvent(_formatEvent);

		CLIPRDR_FORMAT_DATA_REQUEST request = {};
		request.common.msgType = CB_FORMAT_DATA_REQUEST;
		request.requestedFormatId = remoteId;
		const UINT rc = _ctx->ClientFormatDataRequest(_ctx, &request);
		if (rc != CHANNEL_RC_OK)
		{
			_formatSent--; /* nothing was sent, so no response will come for it */
			if (_formatReceived > _formatSent)
				_formatReceived = _formatSent;
			_formatAwaited = 0;
			WLog_Print(_log, WLOG_WARN, "ClientFormatDataRequest failed [%" PRIu32 "]", rc);
			return false;
		}
	}

	const bool answered = waitFor(_formatEvent, timeoutMs);
	std::scoped_lock lock(_formatLock);
	_formatAwaited = 0; /* a late response is now counted and dropped */
	if (!answered)
	{
		WLog_Print(_log, WLOG_WARN, "no answer from the server for clipboard format %" PRIu32,
		           remoteId);
		return false;
	}
	out.swap(_formatData);
	return _formatOk;
}

bool WinClip::fetchFileList(UINT32 gen, UINT32 remoteFormatId, std::vector<FILEDESCRIPTORW>& files)
{
	files.clear();
	std::vector<BYTE> data;
	if (!requestServerFormat(gen, remoteFormatId, kFileListTimeoutMs, data))
		return false;
	if (data.size() > UINT32_MAX)
		return false;

	FILEDESCRIPTORW* list = nullptr;
	UINT32 count = 0;
	const UINT rc =
	    cliprdr_parse_file_list(data.data(), static_cast<UINT32>(data.size()), &list, &count);
	if ((rc != CHANNEL_RC_OK) || (count > kMaxFiles))
	{
		WLog_Print(_log, WLOG_WARN, "unusable file list from the server [%" PRIu32 ", %" PRIu32 "]",
		           rc, count);
		free(list);
		return false;
	}
	try
	{
		files.assign(list, list + count);
	}
	catch (...)
	{
		free(list);
		throw;
	}
	free(list);

	for (auto& fd : files)
	{
		if (!safeRelativeName(fd.cFileName, ARRAYSIZE(fd.cFileName)))
		{
			WLog_Print(_log, WLOG_WARN, "refusing a file list with an unsafe name");
			files.clear();
			return false;
		}
		fd.dwFlags |= FD_PROGRESSUI;
	}
	return true;
}

bool WinClip::fetchFileContents(UINT32 gen, UINT32 index, UINT32 flags, UINT64 position, UINT32 cb,
                                std::vector<BYTE>& out)
{
	out.clear();
	UINT32 streamId = ++_nextStreamId;
	if (streamId == 0)
		streamId = ++_nextStreamId;
	{
		/* one hold, as in requestServerFormat */
		std::scoped_lock lock(_contentsLock);
		if (gen != _generation.load())
			return false;
		_contentsAwaited = streamId;
		_contentsRequested = cb;
		_contentsOk = false;
		_contentsData.clear();
		(void)ResetEvent(_contentsEvent);

		CLIPRDR_FILE_CONTENTS_REQUEST request = {};
		request.common.msgType = CB_FILECONTENTS_REQUEST;
		request.streamId = streamId;
		request.listIndex = index;
		request.dwFlags = flags;
		request.nPositionLow = static_cast<UINT32>(position & 0xFFFFFFFF);
		request.nPositionHigh = static_cast<UINT32>(position >> 32);
		request.cbRequested = cb;
		const UINT rc = _ctx->ClientFileContentsRequest(_ctx, &request);
		if (rc != CHANNEL_RC_OK)
		{
			_contentsAwaited = 0;
			WLog_Print(_log, WLOG_WARN, "ClientFileContentsRequest failed [%" PRIu32 "]", rc);
			return false;
		}
	}

	const bool answered = waitFor(_contentsEvent, kContentsTimeoutMs);
	std::scoped_lock lock(_contentsLock);
	_contentsAwaited = 0;
	if (!answered)
	{
		WLog_Print(_log, WLOG_WARN, "no answer from the server for file %" PRIu32, index);
		return false;
	}
	out.swap(_contentsData);
	return _contentsOk;
}

/* ---------------------------------------------------------------------------------------------- */

UINT WinClip::sendCapabilities()
{
	CLIPRDR_GENERAL_CAPABILITY_SET general = {};
	general.capabilitySetType = CB_CAPSTYPE_GENERAL;
	general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
	general.version = CB_CAPS_VERSION_2;
	general.generalFlags = CB_USE_LONG_FORMAT_NAMES | CB_STREAM_FILECLIP_ENABLED |
	                       CB_FILECLIP_NO_FILE_PATHS | CB_HUGE_FILE_SUPPORT_ENABLED;
	CLIPRDR_CAPABILITIES caps = {};
	caps.common.msgType = CB_CLIP_CAPS;
	caps.cCapabilitiesSets = 1;
	caps.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&general);
	return _ctx->ClientCapabilities(_ctx, &caps);
}

UINT WinClip::sendFormatListResponse(bool ok)
{
	CLIPRDR_FORMAT_LIST_RESPONSE response = {};
	response.common.msgType = CB_FORMAT_LIST_RESPONSE;
	response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
	return _ctx->ClientFormatListResponse(_ctx, &response);
}

UINT WinClip::sendFormatDataResponse(bool ok, const std::vector<BYTE>& data)
{
	CLIPRDR_FORMAT_DATA_RESPONSE response = {};
	response.common.msgType = CB_FORMAT_DATA_RESPONSE;
	response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
	response.common.dataLen = ok ? static_cast<UINT32>(data.size()) : 0;
	response.requestedFormatData = (ok && !data.empty()) ? data.data() : nullptr;
	return _ctx->ClientFormatDataResponse(_ctx, &response);
}

UINT WinClip::sendFileContentsResponse(UINT32 streamId, bool ok, const std::vector<BYTE>& data)
{
	CLIPRDR_FILE_CONTENTS_RESPONSE response = {};
	response.common.msgType = CB_FILECONTENTS_RESPONSE;
	response.common.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
	response.streamId = streamId;
	response.cbRequested = ok ? static_cast<UINT32>(data.size()) : 0;
	response.requestedData = (ok && !data.empty()) ? data.data() : nullptr;
	return _ctx->ClientFileContentsResponse(_ctx, &response);
}

UINT WinClip::onMonitorReady()
{
	{
		/* The old list is gone with a (re)started server clipboard: renders and file streams of
		 * it fail at once. FIRST, so a request checking the generation under its lock either sees
		 * the new one and stops, or was sent before the count is reset below. */
		std::scoped_lock lock(_mapLock);
		_map.clear();
		_fileGroupRemoteId = 0;
		++_generation;
	}
	/* Such a server answers nothing that was asked before it. Format data responses carry no id
	 * and are matched by count, so count every earlier request as answered, and end a wait that
	 * is still running instead of letting it time out. */
	{
		std::scoped_lock lock(_formatLock);
		_formatReceived = _formatSent;
		if (_formatAwaited != 0)
		{
			_formatOk = false;
			_formatData.clear();
			_formatAwaited = 0; /* a response from now on is not this wait's answer */
			(void)SetEvent(_formatEvent);
		}
	}
	{
		std::scoped_lock lock(_contentsLock);
		if (_contentsAwaited != 0)
		{
			_contentsOk = false;
			_contentsData.clear();
			(void)SetEvent(_contentsEvent);
		}
	}

	const UINT rc = sendCapabilities();
	if (rc != CHANNEL_RC_OK)
		return rc;
	_sync = true;
	const HWND hwnd = _hwnd.load();
	if (hwnd)
		(void)PostMessageW(hwnd, WM_CLIP_ANNOUNCE, 0, 0);
	return CHANNEL_RC_OK;
}

UINT WinClip::onServerCapabilities(const CLIPRDR_CAPABILITIES* caps)
{
	auto p = reinterpret_cast<const BYTE*>(caps->capabilitySets);
	for (UINT32 x = 0; p && (x < caps->cCapabilitiesSets); x++)
	{
		auto set = reinterpret_cast<const CLIPRDR_CAPABILITY_SET*>(p);
		if ((set->capabilitySetType == CB_CAPSTYPE_GENERAL) &&
		    (set->capabilitySetLength >= CB_CAPSTYPE_GENERAL_LEN))
			_serverCaps = reinterpret_cast<const CLIPRDR_GENERAL_CAPABILITY_SET*>(set)->generalFlags;
		if (set->capabilitySetLength == 0)
			break;
		p += set->capabilitySetLength;
	}
	return CHANNEL_RC_OK;
}

UINT WinClip::onServerFormatList(const CLIPRDR_FORMAT_LIST* list)
{
	std::vector<Mapping> map;
	UINT32 fileGroup = 0;
	size_t named = 0;
	auto add = [&map](UINT32 remoteId, UINT localId) {
		for (const auto& m : map)
		{
			if (m.localId == localId)
				return; /* listed twice: the first one wins */
		}
		map.push_back({ remoteId, localId });
	};
	try
	{
		for (UINT32 x = 0; x < list->numFormats; x++)
		{
			const CLIPRDR_FORMAT& f = list->formats[x];
			if (f.formatName && (f.formatName[0] != '\0'))
			{
				WCHAR* wname = ConvertUtf8ToWCharAlloc(f.formatName, nullptr);
				if (!wname)
					continue;
				std::wstring name;
				try
				{
					name = wname;
				}
				catch (...)
				{
					free(wname);
					throw;
				}
				free(wname);
				if (_wcsicmp(name.c_str(), kFileGroupName) == 0)
				{
					fileGroup = f.formatId;
					continue;
				}
				if (deniedFormatName(name))
					continue;
				if (++named > kMaxNamedFormatsPerList)
					continue;
				const UINT local = registerServerName(name);
				if (local != 0)
					add(f.formatId, local);
			}
			else if (standardFormatTransferable(f.formatId))
				add(f.formatId, f.formatId);
		}
	}
	catch (const std::exception&) /* out of memory: the local clipboard is left as it is */
	{
		return sendFormatListResponse(false);
	}

	const bool nothing = map.empty() && (fileGroup == 0);
	UINT32 gen = 0;
	{
		std::scoped_lock lock(_mapLock);
		_map.swap(map);
		_fileGroupRemoteId = fileGroup;
		gen = ++_generation;
	}
	const HWND hwnd = _hwnd.load();
	if (hwnd)
		(void)PostMessageW(hwnd, nothing ? WM_CLIP_SERVER_EMPTY : WM_CLIP_SERVER_FORMATS, 0,
		                   static_cast<LPARAM>(gen));
	return sendFormatListResponse(true);
}

/* A server format name as a local registered format id, or 0 when it is not mirrored. */
UINT WinClip::registerServerName(const std::wstring& name)
{
	/* "#<n>" is integer-atom syntax: RegisterClipboardFormat returns the STANDARD id n for it
	 * ("#15" is CF_HDROP), which would let a server put a file list or a GDI handle format on the
	 * local clipboard past standardFormatTransferable. Only real registered names are mirrored. */
	if (name.empty() || (name[0] == L'#') || (name.size() > 255))
		return 0;

	auto& table = nameTable();
	std::scoped_lock lock(table.lock);
	if (!table.scanned)
	{
		/* Once per process: the names already registered in the session (a few ms). */
		WCHAR buffer[256] = {};
		for (UINT id = 0xC000; id <= 0xFFFF; id++)
		{
			const int len = GetClipboardFormatNameW(id, buffer, ARRAYSIZE(buffer));
			if (len > 0)
				(void)table.ids.emplace(atomKey(std::wstring(buffer, static_cast<size_t>(len))), id);
		}
		table.scanned = true;
	}

	const std::wstring key = atomKey(name);
	const auto known = table.ids.find(key);
	if (known != table.ids.end())
		return known->second;
	if (table.added >= kMaxNamedFormats)
		return 0;
	const UINT local = RegisterClipboardFormatW(name.c_str());
	if (local < 0xC000)
		return 0;
	(void)table.ids.emplace(key, local);
	table.added++;
	return local;
}

bool WinClip::clipboardHoldsServerData() const
{
	const HWND owner = GetClipboardOwner();
	if (!owner)
		return false;
	/* Both this module's window and the window OLE uses for our file object live on the
	 * clipboard thread. */
	return GetWindowThreadProcessId(owner, nullptr) == _threadId;
}

bool WinClip::readLocalFormat(UINT format, std::vector<BYTE>& out) const
{
	out.clear();
	if (!openClipboardRetry(nullptr))
		return false;
	/* Checked again now that the clipboard is open (no one can change its owner meanwhile): it may
	 * have become ours since the caller looked, and our delayed formats would ask the server. */
	if (clipboardHoldsServerData())
	{
		(void)CloseClipboard();
		return false;
	}
	bool ok = false;
	HANDLE h = GetClipboardData(format);
	if (h)
	{
		const SIZE_T size = GlobalSize(h);
		const void* p = (size > 0) ? GlobalLock(h) : nullptr;
		if (p && (size <= UINT32_MAX))
		{
			try
			{
				out.assign(static_cast<const BYTE*>(p), static_cast<const BYTE*>(p) + size);
			}
			catch (...)
			{
				/* never leave the clipboard open for every program in the session */
				GlobalUnlock(h);
				(void)CloseClipboard();
				throw;
			}
			ok = true;
		}
		if (p)
			GlobalUnlock(h);
	}
	(void)CloseClipboard();
	return ok;
}

bool WinClip::addLocalEntry(const std::wstring& path, const std::wstring& relative, int depth,
                            std::vector<LocalFile>& files) const
{
	if ((files.size() >= kMaxFiles) || (depth > kMaxDepth))
		return false;
	if (relative.size() >= MAX_PATH) /* FILEDESCRIPTORW.cFileName */
	{
		WLog_Print(_log, WLOG_WARN, "a copied path is too long for the clipboard, stopping");
		return false;
	}

	WIN32_FILE_ATTRIBUTE_DATA attr = {};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
		return false;

	LocalFile entry;
	entry.path = path;
	entry.fd.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
	entry.fd.dwFileAttributes = attr.dwFileAttributes;
	entry.fd.ftLastWriteTime = attr.ftLastWriteTime;
	const bool directory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	if (!directory)
	{
		entry.fd.nFileSizeHigh = attr.nFileSizeHigh;
		entry.fd.nFileSizeLow = attr.nFileSizeLow;
	}
	(void)wcsncpy_s(entry.fd.cFileName, ARRAYSIZE(entry.fd.cFileName), relative.c_str(),
	                _TRUNCATE);
	files.push_back(entry);

	/* A directory's children follow it. Links are not followed, so a loop cannot recurse. */
	if (!directory || (attr.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
		return true;

	WIN32_FIND_DATAW found = {};
	const std::wstring pattern = path + L"\\*";
	HANDLE find = FindFirstFileW(pattern.c_str(), &found);
	if (find == INVALID_HANDLE_VALUE)
		return true; /* an empty or unreadable folder is still copied, empty */
	bool ok = true;
	do
	{
		if ((wcscmp(found.cFileName, L".") == 0) || (wcscmp(found.cFileName, L"..") == 0))
			continue;
		if (!addLocalEntry(path + L"\\" + found.cFileName, relative + L"\\" + found.cFileName,
		                   depth + 1, files))
		{
			ok = false;
			break;
		}
	} while (FindNextFileW(find, &found));
	(void)FindClose(find);
	return ok;
}

bool WinClip::buildLocalFileList(std::vector<BYTE>& out)
{
	out.clear();
	UINT64 epoch = 0;
	{
		std::scoped_lock lock(_localLock);
		epoch = _localEpoch;
	}
	std::vector<std::wstring> roots;
	if (!openClipboardRetry(nullptr))
		return false;
	if (clipboardHoldsServerData()) /* see readLocalFormat */
	{
		(void)CloseClipboard();
		return false;
	}
	auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
	if (drop)
	{
		const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		for (UINT x = 0; x < count; x++)
		{
			const UINT len = DragQueryFileW(drop, x, nullptr, 0);
			if (len == 0)
				continue;
			std::wstring path(len, L'\0');
			if (DragQueryFileW(drop, x, path.data(), len + 1) != len)
				continue;
			roots.push_back(std::move(path));
		}
	}
	(void)CloseClipboard();
	if (roots.empty())
		return false;

	std::vector<LocalFile> files;
	for (auto& root : roots)
	{
		while ((root.size() > 3) && ((root.back() == L'\\') || (root.back() == L'/')))
			root.pop_back();
		const size_t slash = root.find_last_of(L"\\/");
		const std::wstring name = (slash == std::wstring::npos) ? root : root.substr(slash + 1);
		if (name.empty() || !addLocalEntry(root, name, 0, files))
		{
			WLog_Print(_log, WLOG_WARN, "could not list the copied files");
			return false;
		}
	}

	std::vector<FILEDESCRIPTORW> descriptors;
	descriptors.reserve(files.size());
	for (const auto& f : files)
		descriptors.push_back(f.fd);

	BYTE* data = nullptr;
	UINT32 size = 0;
	const UINT32 flags = CB_STREAM_FILECLIP_ENABLED | (_serverCaps & CB_HUGE_FILE_SUPPORT_ENABLED);
	if (cliprdr_serialize_file_list_ex(flags, descriptors.data(),
	                                   static_cast<UINT32>(descriptors.size()), &data,
	                                   &size) != CHANNEL_RC_OK)
	{
		WLog_Print(_log, WLOG_WARN, "could not serialize the copied files (a file over 4 GiB?)");
		free(data);
		return false;
	}
	try
	{
		out.assign(data, data + size);
	}
	catch (...)
	{
		free(data);
		throw;
	}
	free(data);

	std::scoped_lock lock(_localLock);
	if (epoch != _localEpoch)
	{
		out.clear(); /* the clipboard changed while the files were listed */
		return false;
	}
	_localFiles.swap(files);
	return true;
}

bool WinClip::readLocalFile(const std::wstring& path, UINT64 position, UINT32 cb,
                            std::vector<BYTE>& out)
{
	out.clear();
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
	                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER where = {};
	where.QuadPart = static_cast<LONGLONG>(position);
	bool ok = SetFilePointerEx(file, where, nullptr, FILE_BEGIN) != FALSE;
	if (ok)
	{
		out.resize(cb);
		DWORD total = 0;
		while (total < cb)
		{
			DWORD got = 0;
			if (!ReadFile(file, out.data() + total, cb - total, &got, nullptr))
			{
				ok = false;
				break;
			}
			if (got == 0)
				break; /* end of file */
			total += got;
		}
		out.resize(total);
	}
	(void)CloseHandle(file);
	return ok;
}

UINT WinClip::onServerFormatDataRequest(const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
	std::vector<BYTE> data;
	bool ok = false;

	/* A copy made a moment ago may not be rendered yet: let it settle (kLocalSettleMs). A new copy
	 * restarts the wait, up to a few rounds. */
	for (int round = 0; round < 5; round++)
	{
		const ULONGLONG since = GetTickCount64() - _localChangeTick.load();
		if (since >= kLocalSettleMs)
			break;
		if (WaitForSingleObject(_stopEvent, static_cast<DWORD>(kLocalSettleMs - since)) !=
		    WAIT_TIMEOUT)
			break;
	}

	/* Only what was announced, and let through by the feature mask, is served: never CF_HDROP and
	 * its local paths, nor any format this module chose not to offer. */
	bool offered = false;
	{
		std::scoped_lock lock(_localLock);
		offered = (request->requestedFormatId != _cfFileContents) &&
		          (std::find(_offered.begin(), _offered.end(), request->requestedFormatId) !=
		           _offered.end());
	}

	/* The clipboard holds what the server gave us: reading it would ask the server, which waits
	 * for this very answer. */
	if (offered && !clipboardHoldsServerData())
	{
		try
		{
			if (request->requestedFormatId == _cfFileGroup)
				ok = buildLocalFileList(data);
			else
				ok = readLocalFormat(request->requestedFormatId, data);
		}
		catch (const std::exception&) /* out of memory: the server is told it failed */
		{
			data.clear();
			ok = false;
		}
	}
	UINT rc = sendFormatDataResponse(ok, data);
	if ((rc != CHANNEL_RC_OK) && ok)
	{
		/* the data could not be sent (too big to allocate?): the server still needs an answer */
		data.clear();
		rc = sendFormatDataResponse(false, data);
	}
	if (rc != CHANNEL_RC_OK)
		WLog_Print(_log, WLOG_WARN, "ClientFormatDataResponse failed [%" PRIu32 "]", rc);
	return CHANNEL_RC_OK;
}

UINT WinClip::onServerFormatDataResponse(const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	std::scoped_lock lock(_formatLock);
	const UINT64 number = ++_formatReceived;
	if ((_formatAwaited == 0) || (number < _formatAwaited))
		return CHANNEL_RC_OK; /* the answer to a request that already timed out */
	/* A number past the awaited one means the count ran ahead (a request counted as answered at a
	 * Monitor Ready was answered after all, or the server sent an extra response): this is the
	 * awaited answer, and the count is back in step from here. */
	_formatReceived = _formatAwaited;
	_formatOk = (response->common.msgFlags & CB_RESPONSE_OK) != 0;
	if (_formatOk && response->requestedFormatData)
	{
		try
		{
			_formatData.assign(response->requestedFormatData,
			                   response->requestedFormatData + response->common.dataLen);
		}
		catch (const std::exception&)
		{
			_formatData.clear();
			_formatOk = false;
		}
	}
	(void)SetEvent(_formatEvent);
	return CHANNEL_RC_OK;
}

UINT WinClip::onServerFileContentsRequest(const CLIPRDR_FILE_CONTENTS_REQUEST* request)
{
	std::vector<BYTE> data;
	bool ok = false;
	try
	{
		/* Only files of the copy that is announced right now: _localFiles is reset whenever the
		 * clipboard changes, and the list itself must be on offer. */
		LocalFile file;
		bool known = false;
		{
			std::scoped_lock lock(_localLock);
			if ((request->listIndex < _localFiles.size()) &&
			    (std::find(_offered.begin(), _offered.end(), _cfFileGroup) != _offered.end()))
			{
				file = _localFiles[request->listIndex];
				known = true;
			}
		}

		if (known)
		{
			if (request->dwFlags & FILECONTENTS_SIZE)
			{
				const UINT64 size =
				    (static_cast<UINT64>(file.fd.nFileSizeHigh) << 32) | file.fd.nFileSizeLow;
				data.resize(sizeof(UINT64));
				memcpy(data.data(), &size, sizeof(UINT64));
				ok = true;
			}
			else if (request->dwFlags & FILECONTENTS_RANGE)
			{
				const UINT64 position =
				    (static_cast<UINT64>(request->nPositionHigh) << 32) | request->nPositionLow;
				ok = readLocalFile(file.path, position,
				                   std::min(request->cbRequested, kMaxServeBlock), data);
			}
		}
	}
	catch (const std::exception&) /* out of memory: the server is told it failed */
	{
		data.clear();
		ok = false;
	}
	UINT rc = sendFileContentsResponse(request->streamId, ok, data);
	if ((rc != CHANNEL_RC_OK) && ok)
	{
		data.clear(); /* as in onServerFormatDataRequest */
		rc = sendFileContentsResponse(request->streamId, false, data);
	}
	if (rc != CHANNEL_RC_OK)
		WLog_Print(_log, WLOG_WARN, "ClientFileContentsResponse failed [%" PRIu32 "]", rc);
	return CHANNEL_RC_OK;
}

UINT WinClip::onServerFileContentsResponse(const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	std::scoped_lock lock(_contentsLock);
	if ((_contentsAwaited == 0) || (response->streamId != _contentsAwaited))
		return CHANNEL_RC_OK; /* the answer to a request that already timed out */
	_contentsOk = (response->common.msgFlags & CB_RESPONSE_OK) != 0;
	if (_contentsOk && response->requestedData)
	{
		/* never more than was asked for */
		const UINT32 cb = std::min(response->cbRequested, _contentsRequested);
		try
		{
			_contentsData.assign(response->requestedData, response->requestedData + cb);
		}
		catch (const std::exception&)
		{
			_contentsData.clear();
			_contentsOk = false;
		}
	}
	(void)SetEvent(_contentsEvent);
	return CHANNEL_RC_OK;
}

/* ---------------------------------------------------------------------------------------------- */

namespace
{
	WinClip* fromContext(CliprdrClientContext* context)
	{
		return context ? static_cast<WinClip*>(context->custom) : nullptr;
	}

	/* The channel calls these from C, so nothing may unwind out of them. The handlers catch what
	 * they can answer; this is the backstop. CHANNEL_RC_OK, because an error code here would end
	 * the whole connection over a clipboard message. */
	template <typename Fn> UINT guarded(WinClip* clip, Fn&& fn)
	{
		try
		{
			return fn();
		}
		catch (const std::exception&)
		{
			WLog_Print(clip->log(), WLOG_ERROR, "clipboard channel message failed: out of memory");
			return CHANNEL_RC_OK;
		}
	}

	UINT monitorReady(CliprdrClientContext* context, const CLIPRDR_MONITOR_READY*)
	{
		auto clip = fromContext(context);
		return clip ? guarded(clip, [&] { return clip->onMonitorReady(); }) : CHANNEL_RC_OK;
	}
	UINT serverCapabilities(CliprdrClientContext* context, const CLIPRDR_CAPABILITIES* caps)
	{
		auto clip = fromContext(context);
		return (clip && caps) ? guarded(clip, [&] { return clip->onServerCapabilities(caps); })
		                       : CHANNEL_RC_OK;
	}
	UINT serverFormatList(CliprdrClientContext* context, const CLIPRDR_FORMAT_LIST* list)
	{
		auto clip = fromContext(context);
		return (clip && list) ? guarded(clip, [&] { return clip->onServerFormatList(list); })
		                       : CHANNEL_RC_OK;
	}
	UINT serverFormatListResponse(CliprdrClientContext*, const CLIPRDR_FORMAT_LIST_RESPONSE* r)
	{
		if (r && (r->common.msgFlags & CB_RESPONSE_FAIL))
			WLog_WARN(TAG, "the server refused our clipboard format list");
		return CHANNEL_RC_OK;
	}
	UINT serverLock(CliprdrClientContext*, const CLIPRDR_LOCK_CLIPBOARD_DATA*)
	{
		return CHANNEL_RC_OK;
	}
	UINT serverUnlock(CliprdrClientContext*, const CLIPRDR_UNLOCK_CLIPBOARD_DATA*)
	{
		return CHANNEL_RC_OK;
	}
	UINT serverFormatDataRequest(CliprdrClientContext* context,
	                             const CLIPRDR_FORMAT_DATA_REQUEST* request)
	{
		auto clip = fromContext(context);
		return (clip && request)
		           ? guarded(clip, [&] { return clip->onServerFormatDataRequest(request); })
		           : CHANNEL_RC_OK;
	}
	UINT serverFormatDataResponse(CliprdrClientContext* context,
	                              const CLIPRDR_FORMAT_DATA_RESPONSE* response)
	{
		auto clip = fromContext(context);
		return (clip && response)
		           ? guarded(clip, [&] { return clip->onServerFormatDataResponse(response); })
		           : CHANNEL_RC_OK;
	}
	UINT serverFileContentsRequest(CliprdrClientContext* context,
	                               const CLIPRDR_FILE_CONTENTS_REQUEST* request)
	{
		auto clip = fromContext(context);
		return (clip && request)
		           ? guarded(clip, [&] { return clip->onServerFileContentsRequest(request); })
		           : CHANNEL_RC_OK;
	}
	UINT serverFileContentsResponse(CliprdrClientContext* context,
	                                const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
	{
		auto clip = fromContext(context);
		return (clip && response)
		           ? guarded(clip, [&] { return clip->onServerFileContentsResponse(response); })
		           : CHANNEL_RC_OK;
	}
} // namespace

bool sdl_wincliprdr_init(rdpContext* context, CliprdrClientContext* cliprdr)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(cliprdr);

	auto clip = new WinClip(context, cliprdr);
	if (!clip->start())
	{
		clip->stop();
		if (!clip->leaked())
			delete clip;
		return false;
	}

	cliprdr->custom = clip;
	cliprdr->MonitorReady = monitorReady;
	cliprdr->ServerCapabilities = serverCapabilities;
	cliprdr->ServerFormatList = serverFormatList;
	cliprdr->ServerFormatListResponse = serverFormatListResponse;
	cliprdr->ServerLockClipboardData = serverLock;
	cliprdr->ServerUnlockClipboardData = serverUnlock;
	cliprdr->ServerFormatDataRequest = serverFormatDataRequest;
	cliprdr->ServerFormatDataResponse = serverFormatDataResponse;
	cliprdr->ServerFileContentsRequest = serverFileContentsRequest;
	cliprdr->ServerFileContentsResponse = serverFileContentsResponse;
	return true;
}

bool sdl_wincliprdr_uninit(CliprdrClientContext* cliprdr)
{
	WINPR_ASSERT(cliprdr);
	auto clip = fromContext(cliprdr);
	cliprdr->custom = nullptr;
	if (!clip)
		return true;
	clip->stop();
	if (!clip->leaked())
		delete clip;
	return true;
}
