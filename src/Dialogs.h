// DiskPilot - finestre di dialogo
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct CreateDlgData {
    std::wstring info;
    uint64_t maxBytes = 0;
    std::vector<wchar_t> letters;   // lettere libere
    // risultato
    uint64_t size = 0;
    bool atEnd = false;
    std::wstring fs = L"NTFS", label;
    uint32_t cluster = 0;
    wchar_t letter = 0;
};

struct FormatDlgData {
    std::wstring info;
    uint64_t size = 0;
    std::wstring fs = L"NTFS", label;
    uint32_t cluster = 0;
    bool quick = true;
};

struct ResizeDlgData {
    std::wstring info;
    uint64_t cur = 0, minSize = 0, maxSize = 0;
    uint64_t result = 0;
};

struct LetterDlgData {
    std::wstring info;
    wchar_t cur = 0;
    std::vector<wchar_t> letters;
    wchar_t result = 0;
};

struct LabelDlgData {
    std::wstring info;
    std::wstring label;
    int maxLen = 32;
};

bool ShowCreateDlg(HWND parent, CreateDlgData& d);
bool ShowFormatDlg(HWND parent, FormatDlgData& d);
bool ShowResizeDlg(HWND parent, ResizeDlgData& d);
bool ShowLetterDlg(HWND parent, LetterDlgData& d);
bool ShowLabelDlg(HWND parent, LabelDlgData& d);

int LabelLimit(const std::wstring& fs);
