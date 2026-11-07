// DiskPilot - gestione partizioni in stile EaseUS Partition Master
// GUI Win32 + GDI+, motore: Windows Storage Management API (WMI)

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dbt.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include "Wmi.h"
#include "DiskModel.h"
#include "Ops.h"
#include "Dialogs.h"
#include "../res/resource.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dbghelp.lib")
#endif

HINSTANCE g_hInst = nullptr;

namespace {

constexpr UINT WM_APP_MODEL = WM_APP + 1;
constexpr UINT WM_APP_STEP = WM_APP + 2;
constexpr UINT WM_APP_DONE = WM_APP + 3;
constexpr UINT_PTR TIMER_DEVCHANGE = 1;
constexpr int ID_LIST = 100;

// ------------------------------------------------------------ colori

namespace Col {
constexpr COLORREF Bg = RGB(245, 245, 247);
constexpr COLORREF Card = RGB(255, 255, 255);
constexpr COLORREF Border = RGB(226, 226, 231);
constexpr COLORREF Text = RGB(29, 29, 31);
constexpr COLORREF Text2 = RGB(110, 110, 115);
constexpr COLORREF Accent = RGB(0, 122, 255);
constexpr COLORREF AccentDown = RGB(0, 98, 210);
constexpr COLORREF Hover = RGB(236, 236, 241);
constexpr COLORREF Pressed = RGB(224, 224, 230);
constexpr COLORREF Disabled = RGB(190, 190, 195);
constexpr COLORREF Orange = RGB(255, 149, 0);
constexpr COLORREF Green = RGB(52, 199, 89);
constexpr COLORREF Red = RGB(255, 59, 48);
constexpr COLORREF FreeBg = RGB(242, 242, 245);
constexpr COLORREF Hatch = RGB(224, 224, 230);
}

// NTFS, FAT, exFAT, ReFS, EFI, Ripristino, MSR/estesa, libero, altro
const COLORREF kPalette[9] = {
    RGB(10, 132, 255), RGB(52, 199, 89), RGB(48, 176, 199), RGB(175, 82, 222), RGB(255, 149, 0),
    RGB(255, 55, 95), RGB(142, 142, 147), RGB(200, 200, 206), RGB(99, 110, 130),
};
constexpr int kFreeColor = 7;

COLORREF Blend(COLORREF a, COLORREF b, float t) {
    auto mix = [t](int x, int y) { return (BYTE)std::lround(x + (y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

Gdiplus::Color GC(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// ------------------------------------------------------------ stato

struct Sel {
    enum Kind { None, Disk, Part, Free } k = None;
    uint32_t disk = 0;
    uint64_t off = 0;
    bool operator==(const Sel& o) const { return k == o.k && disk == o.disk && (k == Disk || off == o.off); }
};

struct Btn {
    int id;
    const wchar_t* glyph;
    const wchar_t* text;
    int kind;            // 0 toolbar, 1 pannello, 2 pannello principale
    RECT rc{};
    bool enabled = true;
};

struct HitItem { RECT rc; Sel sel; };

struct App {
    HWND hwnd = nullptr, hList = nullptr;
    UINT dpi = 96;
    HFONT fNormal = nullptr, fBold = nullptr, fSmall = nullptr, fSmallBold = nullptr, fTitle = nullptr;
    HFONT fIcon = nullptr, fIconBig = nullptr, fBrand = nullptr;
    std::wstring uiFace, iconFace;
    HIMAGELIST il = nullptr;
    HICON icoBrand = nullptr;
    HACCEL accel = nullptr;
    Gdiplus::Graphics* gfx = nullptr;

    Model live, plan;
    std::vector<Op> ops;
    bool loaded = false, refreshing = false, refreshAgain = false, busy = false;
    int step = -1;
    std::wstring loadErr, note;

    Sel sel;
    std::vector<Sel> listRefs;
    bool fillingList = false;

    std::vector<Btn> btns;
    int hot = -1, pressed = -1;
    bool tracking = false;

    RECT rcToolbar{}, rcMap{}, rcList{}, rcPanel{}, rcPanelList{}, rcStatus{};
    int mapScroll = 0, mapContent = 0, panelScroll = 0;
    std::vector<HitItem> hits;

    Wmi uiWmi;
    bool uiWmiTried = false;
} G;

int S(int v) { return MulDiv(v, (int)G.dpi, 96); }
float SF(float v) { return v * (float)G.dpi / 96.0f; }

// ------------------------------------------------------------ utilità

bool FontExists(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
        *(bool*)p = true;
        return 0;
    }, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

HFONT MkFont(int px, int weight, const std::wstring& face) {
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face.c_str());
}

void DeleteFonts() {
    for (HFONT* f : { &G.fNormal, &G.fBold, &G.fSmall, &G.fSmallBold, &G.fTitle, &G.fIcon, &G.fIconBig, &G.fBrand })
        if (*f) { DeleteObject(*f); *f = nullptr; }
}

void CreateFonts() {
    DeleteFonts();
    if (G.uiFace.empty()) {
        G.uiFace = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        G.iconFace = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }
    G.fNormal = MkFont(13, FW_NORMAL, G.uiFace);
    G.fBold = MkFont(13, FW_SEMIBOLD, G.uiFace);
    G.fSmall = MkFont(12, FW_NORMAL, G.uiFace);
    G.fSmallBold = MkFont(12, FW_SEMIBOLD, G.uiFace);
    G.fTitle = MkFont(14, FW_SEMIBOLD, G.uiFace);
    G.fBrand = MkFont(17, FW_SEMIBOLD, G.uiFace);
    G.fIcon = MkFont(16, FW_NORMAL, G.iconFace);
    G.fIconBig = MkFont(20, FW_NORMAL, G.iconFace);
}

// Riquadrino colorato con angoli arrotondati (alpha premoltiplicato) per la lista
HBITMAP MakeSwatch(int sz, COLORREF c, bool hatch) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP h = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!h) return nullptr;
    auto* px = (uint32_t*)bits;
    float m = sz * 0.14f, r = sz * 0.24f;
    float half = (sz - 2 * m) / 2.0f, ctr = sz / 2.0f;
    for (int y = 0; y < sz; ++y)
        for (int x = 0; x < sz; ++x) {
            float dx = std::fabs(x + 0.5f - ctr) - (half - r);
            float dy = std::fabs(y + 0.5f - ctr) - (half - r);
            float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
            float dist = std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - r;
            float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
            COLORREF k = c;
            if (hatch && ((x + y) / 2) % 2) k = Blend(c, RGB(255, 255, 255), 0.55f);
            uint32_t a = (uint32_t)std::lround(cov * 255);
            uint32_t R = (uint32_t)std::lround(GetRValue(k) * cov), Gg = (uint32_t)std::lround(GetGValue(k) * cov),
                     B = (uint32_t)std::lround(GetBValue(k) * cov);
            px[y * sz + x] = (a << 24) | (R << 16) | (Gg << 8) | B;
        }
    return h;
}

void CreateImageList() {
    if (G.il) ImageList_Destroy(G.il);
    int sz = S(16);
    G.il = ImageList_Create(sz, sz, ILC_COLOR32, 9, 0);
    for (int i = 0; i < 9; ++i) {
        HBITMAP b = MakeSwatch(sz, kPalette[i], i == kFreeColor);
        ImageList_Add(G.il, b, nullptr);
        DeleteObject(b);
    }
    if (G.hList) ListView_SetImageList(G.hList, G.il, LVSIL_SMALL);
}

void LoadBrandIcon() {
    if (G.icoBrand) DestroyIcon(G.icoBrand);
    G.icoBrand = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(34), S(34), LR_DEFAULTCOLOR);
}

bool Confirm(const std::wstring& main, const std::wstring& content, const wchar_t* okText, bool danger) {
    TASKDIALOG_BUTTON b[] = { { IDOK, okText }, { IDCANCEL, L"Annulla" } };
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof(c);
    c.hwndParent = G.hwnd;
    c.hInstance = g_hInst;
    c.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = L"DiskPilot";
    c.pszMainIcon = danger ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    c.pszMainInstruction = main.c_str();
    c.pszContent = content.c_str();
    c.cButtons = 2;
    c.pButtons = b;
    c.nDefaultButton = danger ? IDCANCEL : IDOK;
    int r = 0;
    if (FAILED(TaskDialogIndirect(&c, &r, nullptr, nullptr))) return false;
    return r == IDOK;
}

void Info(const std::wstring& main, const std::wstring& content, PCWSTR icon = TD_INFORMATION_ICON) {
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof(c);
    c.hwndParent = G.hwnd;
    c.hInstance = g_hInst;
    c.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = L"DiskPilot";
    c.pszMainIcon = icon;
    c.pszMainInstruction = main.c_str();
    c.pszContent = content.c_str();
    c.dwCommonButtons = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
}

std::wstring Quoted(const PartInfo& p) {
    std::wstring s = PartShortName(p);
    if (!p.label.empty()) s += L" «" + p.label + L"»";
    else if (!p.letter) {
        std::wstring k = KindName(p.kind);
        if (!k.empty()) s += L" (" + k + L")";
    }
    return s;
}

std::wstring DiskName(uint32_t n) { return L"Disco " + std::to_wstring(n); }

// ------------------------------------------------------------ selezione / modello

DiskInfo* SelDisk() { return G.sel.k == Sel::None ? nullptr : FindDisk(G.plan, G.sel.disk); }

PartInfo* SelPart() {
    if (G.sel.k != Sel::Part) return nullptr;
    DiskInfo* d = SelDisk();
    return d ? FindPart(*d, G.sel.off) : nullptr;
}

bool SelGap(Gap& out) {
    if (G.sel.k != Sel::Free) return false;
    DiskInfo* d = SelDisk();
    if (!d) return false;
    for (const auto& g : ComputeGaps(*d))
        if (g.offset == G.sel.off) { out = g; return true; }
    return false;
}

void ValidateSel() {
    DiskInfo* d = SelDisk();
    if (!d) { G.sel = {}; return; }
    if (G.sel.k == Sel::Part && !FindPart(*d, G.sel.off)) {
        // se la partizione è sparita, seleziona lo spazio libero che la sostituisce
        for (const auto& g : ComputeGaps(*d))
            if (G.sel.off >= g.offset && G.sel.off < g.offset + g.size) { G.sel.k = Sel::Free; G.sel.off = g.offset; return; }
        G.sel.k = Sel::Disk;
    }
    if (G.sel.k == Sel::Free) {
        Gap g;
        if (SelGap(g)) return;
        for (const auto& x : ComputeGaps(*d))
            if (G.sel.off >= x.offset && G.sel.off < x.offset + x.size) { G.sel.off = x.offset; return; }
        G.sel.k = Sel::Disk;
    }
}

void RebuildPlan() {
    G.plan = G.live;
    for (const auto& op : G.ops) SimulateOp(G.plan, op);
    ValidateSel();
}

std::vector<wchar_t> FreeLetters(wchar_t keep) {
    bool used[26] = {};
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) if (mask & (1u << i)) used[i] = true;
    for (const auto& d : G.plan.disks)
        for (const auto& p : d.parts)
            if (p.letter >= L'A' && p.letter <= L'Z') used[p.letter - L'A'] = true;
    std::vector<wchar_t> r;
    for (wchar_t c = L'D'; c <= L'Z'; ++c)
        if (!used[c - L'A'] || c == keep) r.push_back(c);
    return r;
}

bool LiveLetterExists(wchar_t c) {
    return c >= L'A' && c <= L'Z' && (GetLogicalDrives() & (1u << (c - L'A')));
}

int PrimaryCount(const DiskInfo& d) {
    int n = 0;
    for (const auto& p : d.parts) if (p.kind != PartKind::Logical) ++n;
    return n;
}

bool EmptyForConvert(const DiskInfo& d) {
    for (const auto& p : d.parts) if (p.kind != PartKind::Msr) return false;
    return true;
}

uint64_t UsableEnd(const DiskInfo& d) {
    uint64_t end = d.size;
    if (d.style == STYLE_GPT) end = d.size > 34ull * d.sector ? d.size - 34ull * d.sector : 0;
    return AlignDown(end, MiB);
}

// ------------------------------------------------------------ abilitazione comandi

bool Enabled(int id) {
    switch (id) {
    case IDM_REFRESH: return !G.busy && !G.refreshing;
    case IDM_APPLY:
    case IDM_UNDO:
    case IDM_UNDO_ALL: return !G.busy && !G.ops.empty();
    case IDM_MORE: return !G.busy;
    case IDM_ABOUT:
    case IDM_OPENLOG: return true;
    default: break;
    }
    if (G.busy || !G.loaded) return false;
    DiskInfo* d = SelDisk();
    if (!d) return false;
    bool diskOk = !d->barrier && !d->offline && !d->readOnly && !IsDynamicOrSpaces(*d);
    PartInfo* p = SelPart();
    bool dataLike = p && (p->kind == PartKind::Data || p->kind == PartKind::Logical || p->kind == PartKind::Other);
    switch (id) {
    case IDM_CREATE: return diskOk && G.sel.k == Sel::Free;
    case IDM_DELETE: return diskOk && p && !IsProtected(*p);
    case IDM_FORMAT: return diskOk && p && !IsProtected(*p) && (dataLike || p->kind == PartKind::Recovery);
    case IDM_RESIZE: return diskOk && p && !p->isNew && p->hasVolume && (p->kind == PartKind::Data || p->kind == PartKind::Logical);
    case IDM_LETTER: return diskOk && p && p->hasVolume && !IsProtected(*p) && dataLike;
    case IDM_LABEL: return diskOk && p && p->hasVolume && !p->fs.empty();
    case IDM_ACTIVE: return diskOk && p && d->style == STYLE_MBR && p->kind == PartKind::Data && !p->isActive;
    case IDM_EXPLORE: return p && p->letter && !p->isNew && !p->pending && LiveLetterExists(p->letter);
    case IDM_INIT_GPT:
    case IDM_INIT_MBR: return !d->barrier && !d->offline && !d->readOnly && d->style == STYLE_RAW;
    case IDM_CONV_GPT:
        return diskOk && d->style == STYLE_MBR && EmptyForConvert(*d) && !d->isBoot && !d->isSystem;
    case IDM_CONV_MBR:
        return diskOk && d->style == STYLE_GPT && EmptyForConvert(*d) && !d->isBoot && !d->isSystem && d->size <= 2048ull * GiB;
    case IDM_WIPE:
        return diskOk && d->style != STYLE_RAW && !d->parts.empty() && !d->isBoot && !d->isSystem;
    }
    return false;
}

void UpdateButtons() {
    for (auto& b : G.btns) b.enabled = Enabled(b.id);
}

// ------------------------------------------------------------ lista

struct Col_ { const wchar_t* name; int width; int fmt; };
const Col_ kCols[] = {
    { L"Partizione", 210, LVCFMT_LEFT }, { L"File system", 86, LVCFMT_LEFT }, { L"Capacità", 92, LVCFMT_RIGHT },
    { L"Usato", 118, LVCFMT_RIGHT }, { L"Libero", 92, LVCFMT_RIGHT }, { L"Tipo", 130, LVCFMT_LEFT },
    { L"Stato", 190, LVCFMT_LEFT },
};

void SetupColumns() {
    while (ListView_DeleteColumn(G.hList, 0)) {}
    for (int i = 0; i < (int)(sizeof(kCols) / sizeof(kCols[0])); ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = (LPWSTR)kCols[i].name;
        c.cx = S(kCols[i].width);
        c.fmt = i == 0 ? LVCFMT_LEFT : kCols[i].fmt;
        ListView_InsertColumn(G.hList, i, &c);
    }
}

void AddRow(int group, int image, int indent, const Sel& ref, const std::vector<std::wstring>& cells) {
    G.listRefs.push_back(ref);
    LVITEMW it{};
    it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE | LVIF_GROUPID | LVIF_INDENT;
    it.iItem = ListView_GetItemCount(G.hList);
    it.pszText = (LPWSTR)cells[0].c_str();
    it.lParam = (LPARAM)(G.listRefs.size() - 1);
    it.iImage = image;
    it.iIndent = indent;
    it.iGroupId = group;
    int idx = ListView_InsertItem(G.hList, &it);
    for (size_t c = 1; c < cells.size(); ++c) ListView_SetItemText(G.hList, idx, (int)c, (LPWSTR)cells[c].c_str());
}

void SelectInList() {
    G.fillingList = true;
    int n = ListView_GetItemCount(G.hList);
    int found = -1;
    for (int i = 0; i < n; ++i) {
        LVITEMW it{};
        it.mask = LVIF_PARAM;
        it.iItem = i;
        ListView_GetItem(G.hList, &it);
        if ((size_t)it.lParam < G.listRefs.size() && G.listRefs[it.lParam] == G.sel) { found = i; break; }
    }
    ListView_SetItemState(G.hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (found >= 0) {
        ListView_SetItemState(G.hList, found, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(G.hList, found, FALSE);
    }
    G.fillingList = false;
}

void RebuildList() {
    G.fillingList = true;
    SendMessageW(G.hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(G.hList);
    ListView_RemoveAllGroups(G.hList);
    G.listRefs.clear();
    int gid = 0;
    for (const auto& d : G.plan.disks) {
        std::wstring hdr = DiskTitle(d);
        if (d.pending) hdr += L"  ·  modifiche in sospeso";
        LVGROUP grp{};
        grp.cbSize = sizeof(grp);
        grp.mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE;
        grp.pszHeader = (LPWSTR)hdr.c_str();
        grp.iGroupId = gid;
        grp.stateMask = LVGS_COLLAPSIBLE;
        grp.state = LVGS_COLLAPSIBLE;
        ListView_InsertGroup(G.hList, -1, &grp);

        Sel diskRef{ Sel::Disk, d.number, 0 };
        if (d.style == STYLE_RAW && !d.barrier) {
            AddRow(gid, kFreeColor, 0, diskRef, { L"Disco non inizializzato", L"—", FormatSize(d.size), L"—", L"—", L"Disco", L"Da inizializzare" });
        } else if (d.parts.empty() && d.barrier) {
            AddRow(gid, kFreeColor, 0, diskRef, { L"Disco da preparare", L"—", FormatSize(d.size), L"—", L"—", StyleName(d.style), L"Operazione sul disco in sospeso" });
        }

        // righe in ordine di offset: partizioni (estesa compresa) e spazi liberi
        struct Row { uint64_t off; const PartInfo* p; Gap g; };
        std::vector<Row> rows;
        for (const auto& p : d.parts) rows.push_back({ p.offset, &p, {} });
        for (const auto& g : ComputeGaps(d)) rows.push_back({ g.offset, nullptr, g });
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.off != b.off) return a.off < b.off;
            return a.p && a.p->kind == PartKind::Extended;
        });
        for (const auto& r : rows) {
            if (r.p) {
                const PartInfo& p = *r.p;
                std::wstring used = L"—", freeS = L"—";
                if (p.hasVolume && p.volSize && !p.fs.empty()) {
                    uint64_t u = p.volSize > p.volFree ? p.volSize - p.volFree : 0;
                    int pct = (int)std::lround(100.0 * (double)u / (double)p.volSize);
                    used = FormatSize(u) + L"  (" + std::to_wstring(pct) + L"%)";
                    freeS = FormatSize(p.volFree);
                }
                int indent = p.kind == PartKind::Logical ? 1 : 0;
                AddRow(gid, ColorIndex(p), indent, { Sel::Part, d.number, p.offset },
                       { PartTitle(p), FsText(p), FormatSize(p.size), used, freeS, TypeText(d, p), StateText(p) });
            } else {
                AddRow(gid, kFreeColor, r.g.inExt ? 1 : 0, { Sel::Free, d.number, r.g.offset },
                       { L"Non allocato", L"—", FormatSize(r.g.size), L"—", L"—",
                         r.g.inExt ? L"Libero (nell'estesa)" : L"Spazio libero", L"" });
            }
        }
        ++gid;
    }
    SendMessageW(G.hList, WM_SETREDRAW, TRUE, 0);
    G.fillingList = false;
    SelectInList();
    InvalidateRect(G.hList, nullptr, TRUE);
}

// ------------------------------------------------------------ layout

constexpr int kCardH = 118, kCardGap = 12;

void LayoutButtons() {
    int x = S(214), y = S(6), w = S(78), h = S(58);
    for (auto& b : G.btns) {
        if (b.kind != 0) continue;
        if (b.id == IDM_CREATE || b.id == IDM_MORE) x += S(14); // separatori
        b.rc = { x, y, x + w, y + h };
        x += w + S(2);
    }
    // pannello: Applica in basso, sopra le due di annullamento
    int pl = G.rcPanel.left + S(16), pr = G.rcPanel.right - S(16), pb = G.rcPanel.bottom - S(16);
    for (auto& b : G.btns) {
        if (b.id == IDM_APPLY) b.rc = { pl, pb - S(40), pr, pb };
        int mid = (pl + pr) / 2;
        if (b.id == IDM_UNDO) b.rc = { pl, pb - S(40) - S(10) - S(32), mid - S(4), pb - S(40) - S(10) };
        if (b.id == IDM_UNDO_ALL) b.rc = { mid + S(4), pb - S(40) - S(10) - S(32), pr, pb - S(40) - S(10) };
    }
    G.rcPanelList = { G.rcPanel.left, G.rcPanel.top + S(56), G.rcPanel.right, pb - S(40) - S(10) - S(32) - S(12) };
}

void Layout() {
    if (!G.hwnd) return;
    RECT c;
    GetClientRect(G.hwnd, &c);
    int tbH = S(70), stH = S(28), pW = S(310), m = S(14);
    G.rcToolbar = { 0, 0, c.right, tbH };
    G.rcStatus = { 0, c.bottom - stH, c.right, c.bottom };
    G.rcPanel = { c.right - pW, tbH, c.right, G.rcStatus.top };
    int left = m, right = G.rcPanel.left - m, top = tbH + m, bottom = G.rcStatus.top - m;
    int n = G.loaded ? (int)G.plan.disks.size() : 1;
    G.mapContent = std::max(1, n) * (S(kCardH) + S(kCardGap)) - S(kCardGap);
    int avail = std::max(0, bottom - top);
    int mapH = std::min(G.mapContent, avail * 60 / 100);
    if (mapH < S(kCardH)) mapH = std::min(S(kCardH), avail / 2);
    G.rcMap = { left, top, right, top + mapH };
    G.rcList = { left, G.rcMap.bottom + S(12), right, bottom };
    int maxScroll = std::max(0, G.mapContent - mapH);
    G.mapScroll = std::clamp(G.mapScroll, 0, maxScroll);
    int in = S(6);
    MoveWindow(G.hList, G.rcList.left + in, G.rcList.top + in, std::max(0, (int)(G.rcList.right - G.rcList.left) - 2 * in),
               std::max(0, (int)(G.rcList.bottom - G.rcList.top) - 2 * in), TRUE);
    LayoutButtons();
}

void RefreshUi() {
    RebuildList();
    Layout();
    UpdateButtons();
    InvalidateRect(G.hwnd, nullptr, FALSE);
}

// ------------------------------------------------------------ disegno

void Flush() { if (G.gfx) G.gfx->Flush(Gdiplus::FlushIntentionSync); }

void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f, UINT fmt) {
    Flush();
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
}

