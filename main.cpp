// main.cpp (final)
// 功能：
// - 任务栏上方悬浮小窗，显示已保存文件数量
// - 拖入文件：默认覆盖；按住 Ctrl 拖入：追加（最多 max_count）
// - 从小窗拖出：OLE DoDragDrop，CF_HDROP 多文件
// - Win+D/截图遮罩等导致消失：自愈定时器 heal_interval_ms 拉回显示并置顶
// - 右键：弹出美观 tip（#f9f9f9，字体大小可配），位置在“底部任务栏上方居中”
//   tip 高度随文件数量自适应，超过 max_lines（默认30）不再增长，最后一行显示剩余数量
// - Ctrl + 右键：退出
// - x/y 支持负数：距右侧(-x)、距底部(-y)
// - 位置/颜色/字体/透明(可选)/tip参数 通过 config.ini
//
// 编译（MinGW-w64）:
// g++ -std=c++17 -Os -s -mwindows main.cpp -o FileRelayDock.exe -lole32 -lshell32 -luuid

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objidl.h>
#include <strsafe.h>

// ---------------- constants ----------------
static const int HARD_MAX = 100;
#define TIMER_HEAL      1
#define TIMER_TIP_CLOSE 2

// ---------------- global state ----------------
static wchar_t g_paths[HARD_MAX][MAX_PATH];
static wchar_t g_names[HARD_MAX][MAX_PATH];
static int g_count = 0;

static HFONT  g_mainFont = NULL;
static HBRUSH g_mainBgBrush = NULL;

static HFONT  g_tipFont = NULL;
static HBRUSH g_tipBgBrush = NULL;

static POINT g_mouseDownPt{};
static bool  g_mouseDown = false;

static wchar_t g_iniPath[MAX_PATH] = L"";

// right-click tip window
static HWND g_tipWnd = NULL;
static wchar_t g_tipText[16384];

// window classes
static const wchar_t MAIN_CLASS[] = L"FileRelayDockWnd";
static const wchar_t TIP_CLASS[]  = L"FileRelayTipWnd";

static HANDLE g_singleMutex = NULL;

struct AppStyle {
    int x = -420, y = -1, w = 60, h = 43;
    bool topmost = true;

    int healIntervalMs = 1000; // 0=off
    int maxCount = 100;

    COLORREF bg = RGB(0xFF, 0xFF, 0xFF);
    COLORREF fg = RGB(0x33, 0x33, 0x33);

    int fontSize = 16;
    wchar_t fontName[64] = L"Segoe UI";
    bool showSingleTip = false;   // 重复运行是否提示

    // optional transparency for main window
    bool layered = false;
    bool useColorKey = false;
    BYTE alpha = 255;               // 0..255
    COLORREF colorKey = RGB(0x20, 0x20, 0x20);

    // tip look & behavior
    int tipAutoCloseMs = 2000;       // auto close after ms; 0=never
    int tipWidth = 320;              // width fixed (configurable)
    int tipMinH = 80;               // minimum height
    int tipMaxLines = 30;            // cap lines shown; last line becomes "...还有X个"
    int tipMaxH = 0;                 // 0=derive from tipMaxLines; else fixed max height
    int tipFontSize = 9;            // configurable
    int tipMargin = 8;               // distance from taskbar edge
    bool tipClickThrough = false;    // if true, tip won't capture mouse (HTTRANSPARENT)
} g_style;

// ---------------- ini helpers ----------------
static int IniInt(const wchar_t* section, const wchar_t* key, int def, const wchar_t* ini) {
    return (int)GetPrivateProfileIntW(section, key, def, ini);
}
static void IniStr(const wchar_t* section, const wchar_t* key, const wchar_t* def,
                   wchar_t* out, DWORD outcch, const wchar_t* ini) {
    GetPrivateProfileStringW(section, key, def, out, outcch, ini);
}
static COLORREF ParseColor(const wchar_t* s, COLORREF def) {
    if (!s || !*s) return def;
    unsigned int v = 0;
    if (swscanf(s, L"0x%x", &v) == 1) {
        BYTE r = (v >> 16) & 0xFF;
        BYTE g = (v >> 8) & 0xFF;
        BYTE b = (v >> 0) & 0xFF;
        return RGB(r, g, b);
    }
    return def;
}

static void ResolveXY(int& x, int& y, int w, int h) {
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);

    if (x < 0) x = sw - w + x; // distance from right = -x
    if (y < 0) y = sh - h + y; // distance from bottom = -y

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > sw - w) x = sw - w;
    if (y > sh - h) y = sh - h;
}

