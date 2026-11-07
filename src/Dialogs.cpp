#include "Dialogs.h"
#include "DiskModel.h"
#include "../res/resource.h"
#include <commctrl.h>
#include <algorithm>
#include <climits>
#include <cwctype>

extern HINSTANCE g_hInst;

namespace {

struct ClusterOpt { uint32_t bytes; const wchar_t* text; };
const ClusterOpt kClusters[] = {
    { 0, L"Predefinita" }, { 512, L"512 byte" }, { 1024, L"1 KB" }, { 2048, L"2 KB" },
    { 4096, L"4 KB" }, { 8192, L"8 KB" }, { 16384, L"16 KB" }, { 32768, L"32 KB" }, { 65536, L"64 KB" },
};

const wchar_t* kFs[] = { L"NTFS", L"exFAT", L"FAT32", L"ReFS" };

bool g_sync = false; // evita rimbalzi edit <-> slider

void CenterOnParent(HWND dlg) {
    HWND par = GetParent(dlg);
    RECT rp, rd;
    if (!par || !GetWindowRect(par, &rp)) return;
    GetWindowRect(dlg, &rd);
    int w = rd.right - rd.left, h = rd.bottom - rd.top;
    int x = rp.left + ((rp.right - rp.left) - w) / 2;
    int y = rp.top + ((rp.bottom - rp.top) - h) / 2;
    SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void Warn(HWND dlg, const wchar_t* text) {
    MessageBoxW(dlg, text, L"DiskPilot", MB_OK | MB_ICONWARNING);
}

int AddItem(HWND cb, const wchar_t* text, LPARAM data) {
    int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageW(cb, CB_SETITEMDATA, i, data);
    return i;
}

LPARAM SelData(HWND cb) {
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : SendMessageW(cb, CB_GETITEMDATA, i, 0);
}

std::wstring SelText(HWND cb) {
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i < 0) return L"";
    wchar_t buf[64] = {};
    SendMessageW(cb, CB_GETLBTEXT, i, (LPARAM)buf);
    return buf;
}

void FillFs(HWND cb, const std::wstring& cur, bool allowNone) {
    int sel = 0;
    for (int i = 0; i < 4; ++i) {
        int k = AddItem(cb, kFs[i], 0);
        if (_wcsicmp(cur.c_str(), kFs[i]) == 0) sel = k;
    }
    if (allowNone) {
        int k = AddItem(cb, L"Non formattare", 1);
        if (cur.empty()) sel = k;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

std::wstring GetFs(HWND cb) {
    if (SelData(cb) == 1) return L"";
    return SelText(cb);
}

void FillClusters(HWND cb, uint32_t cur) {
    int sel = 0;
    for (const auto& c : kClusters) {
        int k = AddItem(cb, c.text, c.bytes);
        if (c.bytes == cur) sel = k;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

void FillLetters(HWND cb, const std::vector<wchar_t>& letters, wchar_t cur, bool noneFirst) {
    int sel = -1;
    if (noneFirst) { int k = AddItem(cb, L"Nessuna", 0); if (!cur) sel = k; }
    for (wchar_t c : letters) {
        wchar_t t[4] = { c, L':', 0, 0 };
        int k = AddItem(cb, t, c);
        if (c == cur) sel = k;
    }
    if (!noneFirst) { int k = AddItem(cb, L"Nessuna", 0); if (!cur) sel = k; }
    SendMessageW(cb, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
}

std::wstring GetText(HWND dlg, int id) {
    HWND h = GetDlgItem(dlg, id);
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n, L'\0');
    if (n) GetWindowTextW(h, s.data(), n + 1);
    return s;
}

uint64_t GetMB(HWND dlg) {
    std::wstring s = GetText(dlg, IDC_SIZEMB);
    uint64_t v = 0;
    for (wchar_t c : s) if (iswdigit(c)) v = v * 10 + (uint64_t)(c - L'0');
    return v;
}

void SetMB(HWND dlg, uint64_t mb) {
    SetDlgItemTextW(dlg, IDC_SIZEMB, std::to_wstring(mb).c_str());
}

// Controlli dimensione condivisi da Crea e Ridimensiona
struct SizeRange { uint64_t minMB, maxMB; };

void InitSizeCtl(HWND dlg, SizeRange r, uint64_t curMB) {
    HWND spin = GetDlgItem(dlg, IDC_SIZE_SPIN);
    SendMessageW(spin, UDM_SETRANGE32, (WPARAM)(int)std::min<uint64_t>(r.minMB, INT_MAX),
                 (LPARAM)(int)std::min<uint64_t>(r.maxMB, INT_MAX));
    HWND tr = GetDlgItem(dlg, IDC_SIZE_TRACK);
    SendMessageW(tr, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(tr, TBM_SETRANGEMAX, TRUE, 1000);
    g_sync = true;
    SetMB(dlg, curMB);
    g_sync = false;
}

void SyncTrackFromEdit(HWND dlg, SizeRange r) {
    if (g_sync) return;
    uint64_t mb = std::clamp(GetMB(dlg), r.minMB, r.maxMB);
    int pos = r.maxMB > r.minMB ? (int)((mb - r.minMB) * 1000 / (r.maxMB - r.minMB)) : 1000;
    SendDlgItemMessageW(dlg, IDC_SIZE_TRACK, TBM_SETPOS, TRUE, pos);
}

void SyncEditFromTrack(HWND dlg, SizeRange r) {
    int pos = (int)SendDlgItemMessageW(dlg, IDC_SIZE_TRACK, TBM_GETPOS, 0, 0);
    uint64_t mb = r.minMB + (r.maxMB - r.minMB) * (uint64_t)pos / 1000;
    g_sync = true;
    SetMB(dlg, mb);
    g_sync = false;
}

bool CheckLabel(HWND dlg, const std::wstring& fs, const std::wstring& label) {
    int lim = LabelLimit(fs);
    if ((int)label.size() > lim) {
        std::wstring m = L"L'etichetta è troppo lunga: con " + (fs.empty() ? std::wstring(L"questo file system") : fs) +
                         L" il massimo è " + std::to_wstring(lim) + L" caratteri.";
        Warn(dlg, m.c_str());
        return false;
    }
    return true;
}

// ------------------------------------------------------------ Crea

INT_PTR CALLBACK CreateProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (CreateDlgData*)GetWindowLongPtrW(dlg, DWLP_USER);
    if (!d && msg != WM_INITDIALOG) return FALSE; // notifiche dei controlli prima di WM_INITDIALOG
    switch (msg) {
    case WM_INITDIALOG: {
        d = (CreateDlgData*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_INFO, d->info.c_str());
        SizeRange r{ 1, std::max<uint64_t>(1, d->maxBytes / MiB) };
        InitSizeCtl(dlg, r, r.maxMB);
        SyncTrackFromEdit(dlg, r);
        SetDlgItemTextW(dlg, IDC_SIZE_RANGE, (L"max " + std::to_wstring(r.maxMB) + L" MB").c_str());
        CheckRadioButton(dlg, IDC_POS_START, IDC_POS_END, IDC_POS_START);
        FillFs(GetDlgItem(dlg, IDC_FS), L"NTFS", true);
        FillClusters(GetDlgItem(dlg, IDC_CLUSTER), 0);
        SendDlgItemMessageW(dlg, IDC_LABEL, EM_LIMITTEXT, 32, 0);
        SetDlgItemTextW(dlg, IDC_LABEL, L"Nuovo volume");
        FillLetters(GetDlgItem(dlg, IDC_LETTER), d->letters, d->letters.empty() ? 0 : d->letters[0], false);
        CenterOnParent(dlg);
        return TRUE;
    }
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(dlg, IDC_SIZE_TRACK))
            SyncEditFromTrack(dlg, { 1, std::max<uint64_t>(1, d->maxBytes / MiB) });
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SIZEMB:
            if (HIWORD(wp) == EN_CHANGE) SyncTrackFromEdit(dlg, { 1, std::max<uint64_t>(1, d->maxBytes / MiB) });
            return TRUE;
        case IDC_FS:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                bool none = GetFs(GetDlgItem(dlg, IDC_FS)).empty();
                EnableWindow(GetDlgItem(dlg, IDC_CLUSTER), !none);
                EnableWindow(GetDlgItem(dlg, IDC_LABEL), !none);
            }
            return TRUE;
        case IDOK: {
            uint64_t maxMB = std::max<uint64_t>(1, d->maxBytes / MiB);
            uint64_t mb = GetMB(dlg);
            if (mb < 1 || mb > maxMB) {
                Warn(dlg, (L"Indica una dimensione tra 1 e " + std::to_wstring(maxMB) + L" MB.").c_str());
                return TRUE;
            }
            uint64_t bytes = mb >= maxMB ? d->maxBytes : mb * MiB;
            std::wstring fs = GetFs(GetDlgItem(dlg, IDC_FS));
            std::wstring label = fs.empty() ? L"" : GetText(dlg, IDC_LABEL);
            if (fs == L"FAT32" && IsFat32TooBig(bytes)) {
                Warn(dlg, L"Windows può formattare in FAT32 solo volumi fino a 32 GB. Scegli una dimensione minore oppure exFAT/NTFS.");
                return TRUE;
            }
            if (!CheckLabel(dlg, fs, label)) return TRUE;
            d->size = bytes;
            d->atEnd = IsDlgButtonChecked(dlg, IDC_POS_END) == BST_CHECKED;
            d->fs = fs;
            d->label = label;
            d->cluster = fs.empty() ? 0 : (uint32_t)SelData(GetDlgItem(dlg, IDC_CLUSTER));
            d->letter = (wchar_t)SelData(GetDlgItem(dlg, IDC_LETTER));
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ------------------------------------------------------------ Formatta

INT_PTR CALLBACK FormatProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (FormatDlgData*)GetWindowLongPtrW(dlg, DWLP_USER);
    if (!d && msg != WM_INITDIALOG) return FALSE; // notifiche dei controlli prima di WM_INITDIALOG
    switch (msg) {
    case WM_INITDIALOG:
        d = (FormatDlgData*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_INFO, d->info.c_str());
        FillFs(GetDlgItem(dlg, IDC_FS), d->fs, false);
        FillClusters(GetDlgItem(dlg, IDC_CLUSTER), d->cluster);
        SendDlgItemMessageW(dlg, IDC_LABEL, EM_LIMITTEXT, 32, 0);
        SetDlgItemTextW(dlg, IDC_LABEL, d->label.c_str());
        CheckDlgButton(dlg, IDC_QUICK, d->quick ? BST_CHECKED : BST_UNCHECKED);
        CenterOnParent(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            std::wstring fs = GetFs(GetDlgItem(dlg, IDC_FS));
            std::wstring label = GetText(dlg, IDC_LABEL);
            if (fs == L"FAT32" && IsFat32TooBig(d->size)) {
                Warn(dlg, L"Windows può formattare in FAT32 solo volumi fino a 32 GB. Scegli exFAT o NTFS.");
                return TRUE;
            }
            if (!CheckLabel(dlg, fs, label)) return TRUE;
            d->fs = fs;
            d->label = label;
            d->cluster = (uint32_t)SelData(GetDlgItem(dlg, IDC_CLUSTER));
            d->quick = IsDlgButtonChecked(dlg, IDC_QUICK) == BST_CHECKED;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ------------------------------------------------------------ Ridimensiona

void UpdateAfter(HWND dlg, const ResizeDlgData* d) {
    uint64_t mb = GetMB(dlg);
    uint64_t bytes = mb * MiB;
    std::wstring t;
    if (bytes > d->maxSize) t = L"Oltre il massimo consentito";
    else t = L"Spazio non allocato dopo la partizione: " + FormatSize(d->maxSize - std::min(bytes, d->maxSize));
    SetDlgItemTextW(dlg, IDC_AFTER, t.c_str());
}

SizeRange ResizeRange(const ResizeDlgData* d) {
    return { (d->minSize + MiB - 1) / MiB, d->maxSize / MiB };
}

INT_PTR CALLBACK ResizeProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (ResizeDlgData*)GetWindowLongPtrW(dlg, DWLP_USER);
    if (!d && msg != WM_INITDIALOG) return FALSE; // notifiche dei controlli prima di WM_INITDIALOG
    switch (msg) {
    case WM_INITDIALOG: {
        d = (ResizeDlgData*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_INFO, d->info.c_str());
        SizeRange r = ResizeRange(d);
        InitSizeCtl(dlg, r, d->cur / MiB);
        SyncTrackFromEdit(dlg, r);
        std::wstring rng = L"Minima " + FormatSize(d->minSize) + L"  ·  Massima " + FormatSize(d->maxSize);
        SetDlgItemTextW(dlg, IDC_SIZE_RANGE, rng.c_str());
        UpdateAfter(dlg, d);
        CenterOnParent(dlg);
        return TRUE;
    }
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(dlg, IDC_SIZE_TRACK)) { SyncEditFromTrack(dlg, ResizeRange(d)); UpdateAfter(dlg, d); }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SIZEMB:
            if (HIWORD(wp) == EN_CHANGE && d) { SyncTrackFromEdit(dlg, ResizeRange(d)); UpdateAfter(dlg, d); }
            return TRUE;
        case IDOK: {
            SizeRange r = ResizeRange(d);
            uint64_t mb = GetMB(dlg);
            if (mb < r.minMB || mb > r.maxMB) {
                Warn(dlg, (L"Indica una dimensione tra " + std::to_wstring(r.minMB) + L" e " + std::to_wstring(r.maxMB) + L" MB.").c_str());
                return TRUE;
            }
            uint64_t bytes = mb * MiB;
            if (mb == r.maxMB) bytes = d->maxSize;
            if (mb == d->cur / MiB) bytes = d->cur;
            d->result = bytes;
            EndDialog(dlg, bytes == d->cur ? IDCANCEL : IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ------------------------------------------------------------ Lettera

INT_PTR CALLBACK LetterProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (LetterDlgData*)GetWindowLongPtrW(dlg, DWLP_USER);
    if (!d && msg != WM_INITDIALOG) return FALSE; // notifiche dei controlli prima di WM_INITDIALOG
    switch (msg) {
    case WM_INITDIALOG:
        d = (LetterDlgData*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_INFO, d->info.c_str());
        FillLetters(GetDlgItem(dlg, IDC_LETTER), d->letters, d->cur, true);
        CenterOnParent(dlg);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            d->result = (wchar_t)SelData(GetDlgItem(dlg, IDC_LETTER));
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ------------------------------------------------------------ Etichetta

INT_PTR CALLBACK LabelProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (LabelDlgData*)GetWindowLongPtrW(dlg, DWLP_USER);
    if (!d && msg != WM_INITDIALOG) return FALSE; // notifiche dei controlli prima di WM_INITDIALOG
    switch (msg) {
    case WM_INITDIALOG:
        d = (LabelDlgData*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_INFO, d->info.c_str());
        SendDlgItemMessageW(dlg, IDC_LABEL, EM_LIMITTEXT, d->maxLen, 0);
        SetDlgItemTextW(dlg, IDC_LABEL, d->label.c_str());
        SendDlgItemMessageW(dlg, IDC_LABEL, EM_SETSEL, 0, -1);
        CenterOnParent(dlg);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            d->label = GetText(dlg, IDC_LABEL);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

} // namespace

int LabelLimit(const std::wstring& fs) {
    if (_wcsicmp(fs.c_str(), L"FAT32") == 0 || _wcsicmp(fs.c_str(), L"FAT") == 0) return 11;
    if (_wcsicmp(fs.c_str(), L"exFAT") == 0) return 15;
    return 32;
}

bool ShowCreateDlg(HWND p, CreateDlgData& d) { return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_CREATE), p, CreateProc, (LPARAM)&d) == IDOK; }
bool ShowFormatDlg(HWND p, FormatDlgData& d) { return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_FORMAT), p, FormatProc, (LPARAM)&d) == IDOK; }
bool ShowResizeDlg(HWND p, ResizeDlgData& d) { return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_RESIZE), p, ResizeProc, (LPARAM)&d) == IDOK; }
bool ShowLetterDlg(HWND p, LetterDlgData& d) { return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_LETTER), p, LetterProc, (LPARAM)&d) == IDOK; }
bool ShowLabelDlg(HWND p, LabelDlgData& d) { return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_LABEL), p, LabelProc, (LPARAM)&d) == IDOK; }