int TextWidth(HDC dc, const std::wstring& s, HFONT f) {
    SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    return sz.cx;
}

void RoundPath(Gdiplus::GraphicsPath& p, Gdiplus::RectF r, float rad) {
    float d = std::min({ rad * 2, r.Width, r.Height });
    if (d <= 0.5f) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

Gdiplus::RectF RF(const RECT& r) {
    return Gdiplus::RectF((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
}

void FillRound(const Gdiplus::RectF& r, float rad, COLORREF c, BYTE a = 255) {
    Gdiplus::GraphicsPath p;
    RoundPath(p, r, rad);
    Gdiplus::SolidBrush b(GC(c, a));
    G.gfx->FillPath(&b, &p);
}

void StrokeRound(const Gdiplus::RectF& r, float rad, COLORREF c, float w) {
    Gdiplus::GraphicsPath p;
    RoundPath(p, r, rad);
    Gdiplus::Pen pen(GC(c), w);
    G.gfx->DrawPath(&pen, &p);
}

void Pill(HDC dc, int& right, int top, const std::wstring& s, COLORREF c) {
    int w = TextWidth(dc, s, G.fSmallBold) + S(16);
    RECT r{ right - w, top, right, top + S(20) };
    FillRound(RF(r), SF(10), Blend(c, RGB(255, 255, 255), 0.85f));
    Text(dc, s, r, c, G.fSmallBold, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    right -= w + S(6);
}

void PaintButton(HDC dc, const Btn& b, int index) {
    bool hot = index == G.hot && b.enabled, down = index == G.pressed && b.enabled && hot;
    Gdiplus::RectF r = RF(b.rc);
    if (b.kind == 0) {
        if (down) FillRound(r, SF(8), Col::Pressed);
        else if (hot) FillRound(r, SF(8), Col::Hover);
        COLORREF fg = b.enabled ? Col::Text : Col::Disabled;
        COLORREF gl = b.enabled ? (b.id == IDM_DELETE ? Col::Red : Col::Accent) : Col::Disabled;
        RECT gi = b.rc; gi.top += S(7); gi.bottom = gi.top + S(26);
        Text(dc, b.glyph, gi, gl, G.fIconBig, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT tr = b.rc; tr.top = gi.bottom + S(3); tr.bottom = b.rc.bottom - S(3);
        Text(dc, b.text, tr, fg, G.fSmall, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else if (b.kind == 2) {
        COLORREF bg = !b.enabled ? Blend(Col::Accent, RGB(255, 255, 255), 0.65f) : down ? Col::AccentDown
                    : hot ? Blend(Col::Accent, RGB(0, 0, 0), 0.08f) : Col::Accent;
        FillRound(r, SF(9), bg);
        std::wstring t = std::wstring(b.glyph) + L"  ";
        RECT tr = b.rc;
        int gw = TextWidth(dc, t, G.fIcon), tw = TextWidth(dc, b.text, G.fBold);
        int x = (tr.left + tr.right - gw - tw) / 2;
        RECT g1{ x, tr.top, x + gw, tr.bottom }, t1{ x + gw, tr.top, x + gw + tw + S(2), tr.bottom };
        Text(dc, t, g1, RGB(255, 255, 255), G.fIcon, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, b.text, t1, RGB(255, 255, 255), G.fBold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        FillRound(r, SF(8), down ? Col::Pressed : hot ? Col::Hover : Col::Card);
        StrokeRound(Gdiplus::RectF(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1), SF(8), Col::Border, 1.0f);
        Text(dc, b.text, b.rc, b.enabled ? Col::Text : Col::Disabled, G.fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void PaintToolbar(HDC dc) {
    RECT r = G.rcToolbar;
    Gdiplus::SolidBrush wb(GC(Col::Card));
    G.gfx->FillRectangle(&wb, RF(r));
    Gdiplus::SolidBrush line(GC(Col::Border));
    G.gfx->FillRectangle(&line, (float)r.left, (float)r.bottom - 1, (float)(r.right - r.left), 1.0f);
    int cy = (r.top + r.bottom) / 2;
    Flush();
    if (G.icoBrand) DrawIconEx(dc, S(18), cy - S(17), G.icoBrand, S(34), S(34), 0, nullptr, DI_NORMAL);
    RECT t1{ S(62), cy - S(19), S(210), cy + S(1) };
    Text(dc, L"DiskPilot", t1, Col::Text, G.fBrand, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    RECT t2{ S(62), cy + S(1), S(210), cy + S(19) };
    Text(dc, L"Gestione partizioni", t2, Col::Text2, G.fSmall, DT_LEFT | DT_TOP | DT_SINGLELINE);
    for (size_t i = 0; i < G.btns.size(); ++i)
        if (G.btns[i].kind == 0) {
            if (G.btns[i].id == IDM_CREATE || G.btns[i].id == IDM_MORE) {
                int x = G.btns[i].rc.left - S(8);
                G.gfx->FillRectangle(&line, (float)x, (float)(cy - S(18)), 1.0f, (float)S(36));
            }
            PaintButton(dc, G.btns[i], (int)i);
        }
}

std::vector<int> SegWidths(const std::vector<Seg>& segs, int total, int minW, int gap) {
    size_t n = segs.size();
    std::vector<int> w(n, 0);
    if (n == 0) return w;
    int avail = total - gap * (int)(n - 1);
    if ((int)n * minW >= avail) {
        for (size_t i = 0; i < n; ++i) w[i] = std::max(1, avail / (int)n);
        return w;
    }
    std::vector<bool> fixed(n, false);
    for (size_t pass = 0; pass <= n; ++pass) {
        double sum = 0;
        int rem = avail;
        for (size_t i = 0; i < n; ++i) {
            if (fixed[i]) rem -= minW;
            else sum += (double)segs[i].Size();
        }
        bool changed = false;
        for (size_t i = 0; i < n; ++i) {
            if (fixed[i]) continue;
            double px = sum > 0 ? rem * (double)segs[i].Size() / sum : 0;
            if (px < minW) { fixed[i] = true; changed = true; }
        }
        if (!changed) {
            int acc = 0;
            for (size_t i = 0; i < n; ++i) {
                w[i] = fixed[i] ? minW : (int)(sum > 0 ? rem * (double)segs[i].Size() / sum : 0);
                acc += w[i];
            }
            w[n - 1] += avail - acc;
            break;
        }
    }
    return w;
}

void PaintSegment(HDC dc, const RECT& rc, const DiskInfo& d, const Seg& s, bool selected) {
    Gdiplus::RectF r = RF(rc);
    float rad = SF(7);
    std::wstring l1, l2;
    if (s.isFree) {
        FillRound(r, rad, Col::FreeBg);
        Gdiplus::GraphicsPath path;
        RoundPath(path, r, rad);
        Gdiplus::Region clip(&path);
        G.gfx->SetClip(&clip, Gdiplus::CombineModeIntersect);
        Gdiplus::HatchBrush hb(Gdiplus::HatchStyleWideUpwardDiagonal, GC(Col::Hatch), GC(Col::FreeBg));
        G.gfx->FillRectangle(&hb, r);
        G.gfx->ResetClip();
        G.gfx->SetClip(RF(G.rcMap));
        StrokeRound(Gdiplus::RectF(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1), rad, Col::Border, 1.0f);
        l1 = s.gap.inExt ? L"Libero (estesa)" : L"Non allocato";
        l2 = FormatSize(s.gap.size);
        if (s.gap.offset == 0 && s.gap.size == d.size) { // disco intero senza tabella o in attesa
            l1 = d.style == STYLE_RAW && !d.barrier ? L"Disco non inizializzato" : L"Disco in preparazione";
            l2 = d.style == STYLE_RAW && !d.barrier ? L"Clic destro per inizializzarlo come GPT o MBR"
                                                    : L"Le partizioni compariranno dopo Applica";
        }
    } else {
        const PartInfo& p = *s.part;
        COLORREF c = kPalette[ColorIndex(p)];
        FillRound(r, rad, Blend(c, RGB(255, 255, 255), 0.86f));
        Gdiplus::GraphicsPath path;
        RoundPath(path, r, rad);
        Gdiplus::Region clip(&path);
        G.gfx->SetClip(&clip, Gdiplus::CombineModeIntersect);
        if (p.hasVolume && p.volSize && !p.fs.empty()) {
            double used = 1.0 - (double)p.volFree / (double)p.volSize;
            used = std::clamp(used, 0.0, 1.0);
            Gdiplus::SolidBrush ub(GC(Blend(c, RGB(255, 255, 255), 0.62f)));
            G.gfx->FillRectangle(&ub, r.X, r.Y, (float)(r.Width * used), r.Height);
        }
        Gdiplus::SolidBrush top(GC(c));
        G.gfx->FillRectangle(&top, r.X, r.Y, r.Width, SF(4));
        G.gfx->ResetClip();
        G.gfx->SetClip(RF(G.rcMap));
        if (!selected) StrokeRound(Gdiplus::RectF(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1), rad,
                                   Blend(c, RGB(255, 255, 255), 0.55f), 1.0f);
        if (p.pending) {
            Gdiplus::SolidBrush ob(GC(Col::Orange));
            float dsz = SF(7);
            G.gfx->FillEllipse(&ob, r.X + r.Width - dsz - SF(5), r.Y + SF(8), dsz, dsz);
        }
        l1 = PartTitle(p);
        std::wstring fs = FsText(p);
        l2 = (fs == L"—" ? std::wstring() : fs + L" · ") + FormatSize(p.size);
    }
    if (selected) {
        StrokeRound(Gdiplus::RectF(r.X + 1, r.Y + 1, r.Width - 2, r.Height - 2), rad, Col::Accent, SF(2.2f));
    }
    if (rc.right - rc.left >= S(34)) {
        RECT t = rc;
        InflateRect(&t, -S(8), 0);
        int mid = (rc.top + rc.bottom) / 2 + S(2);
        RECT a{ t.left, rc.top + S(6), t.right, mid };
        RECT b{ t.left, mid, t.right, rc.bottom - S(4) };
        Text(dc, l1, a, Col::Text, G.fSmallBold, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, l2, b, Col::Text2, G.fSmall, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void PaintMap(HDC dc) {
    G.hits.clear();
    int saved = SaveDC(dc);
    IntersectClipRect(dc, G.rcMap.left, G.rcMap.top, G.rcMap.right, G.rcMap.bottom);
    G.gfx->SetClip(RF(G.rcMap));

    if (!G.loaded) {
        RECT card{ G.rcMap.left, G.rcMap.top, G.rcMap.right, G.rcMap.top + S(kCardH) };
        FillRound(RF(card), SF(12), Col::Card);
        std::wstring t = G.loadErr.empty() ? L"Lettura dei dischi in corso…" : L"Impossibile leggere i dischi:\n" + G.loadErr;
        Text(dc, t, card, G.loadErr.empty() ? Col::Text2 : Col::Red, G.fNormal, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        G.gfx->ResetClip();
        RestoreDC(dc, saved);
        return;
    }

    int y = G.rcMap.top - G.mapScroll;
    for (const auto& d : G.plan.disks) {
        RECT card{ G.rcMap.left, y, G.rcMap.right, y + S(kCardH) };
        y += S(kCardH) + S(kCardGap);
        if (card.bottom < G.rcMap.top || card.top > G.rcMap.bottom) continue;
        bool diskSel = G.sel.k == Sel::Disk && G.sel.disk == d.number;
        Gdiplus::RectF cr = RF(card);
        FillRound(cr, SF(12), Col::Card);
        StrokeRound(Gdiplus::RectF(cr.X + 0.5f, cr.Y + 0.5f, cr.Width - 1, cr.Height - 1), SF(12),
                    diskSel ? Col::Accent : Col::Border, diskSel ? SF(2) : 1.0f);

        // intestazione
        RECT ic{ card.left + S(14), card.top + S(10), card.left + S(40), card.top + S(36) };
        Text(dc, L"", ic, Col::Text2, G.fIconBig, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        std::wstring name = DiskName(d.number);
        int nw = TextWidth(dc, name, G.fTitle);
        RECT nr{ card.left + S(46), card.top + S(10), card.left + S(46) + nw + S(4), card.top + S(36) };
        Text(dc, name, nr, Col::Text, G.fTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        std::wstring det = d.name + L"  ·  " + FormatSize(d.size) + L"  ·  " + StyleName(d.style);
        std::wstring bus = BusName(d.bus);
        if (!bus.empty()) det += L"  ·  " + bus;
        int pillRight = card.right - S(14);
        if (d.pending) Pill(dc, pillRight, card.top + S(13), L"In sospeso", Col::Orange);
        if (d.offline) Pill(dc, pillRight, card.top + S(13), L"Offline", Col::Red);
        if (d.readOnly) Pill(dc, pillRight, card.top + S(13), L"Sola lettura", Col::Red);
        if (IsDynamicOrSpaces(d)) Pill(dc, pillRight, card.top + S(13), L"Dinamico", kPalette[8]);
        if (d.isBoot || d.isSystem) Pill(dc, pillRight, card.top + S(13), L"Sistema", kPalette[0]);
        RECT dr{ nr.right + S(10), card.top + S(10), pillRight - S(4), card.top + S(36) };
        Text(dc, det, dr, Col::Text2, G.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        G.hits.push_back({ { card.left, card.top, card.right, card.top + S(44) }, { Sel::Disk, d.number, 0 } });

        // barra delle partizioni
        RECT bar{ card.left + S(14), card.top + S(46), card.right - S(14), card.bottom - S(14) };
        if (d.style == STYLE_RAW || (d.parts.empty() && d.barrier)) {
            Seg fake;
            fake.isFree = true;
            fake.gap = { 0, d.size, false };
            PaintSegment(dc, bar, d, fake, diskSel);
            G.hits.push_back({ bar, { Sel::Disk, d.number, 0 } });
            continue;
        }
        auto segs = BuildSegments(d);
        auto widths = SegWidths(segs, bar.right - bar.left, S(46), S(4));
        int x = bar.left;
        for (size_t i = 0; i < segs.size(); ++i) {
            RECT sr{ x, bar.top, x + widths[i], bar.bottom };
            x += widths[i] + S(4);
            Sel ref = segs[i].isFree ? Sel{ Sel::Free, d.number, segs[i].gap.offset }
                                     : Sel{ Sel::Part, d.number, segs[i].part->offset };
            PaintSegment(dc, sr, d, segs[i], ref == G.sel);
            G.hits.push_back({ sr, ref });
        }
    }

    // indicatore di scorrimento
    int viewH = G.rcMap.bottom - G.rcMap.top;
    if (G.mapContent > viewH) {
        float th = (float)viewH * viewH / G.mapContent;
        float ty = G.rcMap.top + (float)G.mapScroll * (viewH - th) / (G.mapContent - viewH);
        FillRound(Gdiplus::RectF((float)G.rcMap.right - SF(5), ty + SF(2), SF(3), th - SF(4)), SF(1.5f), RGB(0, 0, 0), 60);
    }
    G.gfx->ResetClip();
    RestoreDC(dc, saved);
}

void PaintPanel(HDC dc) {
    RECT r = G.rcPanel;
    Gdiplus::SolidBrush wb(GC(Col::Card));
    G.gfx->FillRectangle(&wb, RF(r));
    Gdiplus::SolidBrush line(GC(Col::Border));
    G.gfx->FillRectangle(&line, (float)r.left, (float)r.top, 1.0f, (float)(r.bottom - r.top));

    RECT t{ r.left + S(18), r.top + S(16), r.right - S(16), r.top + S(40) };
    Text(dc, L"Operazioni in sospeso", t, Col::Text, G.fTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!G.ops.empty()) {
        int pr = t.right;
        Pill(dc, pr, t.top + S(2), std::to_wstring(G.ops.size()), Col::Accent);
    }

    RECT lr = G.rcPanelList;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, lr.left, lr.top, lr.right, lr.bottom);
    G.gfx->SetClip(RF(lr));
    if (G.ops.empty()) {
        RECT ic{ lr.left, lr.top + S(30), lr.right, lr.top + S(70) };
        Text(dc, L"", ic, Col::Disabled, G.fIconBig, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT tx{ lr.left + S(28), ic.bottom + S(4), lr.right - S(28), lr.bottom };
        Text(dc, L"Nessuna operazione.\n\nLe modifiche vengono messe in coda e i dischi cambiano solo quando premi Applica.",
             tx, Col::Text2, G.fSmall, DT_CENTER | DT_TOP | DT_WORDBREAK);
    } else {
        int rowH = S(54);
        int y = lr.top - G.panelScroll;
        for (size_t i = 0; i < G.ops.size(); ++i, y += rowH) {
            if (y + rowH < lr.top || y > lr.bottom) continue;
            RECT row{ lr.left + S(10), y, lr.right - S(10), y + rowH - S(4) };
            bool cur = G.busy && (int)i == G.step;
            if (cur) FillRound(RF(row), SF(8), Blend(Col::Accent, RGB(255, 255, 255), 0.9f));
            float cs = SF(22);
            Gdiplus::RectF circ((float)row.left + SF(8), (float)(row.top + row.bottom) / 2 - cs / 2, cs, cs);
            bool done = G.busy && (int)i < G.step;
            COLORREF cc = done ? Col::Green : cur ? Col::Accent : Blend(Col::Accent, RGB(255, 255, 255), 0.85f);
            Gdiplus::SolidBrush cb(GC(cc));
            G.gfx->FillEllipse(&cb, circ);
            RECT cr{ (int)circ.X, (int)circ.Y, (int)(circ.X + circ.Width), (int)(circ.Y + circ.Height) };
            if (done) Text(dc, L"", cr, RGB(255, 255, 255), G.fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            else {
                SelectObject(dc, G.fSmallBold);
                Text(dc, std::to_wstring(i + 1), cr, cur ? RGB(255, 255, 255) : Col::Accent, G.fSmallBold,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            RECT tx{ cr.right + S(10), row.top + S(4), row.right - S(6), row.bottom - S(4) };
            RECT calc = tx;
            Flush();
            SelectObject(dc, G.fSmall);
            DrawTextW(dc, G.ops[i].desc.c_str(), -1, &calc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
            int th = std::min<int>(calc.bottom - calc.top, tx.bottom - tx.top);
            tx.top = (row.top + row.bottom - th) / 2;
            tx.bottom = tx.top + th;
            Text(dc, G.ops[i].desc, tx, done ? Col::Text2 : Col::Text, G.fSmall, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
            if (i + 1 < G.ops.size() && !cur) {
                G.gfx->FillRectangle(&line, (float)tx.left, (float)(y + rowH - S(2)), (float)(row.right - tx.left), 1.0f);
            }
        }
    }
    G.gfx->ResetClip();
    RestoreDC(dc, saved);

    for (size_t i = 0; i < G.btns.size(); ++i)
        if (G.btns[i].kind != 0) PaintButton(dc, G.btns[i], (int)i);
}

void PaintStatus(HDC dc) {
    RECT r = G.rcStatus;
    Gdiplus::SolidBrush bg(GC(Col::Bg));
    G.gfx->FillRectangle(&bg, RF(r));
    Gdiplus::SolidBrush line(GC(Col::Border));
    G.gfx->FillRectangle(&line, (float)r.left, (float)r.top, (float)(r.right - r.left), 1.0f);
    std::wstring s;
    COLORREF dot = Col::Green;
    if (G.busy && G.step >= 0 && G.step < (int)G.ops.size()) {
        s = L"Esecuzione " + std::to_wstring(G.step + 1) + L" di " + std::to_wstring(G.ops.size()) + L":  " + G.ops[G.step].desc;
        dot = Col::Accent;
    } else if (G.refreshing && !G.loaded) { s = L"Lettura dei dischi…"; dot = Col::Accent; }
    else if (!G.loadErr.empty()) { s = L"Errore: " + G.loadErr; dot = Col::Red; }
    else if (!G.ops.empty()) {
        s = std::to_wstring(G.ops.size()) + (G.ops.size() == 1 ? L" operazione in sospeso" : L" operazioni in sospeso") +
            L" — i dischi non sono ancora stati modificati";
        dot = Col::Orange;
    } else s = G.note.empty() ? L"Pronto" : G.note;
    float ds = SF(8);
    Gdiplus::SolidBrush db(GC(dot));
    G.gfx->FillEllipse(&db, (float)S(16), (float)(r.top + r.bottom) / 2 - ds / 2, ds, ds);
    RECT tr{ S(32), r.top, r.right - S(260), r.bottom };
    Text(dc, s, tr, Col::Text2, G.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    std::wstring right = std::to_wstring(G.live.disks.size()) + (G.live.disks.size() == 1 ? L" disco" : L" dischi") +
                         L"  ·  Amministratore";
    RECT rr{ r.right - S(260), r.top, r.right - S(16), r.bottom };
    Text(dc, right, rr, Col::Text2, G.fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void Paint(HDC hdc, const RECT& client) {
    int w = client.right, h = client.bottom;
    if (w <= 0 || h <= 0) return;
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, G.fNormal);
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        G.gfx = &g;
        Gdiplus::SolidBrush bg(GC(Col::Bg));
        g.FillRectangle(&bg, 0, 0, w, h);
        PaintToolbar(dc);
        PaintMap(dc);
        // cornice della lista
        FillRound(RF(G.rcList), SF(12), Col::Card);
        StrokeRound(Gdiplus::RectF((float)G.rcList.left + 0.5f, (float)G.rcList.top + 0.5f,
                                   (float)(G.rcList.right - G.rcList.left) - 1, (float)(G.rcList.bottom - G.rcList.top) - 1),
                    SF(12), Col::Border, 1.0f);
        PaintPanel(dc);
        PaintStatus(dc);
        g.Flush(Gdiplus::FlushIntentionSync);
        G.gfx = nullptr;
    }
    BitBlt(hdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ------------------------------------------------------------ thread

DWORD WINAPI RefreshThread(LPVOID param) {
    HWND h = (HWND)param;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* m = new Model;
    auto* e = new std::wstring;
    {
        Wmi w;
        if (w.Connect(*e) && EnumerateModel(w, *m, *e)) e->clear();
    }
    CoUninitialize();
    if (!PostMessageW(h, WM_APP_MODEL, (WPARAM)e, (LPARAM)m)) { delete m; delete e; }
    return 0;
}

void StartRefresh() {
    if (G.refreshing) { G.refreshAgain = true; return; }
    G.refreshing = true;
    UpdateButtons();
    HANDLE t = CreateThread(nullptr, 0, RefreshThread, G.hwnd, 0, nullptr);
    if (t) CloseHandle(t);
    else G.refreshing = false;
    InvalidateRect(G.hwnd, nullptr, FALSE);
}

struct ApplyJob { HWND hwnd; std::vector<Op> ops; };

DWORD WINAPI ApplyThread(LPVOID param) {
    std::unique_ptr<ApplyJob> job((ApplyJob*)param);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int failed = -1;
    auto* err = new std::wstring;
    {
        Wmi w;
        if (!w.Connect(*err)) failed = 0;
        else {
            LogLine(L"=== Applicazione di " + std::to_wstring(job->ops.size()) + L" operazioni ===");
            for (size_t i = 0; i < job->ops.size(); ++i) {
                PostMessageW(job->hwnd, WM_APP_STEP, (WPARAM)i, 0);
                if (!ExecuteOp(w, job->ops[i], *err)) { failed = (int)i; break; }
            }
            UpdateStorageCache(w);
        }
    }
    CoUninitialize();
    if (!PostMessageW(job->hwnd, WM_APP_DONE, (WPARAM)(INT_PTR)failed, (LPARAM)err)) delete err;
    return 0;
}

// ------------------------------------------------------------ comandi

void AddOp(Op op, Sel newSel) {
    G.ops.push_back(std::move(op));
    G.sel = newSel;
    RebuildPlan();
    G.note.clear();
    RefreshUi();
    int rowH = S(54);
    int visible = G.rcPanelList.bottom - G.rcPanelList.top;
    G.panelScroll = std::max(0, (int)G.ops.size() * rowH - visible);
}

void CmdCreate() {
    DiskInfo* d = SelDisk();
    Gap g;
    if (!d || !SelGap(g)) return;
    if (d->style == STYLE_MBR && !g.inExt && PrimaryCount(*d) >= 4) {
        Info(L"Limite di partizioni raggiunto",
             L"Un disco MBR può avere al massimo 4 partizioni primarie (o 3 primarie e una estesa). "
             L"Elimina una partizione oppure converti il disco in GPT (solo se vuoto).");
        return;
    }
    CreateDlgData cd;
    cd.info = L"Spazio non allocato: " + FormatSize(g.size) + L" sul " + DiskName(d->number) +
              (g.inExt ? L" (dentro la partizione estesa)" : L"");
    cd.maxBytes = g.size;
    cd.letters = FreeLetters(0);
    if (!ShowCreateDlg(G.hwnd, cd)) return;

    Op op;
    op.type = OpType::Create;
    op.disk = d->number;
    uint64_t size = cd.size;
    if (size >= g.size) { size = g.size; op.useMax = true; }
    op.size = size;
    op.offset = g.offset;
    if (cd.atEnd && !op.useMax) op.offset = std::max(g.offset, AlignDown(g.offset + g.size - size, MiB));
    op.fs = cd.fs;
    op.label = cd.label;
    op.cluster = cd.cluster;
    op.letter = cd.letter;
    op.quick = true;
    op.desc = L"Crea partizione da " + FormatSize(size) + L" su " + DiskName(d->number) + L" (" +
              (op.fs.empty() ? std::wstring(L"non formattata") : op.fs) +
              (op.letter ? std::wstring(L", ") + op.letter + L":" : std::wstring()) + L")";
    AddOp(op, { Sel::Part, d->number, op.offset });
}

void CmdDelete() {
    DiskInfo* d = SelDisk();
    PartInfo* p = SelPart();
    if (!d || !p) return;
    if (p->kind == PartKind::Extended) {
        for (const auto& x : d->parts)
            if (x.kind == PartKind::Logical) {
                Info(L"La partizione estesa non è vuota", L"Elimina prima le unità logiche che contiene.");
                return;
            }
    }
    std::wstring extra;
    if (p->kind == PartKind::Efi || p->kind == PartKind::Recovery || p->kind == PartKind::Msr || p->kind == PartKind::Other)
        extra = L"\n\nAttenzione: è una partizione speciale (" + KindName(p->kind) +
                L"). Eliminarla può impedire l'avvio o il ripristino di un sistema operativo.";
    if (p->isNew) extra.clear();
    std::wstring main = L"Eliminare la partizione " + Quoted(*p) + L"?";
    std::wstring content = L"La partizione da " + FormatSize(p->size) + L" sul " + DiskName(d->number) +
                           L" verrà eliminata quando premi Applica. Tutti i dati che contiene andranno persi." + extra;
    if (!Confirm(main, content, L"Aggiungi alla coda", true)) return;
    Op op;
    op.type = OpType::Delete;
    op.disk = d->number;
    op.offset = p->offset;
    op.desc = L"Elimina " + Quoted(*p) + L" — " + FormatSize(p->size) + L" (" + DiskName(d->number) + L")";
    AddOp(op, { Sel::Part, d->number, p->offset }); // ValidateSel la sposta sullo spazio libero
}

void CmdFormat() {
    DiskInfo* d = SelDisk();
    PartInfo* p = SelPart();
    if (!d || !p) return;
    FormatDlgData fd;
    fd.info = L"Partizione " + Quoted(*p) + L" — " + FormatSize(p->size) + L" (" + DiskName(d->number) + L")";
    fd.size = p->size;
    std::wstring cur = p->fs;
    if (cur != L"NTFS" && cur != L"exFAT" && cur != L"FAT32" && cur != L"ReFS") cur = IsFat32TooBig(p->size) ? L"NTFS" : L"NTFS";
    fd.fs = cur;
    fd.label = p->label;
    if (!ShowFormatDlg(G.hwnd, fd)) return;
    Op op;
    op.type = OpType::Format;
    op.disk = d->number;
    op.offset = p->offset;
    op.fs = fd.fs;
    op.label = fd.label;
    op.cluster = fd.cluster;
    op.quick = fd.quick;
    op.desc = L"Formatta " + PartShortName(*p) + L" in " + fd.fs + (fd.label.empty() ? L"" : L" «" + fd.label + L"»") +
              (fd.quick ? L"" : L" (completa)");
    AddOp(op, G.sel);
}

void CmdResize() {
    DiskInfo* d = SelDisk();
    PartInfo* p = SelPart();
    if (!d || !p) return;
    DiskInfo* ld = FindDisk(G.live, d->number);
    PartInfo* lp = ld ? FindPart(*ld, p->offset) : nullptr;
    if (!lp) return;
    if (!G.uiWmi.Ok()) {
        std::wstring e;
        if (!G.uiWmi.Connect(e)) { Info(L"Impossibile leggere i limiti", e, TD_ERROR_ICON); return; }
    }
    uint64_t mn = 0, mx = 0;
    std::wstring err;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool ok = QuerySupportedSize(G.uiWmi, d->number, p->offset, mn, mx, err);
    SetCursor(old);
    if (!ok) { Info(L"Impossibile leggere i limiti di ridimensionamento", err, TD_ERROR_ICON); return; }

    // fine dello spazio disponibile nel modello pianificato
    uint64_t limit = UsableEnd(*d);
    const PartInfo* ext = p->kind == PartKind::Logical ? FindExtendedFor(*d, p->offset) : nullptr;
    if (ext) limit = std::min(limit, ext->offset + ext->size);
    for (const auto& x : d->parts) {
        if (x.offset <= p->offset) continue;
        if (p->kind == PartKind::Logical) {
            if (x.kind == PartKind::Logical) limit = std::min(limit, x.offset > MiB ? x.offset - MiB : x.offset);
        } else if (x.kind != PartKind::Logical) {
            limit = std::min(limit, x.offset);
        }
    }
    uint64_t planMax = limit > p->offset ? limit - p->offset : p->size;
    bool canGrow = mx > lp->size;
    uint64_t maxB = canGrow ? std::max(planMax, p->size) : p->size;
    uint64_t minB = mn ? std::min(mn, p->size) : p->size;
    if (maxB < p->size) maxB = p->size;
    if (maxB - minB < MiB) {
        Info(L"Partizione non ridimensionabile",
             L"Windows non permette di ridurre né di estendere questa partizione. Per estenderla serve spazio non "
             L"allocato subito dopo e un file system NTFS o ReFS.");
        return;
    }
    ResizeDlgData rd;
    rd.info = L"Partizione " + Quoted(*p) + L" — attuale " + FormatSize(p->size);
    rd.cur = p->size;
    rd.minSize = minB;
    rd.maxSize = maxB;
    if (!ShowResizeDlg(G.hwnd, rd)) return;
    Op op;
    op.type = OpType::Resize;
    op.disk = d->number;
    op.offset = p->offset;
    op.size = rd.result;
    op.desc = (rd.result < p->size ? L"Riduci " : L"Estendi ") + PartShortName(*p) + L" da " + FormatSize(p->size) +
              L" a " + FormatSize(rd.result);
    AddOp(op, G.sel);
}

void CmdLetter() {
    DiskInfo* d = SelDisk();
    PartInfo* p = SelPart();
    if (!d || !p) return;
    LetterDlgData ld;
    ld.info = L"Partizione " + Quoted(*p) + L" — " + FormatSize(p->size);
    ld.cur = p->letter;
    ld.letters = FreeLetters(p->letter);
    if (!ShowLetterDlg(G.hwnd, ld) || ld.result == p->letter) return;
    Op op;
    op.type = OpType::SetLetter;
    op.disk = d->number;
    op.offset = p->offset;
    op.letter = ld.result;
    op.oldLetter = p->letter;
    std::wstring from = p->letter ? std::wstring(1, p->letter) + L":" : PartShortName(*p);
    op.desc = ld.result ? L"Cambia lettera " + from + L" → " + std::wstring(1, ld.result) + L":"
                        : L"Rimuovi la lettera " + from;
    AddOp(op, G.sel);
}

void CmdLabel() {
    DiskInfo* d = SelDisk();
    PartInfo* p = SelPart();
    if (!d || !p) return;
    LabelDlgData ld;
    ld.info = L"Partizione " + PartShortName(*p) + L" (" + p->fs + L") — massimo " + std::to_wstring(LabelLimit(p->fs)) + L" caratteri";
    ld.label = p->label;
    ld.maxLen = LabelLimit(p->fs);
    if (!ShowLabelDlg(G.hwnd, ld) || ld.label == p->label) return;
    Op op;
    op.type = OpType::SetLabel;
    op.disk = d->number;
    op.offset = p->offset;
    op.label = ld.label;
    op.desc = L"Etichetta di " + PartShortName(*p) + L" → " + (ld.label.empty() ? L"(nessuna)" : L"«" + ld.label + L"»");
    AddOp(op, G.sel);
}

void CmdActive() {
    DiskInfo* d = SelDisk();
    PartInfo* p = SelPart();
    if (!d || !p) return;
    if (!Confirm(L"Impostare " + Quoted(*p) + L" come partizione attiva?",
                 L"Su un disco MBR il BIOS avvia la partizione attiva. Se la partizione non contiene un sistema "
                 L"avviabile, il computer potrebbe non partire da questo disco.", L"Aggiungi alla coda", true))
        return;
    Op op;
    op.type = OpType::SetActive;
    op.disk = d->number;
    op.offset = p->offset;
    op.desc = L"Imposta come attiva " + Quoted(*p) + L" (" + DiskName(d->number) + L")";
    AddOp(op, G.sel);
}

void CmdDiskStyle(OpType type, uint16_t style) {
    DiskInfo* d = SelDisk();
    if (!d) return;
    std::wstring sn = StyleName(style);
    if (type == OpType::ConvertDisk &&
        !Confirm(L"Convertire il " + DiskName(d->number) + L" in " + sn + L"?",
                 L"La conversione è possibile solo su un disco senza partizioni.", L"Aggiungi alla coda", false))
        return;
    Op op;
    op.type = type;
    op.disk = d->number;
    op.style = style;
    op.desc = (type == OpType::InitDisk ? L"Inizializza " + DiskName(d->number) + L" come " + sn
                                        : L"Converti " + DiskName(d->number) + L" in " + sn);
    AddOp(op, { Sel::Disk, d->number, 0 });
}

void CmdWipe() {
    DiskInfo* d = SelDisk();
    if (!d) return;
    std::wstring list;
    for (const auto& p : d->parts) {
        if (p.kind == PartKind::Extended) continue;
        list += L"\n•  " + PartTitle(p) + L"  —  " + FormatSize(p.size);
    }
    if (!Confirm(L"Eliminare tutte le partizioni del " + DiskName(d->number) + L"?",
                 d->name + L" (" + FormatSize(d->size) + L")\n\nVerranno eliminate:" + list +
                 L"\n\nTutti i dati del disco andranno persi quando premi Applica.",
                 L"Aggiungi alla coda", true))
        return;
    Op op;
    op.type = OpType::WipeDisk;
    op.disk = d->number;
    op.style = d->style;
    op.desc = L"Elimina tutte le partizioni del " + DiskName(d->number) + L" (" + d->name + L")";
    AddOp(op, { Sel::Disk, d->number, 0 });
}

void CmdExplore() {
    PartInfo* p = SelPart();
    if (!p || !p->letter) return;
    std::wstring path = std::wstring(1, p->letter) + L":\\";
    ShellExecuteW(G.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CmdApply() {
    if (G.ops.empty() || G.busy) return;
    std::wstring list;
    for (size_t i = 0; i < G.ops.size(); ++i) list += std::to_wstring(i + 1) + L".  " + G.ops[i].desc + L"\n";
    bool destructive = false;
    for (const auto& o : G.ops)
        if (o.type == OpType::Delete || o.type == OpType::Format || o.type == OpType::WipeDisk || o.type == OpType::Resize)
            destructive = true;
    std::wstring content = list + L"\nNon spegnere il computer e non scollegare i dischi durante l'esecuzione.";
    if (destructive) content += L" Le operazioni di eliminazione e formattazione non si possono annullare.";
    if (!Confirm(L"Applicare " + std::to_wstring(G.ops.size()) +
                     (G.ops.size() == 1 ? L" operazione?" : L" operazioni?"),
                 content, L"Applica", destructive))
        return;
    G.busy = true;
    G.step = 0;
    G.note.clear();
    UpdateButtons();
    EnableWindow(G.hList, FALSE);
    auto* job = new ApplyJob{ G.hwnd, G.ops };
    HANDLE t = CreateThread(nullptr, 0, ApplyThread, job, 0, nullptr);
    if (!t) {
        delete job;
        G.busy = false;
        EnableWindow(G.hList, TRUE);
        UpdateButtons();
        Info(L"Impossibile avviare le operazioni", L"Creazione del thread non riuscita.", TD_ERROR_ICON);
        return;
    }
    CloseHandle(t);
    InvalidateRect(G.hwnd, nullptr, FALSE);
}

void ShowMenu(POINT screen, bool fromMore);

void DoCommand(int id) {
    if (!Enabled(id)) return;
    switch (id) {
    case IDM_REFRESH:
        if (!G.ops.empty() &&
            !Confirm(L"Aggiornare l'elenco dei dischi?", L"Le operazioni in sospeso verranno annullate.", L"Aggiorna", false))
            return;
        G.ops.clear();
        G.panelScroll = 0;
        StartRefresh();
        break;
    case IDM_CREATE: CmdCreate(); break;
    case IDM_DELETE: CmdDelete(); break;
    case IDM_FORMAT: CmdFormat(); break;
    case IDM_RESIZE: CmdResize(); break;
    case IDM_LETTER: CmdLetter(); break;
    case IDM_LABEL: CmdLabel(); break;
    case IDM_ACTIVE: CmdActive(); break;
    case IDM_EXPLORE: CmdExplore(); break;
    case IDM_INIT_GPT: CmdDiskStyle(OpType::InitDisk, STYLE_GPT); break;
    case IDM_INIT_MBR: CmdDiskStyle(OpType::InitDisk, STYLE_MBR); break;
    case IDM_CONV_GPT: CmdDiskStyle(OpType::ConvertDisk, STYLE_GPT); break;
    case IDM_CONV_MBR: CmdDiskStyle(OpType::ConvertDisk, STYLE_MBR); break;
    case IDM_WIPE: CmdWipe(); break;
    case IDM_APPLY: CmdApply(); break;
    case IDM_UNDO:
        G.ops.pop_back();
        RebuildPlan();
        RefreshUi();
        break;
    case IDM_UNDO_ALL:
        if (!Confirm(L"Annullare tutte le operazioni in sospeso?", L"I dischi non sono stati modificati.", L"Annulla tutte", false))
            return;
        G.ops.clear();
        G.panelScroll = 0;
        RebuildPlan();
        RefreshUi();
        break;
    case IDM_MORE:
        for (const auto& b : G.btns)
            if (b.id == IDM_MORE) {
                POINT pt{ b.rc.left, b.rc.bottom };
                ClientToScreen(G.hwnd, &pt);
                ShowMenu(pt, true);
            }
        break;
    case IDM_OPENLOG: {
        wchar_t dir[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
            std::wstring f = std::wstring(dir) + L"\\DiskPilot\\DiskPilot.log";
            if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) Info(L"Registro vuoto", L"Nessuna operazione eseguita finora.");
            else ShellExecuteW(G.hwnd, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    case IDM_ABOUT:
        Info(L"DiskPilot 0.1",
             L"Gestione partizioni per Windows: crea, elimina, formatta, ridimensiona, cambia lettera ed etichetta, "
             L"inizializza e converti dischi.\n\nLe operazioni passano dalla Storage Management API di Windows, "
             L"la stessa di Gestione disco. Il registro delle operazioni è in %LOCALAPPDATA%\\DiskPilot.");
        break;
    }
}

void AppendItem(HMENU m, int id, const wchar_t* text) {
    AppendMenuW(m, MF_STRING | (Enabled(id) ? 0 : MF_GRAYED), id, text);
}

void ShowMenu(POINT pt, bool fromMore) {
    HMENU m = CreatePopupMenu();
    DiskInfo* d = SelDisk();
    if (fromMore) {
        AppendItem(m, IDM_ACTIVE, L"Imposta come partizione attiva");
        AppendItem(m, IDM_EXPLORE, L"Apri in Esplora file");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendItem(m, IDM_INIT_GPT, L"Inizializza disco come GPT");
        AppendItem(m, IDM_INIT_MBR, L"Inizializza disco come MBR");
        AppendItem(m, IDM_CONV_GPT, L"Converti disco in GPT");
        AppendItem(m, IDM_CONV_MBR, L"Converti disco in MBR");
        AppendItem(m, IDM_WIPE, L"Elimina tutte le partizioni del disco…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendItem(m, IDM_OPENLOG, L"Apri il registro operazioni");
        AppendItem(m, IDM_ABOUT, L"Informazioni su DiskPilot");
    } else if (G.sel.k == Sel::Part) {
        AppendItem(m, IDM_FORMAT, L"Formatta…");
        AppendItem(m, IDM_DELETE, L"Elimina\tCanc");
        AppendItem(m, IDM_RESIZE, L"Ridimensiona…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendItem(m, IDM_LETTER, L"Cambia lettera unità…");
        AppendItem(m, IDM_LABEL, L"Cambia etichetta…");
        if (d && d->style == STYLE_MBR) AppendItem(m, IDM_ACTIVE, L"Imposta come attiva");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendItem(m, IDM_EXPLORE, L"Apri in Esplora file");
    } else if (G.sel.k == Sel::Free) {
        AppendItem(m, IDM_CREATE, L"Crea partizione…");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendItem(m, IDM_WIPE, L"Elimina tutte le partizioni del disco…");
    } else if (G.sel.k == Sel::Disk && d) {
        if (d->style == STYLE_RAW) {
            AppendItem(m, IDM_INIT_GPT, L"Inizializza come GPT");
            AppendItem(m, IDM_INIT_MBR, L"Inizializza come MBR");
        } else {
            AppendItem(m, IDM_CONV_GPT, L"Converti in GPT");
            AppendItem(m, IDM_CONV_MBR, L"Converti in MBR");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendItem(m, IDM_WIPE, L"Elimina tutte le partizioni…");
        }
    } else {
        DestroyMenu(m);
        return;
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, G.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) DoCommand(cmd);
}

// ------------------------------------------------------------ input

int HitButton(POINT pt) {
    for (size_t i = 0; i < G.btns.size(); ++i)
        if (PtInRect(&G.btns[i].rc, pt)) return (int)i;
    return -1;
}

bool HitMap(POINT pt, Sel& out) {
    if (!PtInRect(&G.rcMap, pt)) return false;
    // le fette hanno la precedenza sull'intestazione
    for (auto it = G.hits.rbegin(); it != G.hits.rend(); ++it)
        if (PtInRect(&it->rc, pt)) { out = it->sel; return true; }
    return false;
}

void ScrollMapToSel() {
    if (G.sel.k == Sel::None) return;
    int idx = 0;
    for (const auto& d : G.plan.disks) {
        if (d.number == G.sel.disk) break;
        ++idx;
    }
    int top = idx * (S(kCardH) + S(kCardGap));
    int viewH = G.rcMap.bottom - G.rcMap.top;
    if (top < G.mapScroll) G.mapScroll = top;
    else if (top + S(kCardH) > G.mapScroll + viewH) G.mapScroll = top + S(kCardH) - viewH;
    G.mapScroll = std::clamp(G.mapScroll, 0, std::max(0, G.mapContent - viewH));
}

void SetSel(const Sel& s, bool fromList) {
    G.sel = s;
    if (!fromList) SelectInList();
    else ScrollMapToSel();
    UpdateButtons();
    InvalidateRect(G.hwnd, nullptr, FALSE);
}

void OnDpiChanged(UINT dpi, const RECT* suggested) {
    G.dpi = dpi;
    CreateFonts();
    CreateImageList();
    LoadBrandIcon();
    SendMessageW(G.hList, WM_SETFONT, (WPARAM)G.fNormal, TRUE);
    for (int i = 0; i < (int)(sizeof(kCols) / sizeof(kCols[0])); ++i) ListView_SetColumnWidth(G.hList, i, S(kCols[i].width));
    if (suggested)
        SetWindowPos(G.hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    InvalidateRect(G.hwnd, nullptr, FALSE);
}

LRESULT OnListNotify(NMHDR* nh) {
    switch (nh->code) {
    case LVN_ITEMCHANGED: {
        auto* nm = (NMLISTVIEW*)nh;
        if (G.fillingList) break;
        if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED)) {
            if ((size_t)nm->lParam < G.listRefs.size()) SetSel(G.listRefs[nm->lParam], true);
        }
        break;
    }
    case NM_DBLCLK:
        if (G.sel.k == Sel::Free) DoCommand(IDM_CREATE);
        else if (G.sel.k == Sel::Disk) {
            DiskInfo* d = SelDisk();
            if (d && d->style == STYLE_RAW) DoCommand(IDM_INIT_GPT);
        }
        break;
    case NM_CUSTOMDRAW: {
        auto* cd = (NMLVCUSTOMDRAW*)nh;
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            size_t idx = (size_t)cd->nmcd.lItemlParam;
            if (idx < G.listRefs.size() && G.listRefs[idx].k == Sel::Part) {
                DiskInfo* d = FindDisk(G.plan, G.listRefs[idx].disk);
                PartInfo* p = d ? FindPart(*d, G.listRefs[idx].off) : nullptr;
                if (p && p->pending) cd->clrText = RGB(196, 104, 0);
            } else if (idx < G.listRefs.size() && G.listRefs[idx].k == Sel::Free) {
                cd->clrText = Col::Text2;
            }
            return CDRF_DODEFAULT;
        }
        break;
    }
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        G.hwnd = h;
        G.dpi = GetDpiForWindow(h);
        CreateFonts();
        LoadBrandIcon();
        G.btns = {
            { IDM_REFRESH, L"", L"Aggiorna", 0 },
            { IDM_CREATE, L"", L"Crea", 0 },
            { IDM_DELETE, L"", L"Elimina", 0 },
            { IDM_FORMAT, L"", L"Formatta", 0 },
            { IDM_RESIZE, L"", L"Ridimensiona", 0 },
            { IDM_LETTER, L"", L"Lettera", 0 },
            { IDM_LABEL, L"", L"Etichetta", 0 },
            { IDM_MORE, L"", L"Altro", 0 },
            { IDM_UNDO, L"", L"Annulla ultima", 1 },
            { IDM_UNDO_ALL, L"", L"Annulla tutte", 1 },
            { IDM_APPLY, L"", L"Applica", 2 },
        };
        G.hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                  0, 0, 0, 0, h, (HMENU)(INT_PTR)ID_LIST, g_hInst, nullptr);
        ListView_SetExtendedListViewStyle(G.hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowTheme(G.hList, L"Explorer", nullptr);
        SendMessageW(G.hList, WM_SETFONT, (WPARAM)G.fNormal, FALSE);
        ListView_EnableGroupView(G.hList, TRUE);
        CreateImageList();
        SetupColumns();
        COLORREF cap = Col::Card;
        DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
        Layout();
        UpdateButtons();
        StartRefresh();
        return 0;
    }
    case WM_SIZE:
        Layout();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = { S(1020), S(640) };
        return 0;
    }
    case WM_DPICHANGED:
        OnDpiChanged(HIWORD(wp), (const RECT*)lp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c;
        GetClientRect(h, &c);
        Paint(dc, c);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hot = HitButton(pt);
        if (hot != G.hot) { G.hot = hot; InvalidateRect(h, nullptr, FALSE); }
        if (!G.tracking) {
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            G.tracking = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        G.tracking = false;
        if (G.hot != -1) { G.hot = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int b = HitButton(pt);
        if (b >= 0) {
            if (G.btns[b].enabled) { G.pressed = b; SetCapture(h); InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        Sel s;
        if (!G.busy && HitMap(pt, s)) { SetSel(s, false); SetFocus(G.hList); }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (G.pressed >= 0) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int b = G.pressed;
            G.pressed = -1;
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
            if (HitButton(pt) == b) DoCommand(G.btns[b].id);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        Sel s;
        if (!G.busy && HitMap(pt, s)) {
            SetSel(s, false);
            if (s.k == Sel::Free) DoCommand(IDM_CREATE);
            else if (s.k == Sel::Part) DoCommand(IDM_FORMAT);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        Sel s;
        if (!G.busy && HitMap(pt, s)) {
            SetSel(s, false);
            ClientToScreen(h, &pt);
            ShowMenu(pt, false);
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        if ((HWND)wp == G.hList && !G.busy) {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) {
                int i = ListView_GetNextItem(G.hList, -1, LVNI_SELECTED);
                RECT r{};
                if (i >= 0) ListView_GetItemRect(G.hList, i, &r, LVIR_LABEL);
                pt = { r.left + S(20), r.bottom };
                ClientToScreen(G.hList, &pt);
            }
            ShowMenu(pt, false);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (PtInRect(&G.rcMap, pt)) {
            int viewH = G.rcMap.bottom - G.rcMap.top;
            G.mapScroll = std::clamp(G.mapScroll - delta * S(60) / WHEEL_DELTA, 0, std::max(0, G.mapContent - viewH));
            InvalidateRect(h, &G.rcMap, FALSE);
        } else if (PtInRect(&G.rcPanelList, pt)) {
            int content = (int)G.ops.size() * S(54);
            int viewH = G.rcPanelList.bottom - G.rcPanelList.top;
            G.panelScroll = std::clamp(G.panelScroll - delta * S(54) / WHEEL_DELTA, 0, std::max(0, content - viewH));
            InvalidateRect(h, &G.rcPanel, FALSE);
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)lp;
        if (nh->hwndFrom == G.hList) return OnListNotify(nh);
        break;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == 0 || HIWORD(wp) == 1) DoCommand(LOWORD(wp));
        return 0;
    case WM_DEVICECHANGE:
        if ((wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED) && !G.busy && G.ops.empty())
            SetTimer(h, TIMER_DEVCHANGE, 1200, nullptr);
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_DEVCHANGE) {
            KillTimer(h, TIMER_DEVCHANGE);
            if (!G.busy && G.ops.empty()) StartRefresh();
        }
        return 0;
    case WM_APP_MODEL: {
        std::unique_ptr<std::wstring> err((std::wstring*)wp);
        std::unique_ptr<Model> m((Model*)lp);
        G.refreshing = false;
        if (err->empty()) {
            G.live = std::move(*m);
            G.loaded = true;
            G.loadErr.clear();
        } else {
            G.loadErr = *err;
        }
        if (G.refreshAgain) { G.refreshAgain = false; StartRefresh(); }
        RebuildPlan();
        RefreshUi();
        return 0;
    }
    case WM_APP_STEP:
        G.step = (int)wp;
        if (G.step >= 0) {
            int rowH = S(54), viewH = G.rcPanelList.bottom - G.rcPanelList.top;
            int y = G.step * rowH;
            if (y < G.panelScroll) G.panelScroll = y;
            else if (y + rowH > G.panelScroll + viewH) G.panelScroll = y + rowH - viewH;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_DONE: {
        std::unique_ptr<std::wstring> err((std::wstring*)lp);
        int failed = (int)(INT_PTR)wp;
        std::vector<Op> done = G.ops;
        G.busy = false;
        G.step = -1;
        G.ops.clear();
        G.panelScroll = 0;
        EnableWindow(G.hList, TRUE);
        if (failed >= 0) {
            G.note = L"Operazione non riuscita";
            std::wstring what = failed < (int)done.size() ? done[failed].desc : L"";
            std::wstring content = L"«" + what + L"»\n\n" + *err;
            if (failed + 1 < (int)done.size())
                content += L"\n\nLe " + std::to_wstring(done.size() - failed - 1) +
                           L" operazioni successive non sono state eseguite.";
            if (failed > 0) content += L"\nLe " + std::to_wstring(failed) + L" operazioni precedenti sono state completate.";
            Info(L"Operazione non riuscita", content, TD_ERROR_ICON);
        } else {
            G.note = L"Tutte le operazioni sono state completate";
            MessageBeep(MB_OK);
        }
        StartRefresh();
        UpdateButtons();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        if (G.busy) {
            Info(L"Operazioni in corso", L"Attendi la fine delle operazioni prima di chiudere DiskPilot.", TD_WARNING_ICON);
            return 0;
        }
        if (!G.ops.empty() &&
            !Confirm(L"Uscire senza applicare?", std::to_wstring(G.ops.size()) +
                     L" operazioni in sospeso andranno perse. I dischi non sono stati modificati.", L"Esci", false))
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// In caso di crash: riga nel registro + minidump in %LOCALAPPDATA%\DiskPilot\crash.dmp
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    wchar_t buf[160];
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &mod);
    swprintf(buf, 160, L"CRASH  codice 0x%08lX  indirizzo modulo+0x%llX",
             (unsigned long)ep->ExceptionRecord->ExceptionCode,
             (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)mod));
    LogLine(buf);
    wchar_t dir[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) {
        std::wstring f = std::wstring(dir) + L"\\DiskPilot\\crash.dmp";
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(h);
        }
    }
    MessageBoxW(nullptr, L"DiskPilot si è chiuso per un errore imprevisto.\n\n"
                         L"I dettagli sono nel registro (menu Altro → Apri il registro operazioni) "
                         L"e in %LOCALAPPDATA%\\DiskPilot\\crash.dmp.",
                L"DiskPilot", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    g_hInst = hi;
    SetUnhandledExceptionFilter(CrashHandler);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    if (!IsUserAnAdmin()) {
        MessageBoxW(nullptr, L"DiskPilot deve essere avviato come amministratore.", L"DiskPilot", MB_OK | MB_ICONERROR);
        return 1;
    }

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gtoken = 0;
    Gdiplus::GdiplusStartup(&gtoken, &gsi, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"DiskPilotMain";
    RegisterClassExW(&wc);

    UINT sdpi = GetDpiForSystem();
    int w = MulDiv(1260, (int)sdpi, 96), h = MulDiv(800, (int)sdpi, 96);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    w = std::min(w, (int)(wa.right - wa.left));
    h = std::min(h, (int)(wa.bottom - wa.top));
    int x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DiskPilot", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, nullptr, nullptr, hi, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    ACCEL acc[] = {
        { FVIRTKEY, VK_F5, IDM_REFRESH },
        { FVIRTKEY, VK_DELETE, IDM_DELETE },
        { FVIRTKEY | FCONTROL, 'Z', IDM_UNDO },
        { FVIRTKEY, VK_F2, IDM_LABEL },
    };
    G.accel = CreateAcceleratorTableW(acc, (int)(sizeof(acc) / sizeof(acc[0])));

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(hwnd, G.accel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyAcceleratorTable(G.accel);
    DeleteFonts();
    if (G.il) ImageList_Destroy(G.il);
    if (G.icoBrand) DestroyIcon(G.icoBrand);
    G.uiWmi = Wmi();
    Gdiplus::GdiplusShutdown(gtoken);
    CoUninitialize();
    return (int)m.wParam;
}