static void RebuildGdiObjects() {
    if (g_mainFont) { DeleteObject(g_mainFont); g_mainFont = NULL; }
    if (g_tipFont)  { DeleteObject(g_tipFont);  g_tipFont  = NULL; }
    if (g_mainBgBrush) { DeleteObject(g_mainBgBrush); g_mainBgBrush = NULL; }
    if (g_tipBgBrush)  { DeleteObject(g_tipBgBrush);  g_tipBgBrush  = NULL; }

    HDC hdc = GetDC(NULL);
    int logPix = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(NULL, hdc);

    // main font
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(g_style.fontSize, logPix, 72);
    lf.lfWeight = FW_NORMAL;
    StringCchCopyW(lf.lfFaceName, LF_FACESIZE, g_style.fontName);
    g_mainFont = CreateFontIndirectW(&lf);

    // tip font
    LOGFONTW tf{};
    tf.lfHeight = -MulDiv(g_style.tipFontSize, logPix, 72);
    tf.lfWeight = FW_NORMAL;
    StringCchCopyW(tf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    g_tipFont = CreateFontIndirectW(&tf);

    g_mainBgBrush = CreateSolidBrush(g_style.bg);
    g_tipBgBrush  = CreateSolidBrush(RGB(0xF9, 0xF9, 0xF9)); // #f9f9f9
}

static bool FileExists(const wchar_t* path) {
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static void WriteDefaultIni(const wchar_t* iniPath) {
    // 这里用当前 g_style 的默认值写出一份 ini
    // 注意：bg/fg/colorkey 用 0xRRGGBB 输出
    auto toHexRGB = [](COLORREF c) -> unsigned int {
        unsigned int r = GetRValue(c);
        unsigned int g = GetGValue(c);
        unsigned int b = GetBValue(c);
        return (r << 16) | (g << 8) | (b);
    };

    wchar_t buf[2048];
    HANDLE h = CreateFileW(iniPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    auto writeW = [&](const wchar_t* s) {
        DWORD bytes = 0;
        WriteFile(h, s, (DWORD)(wcslen(s) * sizeof(wchar_t)), &bytes, NULL);
    };

    // 写 UTF-16 LE BOM，保证记事本打开正常
    WORD bom = 0xFEFF;
    DWORD bytes = 0;
    WriteFile(h, &bom, sizeof(bom), &bytes, NULL);

    writeW(L"; TransFile config.ini\r\n");
    writeW(L"; x/y 支持负数：x=-20 表示离右侧20px，y=-60 表示离底部60px\r\n");
    writeW(L"; 拖入：默认覆盖；按住 Ctrl 拖入=追加\r\n");
    writeW(L"; 右键显示tip；按住 Ctrl + 右键退出程序 \r\n");
    writeW(L"\r\n");

    StringCchPrintfW(buf, 2048,
        L"[window]\r\n"
        L"x=%d\r\n"
        L"y=%d\r\n"
        L"w=%d\r\n"
        L"h=%d\r\n"
        L"topmost=%d\r\n"
        L"max_count=%d\r\n"
        L"heal_interval_ms=%d\r\n"
        L"show_single_tip=0\r\n"
        L"\r\n",
        g_style.x, g_style.y, g_style.w, g_style.h,
        g_style.topmost ? 1 : 0,
        g_style.maxCount,
        g_style.healIntervalMs
    );
    writeW(buf);

    StringCchPrintfW(buf, 2048,
        L"[style]\r\n"
        L"bg=0x%06X\r\n"
        L"fg=0x%06X\r\n"
        L"font_size=%d\r\n"
        L"font_name=%s\r\n"
        L"; layered=0\r\n"
        L"; alpha=255\r\n"
        L"; colorkey=0\r\n"
        L"; colorkey_rgb=0x%06X\r\n"
        L"\r\n",
        toHexRGB(g_style.bg),
        toHexRGB(g_style.fg),
        g_style.fontSize,
        g_style.fontName,
        toHexRGB(g_style.colorKey)
    );
    writeW(buf);

    StringCchPrintfW(buf, 2048,
        L"[tip]\r\n"
        L"w=%d\r\n"
        L"min_h=%d\r\n"
        L"max_lines=%d\r\n"
        L"max_h=%d\r\n"
        L"font_size=%d\r\n"
        L"margin=%d\r\n"
        L"auto_close_ms=%d\r\n"
        L"click_through=%d\r\n"
        L"\r\n",
        g_style.tipWidth,
        g_style.tipMinH,
        g_style.tipMaxLines,
        g_style.tipMaxH,
        g_style.tipFontSize,
        g_style.tipMargin,
        g_style.tipAutoCloseMs,
        g_style.tipClickThrough ? 1 : 0
    );
    writeW(buf);

    CloseHandle(h);
}

static void LoadIniStyle(const wchar_t* iniPath) {
    g_style.x = IniInt(L"window", L"x", -430, iniPath);
    g_style.y = IniInt(L"window", L"y", -1, iniPath);
    g_style.w = IniInt(L"window", L"w", 60, iniPath);
    g_style.h = IniInt(L"window", L"h", 43, iniPath);
    g_style.topmost = IniInt(L"window", L"topmost", 1, iniPath) != 0;

    g_style.healIntervalMs = IniInt(L"window", L"heal_interval_ms", 1000, iniPath);
    if (g_style.healIntervalMs < 0) g_style.healIntervalMs = 0;

    g_style.maxCount = IniInt(L"window", L"max_count", 100, iniPath);
    if (g_style.maxCount < 1) g_style.maxCount = 1;
    if (g_style.maxCount > HARD_MAX) g_style.maxCount = HARD_MAX;

    wchar_t buf[128];
    IniStr(L"style", L"bg", L"0xffffff", buf, 128, iniPath);
    g_style.bg = ParseColor(buf, RGB(0x20, 0x20, 0x20));
    IniStr(L"style", L"fg", L"0x333", buf, 128, iniPath);
    g_style.fg = ParseColor(buf, RGB(0xFF, 0xFF, 0xFF));

    g_style.fontSize = IniInt(L"style", L"font_size", 16, iniPath);
    IniStr(L"style", L"font_name", L"Segoe UI", g_style.fontName, 64, iniPath);

    g_style.showSingleTip = IniInt(L"window", L"show_single_tip", 0, iniPath) != 0;

    // main window transparency (optional)
    g_style.layered = IniInt(L"style", L"layered", 0, iniPath) != 0;
    int a = IniInt(L"style", L"alpha", 255, iniPath);
    if (a < 0) a = 0; if (a > 255) a = 255;
    g_style.alpha = (BYTE)a;

    g_style.useColorKey = IniInt(L"style", L"colorkey", 0, iniPath) != 0;
    IniStr(L"style", L"colorkey_rgb", L"0x202020", buf, 128, iniPath);
    g_style.colorKey = ParseColor(buf, RGB(0x20, 0x20, 0x20));

    // tip config
    g_style.tipAutoCloseMs = IniInt(L"tip", L"auto_close_ms", 2000, iniPath);
    if (g_style.tipAutoCloseMs < 0) g_style.tipAutoCloseMs = 0;

    g_style.tipWidth = IniInt(L"tip", L"w", 320, iniPath);
    if (g_style.tipWidth < 180) g_style.tipWidth = 180;

    g_style.tipMinH = IniInt(L"tip", L"min_h", 80, iniPath);
    if (g_style.tipMinH < 60) g_style.tipMinH = 60;

    g_style.tipMaxLines = IniInt(L"tip", L"max_lines", 30, iniPath);
    if (g_style.tipMaxLines < 1) g_style.tipMaxLines = 1;
    if (g_style.tipMaxLines > 200) g_style.tipMaxLines = 200;

    g_style.tipMaxH = IniInt(L"tip", L"max_h", 0, iniPath);
    if (g_style.tipMaxH < 0) g_style.tipMaxH = 0;

    g_style.tipFontSize = IniInt(L"tip", L"font_size", 9, iniPath);
    if (g_style.tipFontSize < 8)  g_style.tipFontSize = 8;
    if (g_style.tipFontSize > 28) g_style.tipFontSize = 28;

    g_style.tipMargin = IniInt(L"tip", L"margin", 8, iniPath);
    if (g_style.tipMargin < 0) g_style.tipMargin = 0;

    g_style.tipClickThrough = IniInt(L"tip", L"click_through", 0, iniPath) != 0;

    RebuildGdiObjects();
}

// ---------------- OLE drag-out ----------------
class DropSource : public IDropSource {
    LONG m_ref;
public:
    DropSource() : m_ref(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = (IDropSource*)this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed) return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

class DataObject : public IDataObject {
    LONG m_ref;
    wchar_t* m_list;
    SIZE_T m_listChars;
public:
    DataObject(const wchar_t paths[][MAX_PATH], int count)
        : m_ref(1), m_list(nullptr), m_listChars(0) {

        if (count < 1) return;

        SIZE_T chars = 1;
        for (int i = 0; i < count; ++i) {
            SIZE_T len = wcslen(paths[i]);
            if (len == 0) continue;
            chars += len + 1;
        }
        if (chars < 2) chars = 2;

        m_list = (wchar_t*)CoTaskMemAlloc(chars * sizeof(wchar_t));
        if (!m_list) { m_listChars = 0; return; }

        wchar_t* p = m_list;
        for (int i = 0; i < count; ++i) {
            if (paths[i][0] == 0) continue;
            SIZE_T len = wcslen(paths[i]);
            memcpy(p, paths[i], len * sizeof(wchar_t));
            p += len;
            *p++ = L'\0';
        }
        *p++ = L'\0';
        m_listChars = (SIZE_T)(p - m_list);
    }

    ~DataObject() {
        if (m_list) CoTaskMemFree(m_list);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = (IDataObject*)this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    STDMETHODIMP GetData(FORMATETC* pFormat, STGMEDIUM* pMedium) override {
        if (!pFormat || !pMedium) return E_POINTER;
        if (pFormat->cfFormat != CF_HDROP) return DV_E_FORMATETC;
        if (!(pFormat->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        if (!m_list || m_listChars < 2) return DV_E_FORMATETC;

        SIZE_T bytes = sizeof(DROPFILES) + m_listChars * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GHND | GMEM_SHARE, bytes);
        if (!hMem) return STG_E_MEDIUMFULL;

        BYTE* p = (BYTE*)GlobalLock(hMem);
        if (!p) { GlobalFree(hMem); return STG_E_MEDIUMFULL; }

        DROPFILES* df = (DROPFILES*)p;
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(p + sizeof(DROPFILES), m_list, m_listChars * sizeof(wchar_t));

        GlobalUnlock(hMem);

        pMedium->tymed = TYMED_HGLOBAL;
        pMedium->hGlobal = hMem;
        pMedium->pUnkForRelease = nullptr;
        return S_OK;
    }

    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* pFormat) override {
        if (!pFormat) return E_POINTER;
        if (pFormat->cfFormat == CF_HDROP && (pFormat->tymed & TYMED_HGLOBAL)) return S_OK;
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
};

static void StartDragIfHasFiles(HWND hwnd) {
    (void)hwnd;
    if (g_count <= 0) return;

    IDataObject* data = new DataObject(g_paths, g_count);
    IDropSource* src = new DropSource();
    DWORD effect = 0;
    DoDragDrop(data, src, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    src->Release();
    data->Release();
}

// ---------------- main drawing ----------------
static void PaintMain(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc; GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, g_mainBgBrush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_style.fg);
    HFONT old = (HFONT)SelectObject(hdc, g_mainFont);

    wchar_t text[64];
    StringCchPrintfW(text, 64, L"%d", g_count);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, old);
    EndPaint(hwnd, &ps);
}

static void ApplyLayeredAttributes(HWND hwnd) {
    if (!g_style.layered) return;

    if (g_style.useColorKey) {
        SetLayeredWindowAttributes(hwnd, g_style.colorKey, 0, LWA_COLORKEY);
    } else {
        SetLayeredWindowAttributes(hwnd, 0, g_style.alpha, LWA_ALPHA);
    }
}

// ---------------- taskbar position helpers ----------------
static bool GetTaskbarRect(RECT& out) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
        out = abd.rc;
        return true;
    }
    return false;
}

static void ComputeTipPosCenteredAboveBottomTaskbar(int tipW, int tipH, int& outX, int& outY) {
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);

    outX = (sw - tipW) / 2;
    outY = sh - tipH - g_style.tipMargin;

    RECT tb{};
    if (GetTaskbarRect(tb)) {
        int tbW = tb.right - tb.left;
        int tbH = tb.bottom - tb.top;

        // bottom taskbar (most common)
        if (tbW >= tbH && tb.bottom >= sh - 2) {
            outY = tb.top - tipH - g_style.tipMargin;
        } else {
            // not bottom: fall back to "bottom of screen" behavior
            outY = sh - tipH - g_style.tipMargin;
        }
    }

    if (outX < 0) outX = 0;
    if (outY < 0) outY = 0;
    if (outX > sw - tipW) outX = sw - tipW;
    if (outY > sh - tipH) outY = sh - tipH;
}

// ---------------- Tip window ----------------
static int BuildTipTextAndGetShownLines() {
    g_tipText[0] = 0;

    if (g_count <= 0) {
        StringCchCopyW(g_tipText, _countof(g_tipText), L"(空)");
        return 1;
    }

    int maxLines = g_style.tipMaxLines;
    if (maxLines < 1) maxLines = 1;

    bool needMoreLine = (g_count > maxLines);
    int showNames = needMoreLine ? (maxLines - 1) : maxLines;
    if (showNames < 0) showNames = 0;

    int written = 0;
    for (int i = 0; i < g_count && written < showNames; ++i) {
        const wchar_t* s = g_names[i][0] ? g_names[i] : g_paths[i];
        if (!s || !*s) continue;

        if (g_tipText[0] != 0) StringCchCatW(g_tipText, _countof(g_tipText), L"\r\n");
        StringCchCatW(g_tipText, _countof(g_tipText), s);
        written++;
    }

    int lines = written;
    if (needMoreLine && maxLines >= 1) {
        int remaining = g_count - written;
        if (g_tipText[0] != 0) StringCchCatW(g_tipText, _countof(g_tipText), L"\r\n");

        wchar_t more[64];
        StringCchPrintfW(more, _countof(more), L"...还有 %d 个文件", remaining);
        StringCchCatW(g_tipText, _countof(g_tipText), more);
        lines += 1;
    }

    if (lines <= 0) lines = 1;
    return lines;
}

// Estimate line height from font size (simple & compact; good enough for Segoe UI)
static int EstimateLineHeightPx() {
    int h = (int)(g_style.tipFontSize * 1.7); // 1.45
    if (h < 14) h = 14;
    return h;
}

static void ShowAutoCloseTip(HWND owner) {
    int shownLines = BuildTipTextAndGetShownLines();
    int lineH = EstimateLineHeightPx();

    const int padTop = 10, padBottom = 10;
    const int border = 2;

    int desiredH = padTop + padBottom + border + shownLines * lineH;

    int maxH = g_style.tipMaxH;
    if (maxH == 0) {
        int maxLines = g_style.tipMaxLines;
        if (maxLines < 1) maxLines = 1;
        maxH = padTop + padBottom + border + maxLines * lineH;
    }

    int h = desiredH;
    if (h < g_style.tipMinH) h = g_style.tipMinH;
    if (h > maxH) h = maxH;

    int w = g_style.tipWidth;

    if (g_tipWnd && IsWindow(g_tipWnd)) {
        DestroyWindow(g_tipWnd);
        g_tipWnd = NULL;
    }

    int x = 0, y = 0;
    ComputeTipPosCenteredAboveBottomTaskbar(w, h, x, y);

    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    g_tipWnd = CreateWindowExW(
        ex, TIP_CLASS, L"", WS_POPUP,
        x, y, w, h,
        owner, NULL, GetModuleHandleW(NULL), NULL
    );

    ShowWindow(g_tipWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_tipWnd);
}

static void PaintTip(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc; GetClientRect(hwnd, &rc);

    FillRect(hdc, &rc, g_tipBgBrush);

    // subtle border
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xDD, 0xDD, 0xDD));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0x22, 0x22, 0x22));
    HFONT oldFont = (HFONT)SelectObject(hdc, g_tipFont);

    RECT tr = rc;
    tr.left += 12; tr.top += 10; tr.right -= 12; tr.bottom -= 10;

    DrawTextW(hdc, g_tipText, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK);

    SelectObject(hdc, oldFont);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK TipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        if (g_style.tipAutoCloseMs > 0) {
            SetTimer(hwnd, TIMER_TIP_CLOSE, (UINT)g_style.tipAutoCloseMs, NULL);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_TIP_CLOSE) {
            KillTimer(hwnd, TIMER_TIP_CLOSE);
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_PAINT:
        PaintTip(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST:
        if (g_style.tipClickThrough) return HTTRANSPARENT;
        break;

    case WM_DESTROY:
        if (g_tipWnd == hwnd) g_tipWnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- Main window proc ----------------
static void UpdateMain(HWND hwnd) {
    InvalidateRect(hwnd, NULL, TRUE);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        if (g_style.healIntervalMs > 0) {
            SetTimer(hwnd, TIMER_HEAL, (UINT)g_style.healIntervalMs, NULL);
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_HEAL) {
            if (IsIconic(hwnd) || !IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
            SetWindowPos(hwnd, g_style.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return 0;
        }
        break;

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT total = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);

        bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        // default: overwrite; Ctrl: append
        if (!ctrlDown) g_count = 0;

        for (UINT i = 0; i < total && g_count < g_style.maxCount; ++i) {
            wchar_t path[MAX_PATH];
            UINT n = DragQueryFileW(hDrop, i, path, MAX_PATH);
            if (n > 0) {
                StringCchCopyW(g_paths[g_count], MAX_PATH, path);
                const wchar_t* base = wcsrchr(path, L'\\');
                StringCchCopyW(g_names[g_count], MAX_PATH, base ? (base + 1) : path);
                g_count++;
            }
        }
        DragFinish(hDrop);

        UpdateMain(hwnd);
        return 0;
    }

    case WM_RBUTTONDOWN:
        // Ctrl + Right Click to exit
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            DestroyWindow(hwnd);
            return 0;
        }
        ShowAutoCloseTip(hwnd);
        return 0;

    case WM_LBUTTONDOWN:
        g_mouseDown = true;
        g_mouseDownPt.x = GET_X_LPARAM(lParam);
        g_mouseDownPt.y = GET_Y_LPARAM(lParam);
        SetCapture(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        if (g_mouseDown && (wParam & MK_LBUTTON)) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int dx = x - g_mouseDownPt.x;
            int dy = y - g_mouseDownPt.y;
            if ((dx * dx + dy * dy) > 25) {
                g_mouseDown = false;
                ReleaseCapture();
                StartDragIfHasFiles(hwnd);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        g_mouseDown = false;
        ReleaseCapture();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintMain(hwnd);
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            return 0;
        }
        break;

    case WM_SHOWWINDOW:
        if (wParam == FALSE) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd, g_style.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (g_style.healIntervalMs > 0) KillTimer(hwnd, TIMER_HEAL);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- entry ----------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    OleInitialize(NULL);

    // ini path: exe directory + config.ini
    GetModuleFileNameW(NULL, g_iniPath, MAX_PATH);
    wchar_t* slash = wcsrchr(g_iniPath, L'\\');
    if (slash) *(slash + 1) = 0;
    StringCchCatW(g_iniPath, MAX_PATH, L"config.ini");

    if (!FileExists(g_iniPath)) {
        // 先用当前默认 g_style 写出一份 ini
        WriteDefaultIni(g_iniPath);
    }
    LoadIniStyle(g_iniPath);

    // ---- single instance ----
    g_singleMutex = CreateMutexW(NULL, TRUE, L"Global\\FileRelayDock_SingleInstance");
    if (!g_singleMutex) {
        // 创建失败也别硬崩，继续跑（可选：直接退出）
    } else {
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (g_style.showSingleTip) {
                MessageBoxW(NULL, L"程序已经在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
            }
            CloseHandle(g_singleMutex);
            return 0;
        }
    }

    ResolveXY(g_style.x, g_style.y, g_style.w, g_style.h);

    // register main class
    WNDCLASSW wc{};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = MAIN_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    RegisterClassW(&wc);

    // register tip class
    WNDCLASSW twc{};
    twc.lpfnWndProc = TipWndProc;
    twc.hInstance = hInst;
    twc.lpszClassName = TIP_CLASS;
    twc.hCursor = LoadCursor(NULL, IDC_ARROW);
    twc.hbrBackground = NULL;
    RegisterClassW(&twc);

    DWORD ex = WS_EX_TOOLWINDOW;
    if (g_style.topmost) ex |= WS_EX_TOPMOST;
    if (g_style.layered) ex |= WS_EX_LAYERED;

    HWND hwnd = CreateWindowExW(
        ex, MAIN_CLASS, L"", WS_POPUP,
        g_style.x, g_style.y, g_style.w, g_style.h,
        NULL, NULL, hInst, NULL
    );

    ApplyLayeredAttributes(hwnd);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    SetWindowPos(hwnd, g_style.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 g_style.x, g_style.y, g_style.w, g_style.h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 单例互斥体释放（放这里）
    if (g_singleMutex) {
        ReleaseMutex(g_singleMutex);   // 也可以不写这句，仅 CloseHandle 也行
        CloseHandle(g_singleMutex);
        g_singleMutex = NULL;
    }

    if (g_mainFont) DeleteObject(g_mainFont);
    if (g_tipFont)  DeleteObject(g_tipFont);
    if (g_mainBgBrush) DeleteObject(g_mainBgBrush);
    if (g_tipBgBrush)  DeleteObject(g_tipBgBrush);

    OleUninitialize();
    return 0;
}
