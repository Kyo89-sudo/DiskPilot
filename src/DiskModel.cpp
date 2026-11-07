#include "DiskModel.h"
#include "Wmi.h"
#include <algorithm>
#include <cwctype>
#include <map>

static const wchar_t* G_EFI = L"{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}";
static const wchar_t* G_MSR = L"{e3c9e316-0b5c-4db8-817d-f92df00215ae}";
static const wchar_t* G_REC = L"{de94bba4-06d1-4d40-a16a-bfd50179d6ac}";
static const wchar_t* G_BASIC = L"{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}";
static const wchar_t* G_LDMM = L"{5808c8aa-7e8f-42e0-85d2-e1e90434cfb3}";
static const wchar_t* G_LDMD = L"{af9b60a0-1431-4f62-bc68-3311714a69ad}";
static const wchar_t* G_SPACES = L"{e75caf8f-f680-4cee-afa3-b001e56efc2d}";

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

static bool IsExtendedType(uint16_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static PartKind Classify(uint16_t style, const PartInfo& p) {
    if (style == STYLE_GPT) {
        if (p.gptType == G_EFI) return PartKind::Efi;
        if (p.gptType == G_MSR) return PartKind::Msr;
        if (p.gptType == G_REC) return PartKind::Recovery;
        if (p.gptType == G_LDMM) return PartKind::LdmMeta;
        if (p.gptType == G_LDMD) return PartKind::LdmData;
        if (p.gptType == G_SPACES) return PartKind::Spaces;
        if (p.gptType == G_BASIC) return PartKind::Data;
        return PartKind::Other;
    }
    if (IsExtendedType(p.mbrType)) return PartKind::Extended;
    if (p.mbrType == 0x27) return PartKind::Recovery;
    if (p.mbrType == 0xEF) return PartKind::Efi;
    if (p.mbrType == 0x42) return PartKind::LdmData;
    return PartKind::Data;
}

void SortParts(DiskInfo& d) {
    std::sort(d.parts.begin(), d.parts.end(), [](const PartInfo& a, const PartInfo& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.size > b.size; // contenitore estesa prima
    });
    if (d.style != STYLE_MBR) return;
    for (auto& p : d.parts) {
        if (p.kind == PartKind::Extended) continue;
        bool logical = false;
        for (const auto& e : d.parts)
            if (e.kind == PartKind::Extended && p.offset >= e.offset && p.offset < e.offset + e.size) logical = true;
        if (logical && (p.kind == PartKind::Data || p.kind == PartKind::Logical)) p.kind = PartKind::Logical;
        else if (!logical && p.kind == PartKind::Logical) p.kind = PartKind::Data;
    }
}

bool EnumerateModel(Wmi& w, Model& m, std::wstring& err) {
    m.disks.clear();
    std::vector<WObj> disks, parts, vols;
    if (!w.Query(L"SELECT * FROM MSFT_Disk", disks, err)) return false;
    if (!w.Query(L"SELECT * FROM MSFT_Partition", parts, err)) return false;
    if (!w.Query(L"SELECT * FROM MSFT_Volume", vols, err)) return false;

    struct VolI { std::wstring fs, label; uint64_t size, free; };
    std::map<std::wstring, VolI> vm;
    for (auto& v : vols) {
        std::wstring path = Lower(WStr(v.Get(), L"Path"));
        if (path.empty()) continue;
        vm[path] = { WStr(v.Get(), L"FileSystem"), WStr(v.Get(), L"FileSystemLabel"),
                     WU64(v.Get(), L"Size"), WU64(v.Get(), L"SizeRemaining") };
    }

    for (auto& o : disks) {
        DiskInfo d;
        d.number = WU32(o.Get(), L"Number");
        d.name = Trim(WStr(o.Get(), L"FriendlyName"));
        if (d.name.empty()) d.name = Trim(WStr(o.Get(), L"Model"));
        if (d.name.empty()) d.name = L"Disco sconosciuto";
        d.size = WU64(o.Get(), L"Size");
        d.sector = WU32(o.Get(), L"LogicalSectorSize");
        if (d.sector == 0) d.sector = 512;
        d.style = (uint16_t)WU32(o.Get(), L"PartitionStyle");
        d.bus = (uint16_t)WU32(o.Get(), L"BusType");
        d.isBoot = WBool(o.Get(), L"IsBoot");
        d.isSystem = WBool(o.Get(), L"IsSystem");
        d.offline = WBool(o.Get(), L"IsOffline");
        d.readOnly = WBool(o.Get(), L"IsReadOnly");
        if (d.size == 0) continue; // lettori di schede vuoti
        m.disks.push_back(std::move(d));
    }

    for (auto& o : parts) {
        uint32_t dn = WU32(o.Get(), L"DiskNumber");
        DiskInfo* d = FindDisk(m, dn);
        if (!d) continue;
        PartInfo p;
        p.offset = WU64(o.Get(), L"Offset");
        p.size = WU64(o.Get(), L"Size");
        p.number = WU32(o.Get(), L"PartitionNumber");
        p.letter = (wchar_t)WU32(o.Get(), L"DriveLetter");
        if (p.letter && !iswalpha(p.letter)) p.letter = 0;
        p.letter = (wchar_t)towupper(p.letter);
        p.gptType = Lower(WStr(o.Get(), L"GptType"));
        p.mbrType = (uint16_t)WU32(o.Get(), L"MbrType");
        p.isActive = WBool(o.Get(), L"IsActive");
        p.isBoot = WBool(o.Get(), L"IsBoot");
        p.isSystem = WBool(o.Get(), L"IsSystem");
        p.isHidden = WBool(o.Get(), L"IsHidden");
        p.isReadOnly = WBool(o.Get(), L"IsReadOnly");
        p.kind = Classify(d->style, p);
        for (const auto& ap : WStrArr(o.Get(), L"AccessPaths")) {
            std::wstring l = Lower(ap);
            if (l.rfind(L"\\\\?\\volume", 0) == 0) {
                p.volPath = ap;
                auto it = vm.find(l);
                if (it != vm.end()) {
                    p.hasVolume = true;
                    p.fs = it->second.fs;
                    p.label = it->second.label;
                    p.volSize = it->second.size;
                    p.volFree = it->second.free;
                }
            }
        }
        d->parts.push_back(std::move(p));
    }

    for (auto& d : m.disks) SortParts(d);
    std::sort(m.disks.begin(), m.disks.end(), [](const DiskInfo& a, const DiskInfo& b) { return a.number < b.number; });
    return true;
}

DiskInfo* FindDisk(Model& m, uint32_t number) {
    for (auto& d : m.disks) if (d.number == number) return &d;
    return nullptr;
}

PartInfo* FindPart(DiskInfo& d, uint64_t offset) {
    for (auto& p : d.parts) if (p.offset == offset) return &p;
    return nullptr;
}

const PartInfo* FindExtendedFor(const DiskInfo& d, uint64_t offset) {
    for (const auto& e : d.parts)
        if (e.kind == PartKind::Extended && offset >= e.offset && offset < e.offset + e.size) return &e;
    return nullptr;
}

std::vector<Gap> ComputeGaps(const DiskInfo& d) {
    std::vector<Gap> g;
    if (d.style == STYLE_RAW || d.size < 4 * MiB) return g;
    uint64_t start = MiB;
    uint64_t end = d.size;
    if (d.style == STYLE_GPT) end = d.size > 34ull * d.sector ? d.size - 34ull * d.sector : 0;
    end = AlignDown(end, MiB);

    auto add = [&](uint64_t lo, uint64_t hi, bool ext) {
        lo = AlignUp(lo, MiB);
        hi = AlignDown(hi, MiB);
        if (hi > lo && hi - lo >= MiB) g.push_back({ lo, hi - lo, ext });
    };

    uint64_t cur = start;
    for (const auto& p : d.parts) {
        if (p.kind == PartKind::Logical) continue;
        if (p.offset > cur) add(cur, std::min(p.offset, end), false);
        cur = std::max(cur, p.offset + p.size);
    }
    if (end > cur) add(cur, end, false);

    for (const auto& e : d.parts) {
        if (e.kind != PartKind::Extended) continue;
        uint64_t c = e.offset;
        uint64_t eEnd = e.offset + e.size;
        for (const auto& p : d.parts) {
            if (p.kind != PartKind::Logical || p.offset < e.offset || p.offset >= eEnd) continue;
            // la EBR occupa lo spazio prima di ogni logica: lo consideriamo usato
            uint64_t pStart = p.offset > MiB ? p.offset - MiB : p.offset;
            if (pStart > c) add(c, pStart, true);
            c = std::max(c, p.offset + p.size);
        }
        if (eEnd > c) add(c, eEnd, true);
    }
    std::sort(g.begin(), g.end(), [](const Gap& a, const Gap& b) { return a.offset < b.offset; });
    return g;
}

std::vector<Seg> BuildSegments(const DiskInfo& d) {
    std::vector<Seg> s;
    for (const auto& p : d.parts) {
        if (p.kind == PartKind::Extended) continue; // il contenuto la rappresenta
        Seg x; x.part = &p; s.push_back(x);
    }
    for (const auto& gp : ComputeGaps(d)) {
        Seg x; x.isFree = true; x.gap = gp; s.push_back(x);
    }
    std::sort(s.begin(), s.end(), [](const Seg& a, const Seg& b) { return a.Offset() < b.Offset(); });
    return s;
}

std::wstring FormatSize(uint64_t b) {
    wchar_t buf[64];
    double v = (double)b;
    if (b >= 1024ull * GiB) swprintf(buf, 64, L"%.2f TB", v / (1024.0 * GiB));
    else if (b >= GiB) swprintf(buf, 64, L"%.1f GB", v / GiB);
    else if (b >= MiB) swprintf(buf, 64, L"%.0f MB", v / MiB);
    else if (b >= 1024) swprintf(buf, 64, L"%.0f KB", v / 1024.0);
    else swprintf(buf, 64, L"%llu byte", (unsigned long long)b);
    std::wstring s = buf;
    for (auto& c : s) if (c == L'.') c = L',';
    return s;
}

std::wstring StyleName(uint16_t s) {
    switch (s) {
    case STYLE_MBR: return L"MBR";
    case STYLE_GPT: return L"GPT";
    default: return L"Non inizializzato";
    }
}

std::wstring BusName(uint16_t b) {
    switch (b) {
    case 1: return L"SCSI";
    case 3: return L"ATA";
    case 7: return L"USB";
    case 8: return L"RAID";
    case 9: return L"iSCSI";
    case 10: return L"SAS";
    case 11: return L"SATA";
    case 12: return L"SD";
    case 13: return L"MMC";
    case 15: return L"Disco virtuale";
    case 16: return L"Spazi di archiviazione";
    case 17: return L"NVMe";
    default: return L"";
    }
}

std::wstring KindName(PartKind k) {
    switch (k) {
    case PartKind::Efi: return L"EFI di sistema";
    case PartKind::Msr: return L"Riservata Microsoft";
    case PartKind::Recovery: return L"Ripristino";
    case PartKind::Extended: return L"Partizione estesa";
    case PartKind::LdmMeta: return L"Metadati disco dinamico";
    case PartKind::LdmData: return L"Dati disco dinamico";
    case PartKind::Spaces: return L"Spazi di archiviazione";
    case PartKind::Other: return L"OEM / sconosciuta";
    default: return L"";
    }
}

std::wstring PartShortName(const PartInfo& p) {
    if (p.letter) return std::wstring(1, p.letter) + L":";
    if (p.number) return L"Partizione " + std::to_wstring(p.number);
    return L"Nuova partizione";
}

std::wstring PartTitle(const PartInfo& p) {
    if (p.letter) return std::wstring(1, p.letter) + L":" + (p.label.empty() ? L"" : L" " + p.label);
    if (!p.label.empty()) return p.label;
    std::wstring k = KindName(p.kind);
    if (!k.empty()) return k;
    return PartShortName(p);
}

std::wstring FsText(const PartInfo& p) {
    if (p.kind == PartKind::Extended) return L"—";
    if (!p.hasVolume) {
        if (p.kind == PartKind::Msr) return L"—";
        return L"Altro";
    }
    return p.fs.empty() ? L"RAW" : p.fs;
}

std::wstring TypeText(const DiskInfo& d, const PartInfo& p) {
    if (d.style == STYLE_MBR) {
        if (p.kind == PartKind::Extended) return L"Estesa";
        if (p.kind == PartKind::Logical) return L"Logica";
        if (p.kind == PartKind::Recovery) return L"Primaria (ripristino)";
        return L"Primaria";
    }
    switch (p.kind) {
    case PartKind::Data: return L"Dati di base";
    case PartKind::Efi: return L"EFI";
    case PartKind::Msr: return L"MSR";
    case PartKind::Recovery: return L"Ripristino";
    case PartKind::LdmMeta:
    case PartKind::LdmData: return L"Dinamica";
    case PartKind::Spaces: return L"Spazi di archiviazione";
    default: return L"GPT";
    }
}

std::wstring StateText(const PartInfo& p) {
    std::wstring s;
    auto add = [&](const wchar_t* t) { if (!s.empty()) s += L", "; s += t; };
    if (p.isNew) add(L"Nuova (in sospeso)");
    else if (p.pending) add(L"Modifica in sospeso");
    if (p.isSystem) add(L"Sistema");
    if (p.isBoot) add(L"Avvio");
    if (p.isActive) add(L"Attiva");
    if (p.isHidden) add(L"Nascosta");
    if (p.isReadOnly) add(L"Sola lettura");
    if (s.empty()) s = L"Integra";
    return s;
}

std::wstring DiskTitle(const DiskInfo& d) {
    std::wstring s = L"Disco " + std::to_wstring(d.number) + L"  ·  " + d.name + L"  ·  " + FormatSize(d.size);
    s += L"  ·  " + StyleName(d.style);
    std::wstring b = BusName(d.bus);
    if (!b.empty()) s += L"  ·  " + b;
    return s;
}

int ColorIndex(const PartInfo& p) {
    switch (p.kind) {
    case PartKind::Efi: return 4;
    case PartKind::Recovery: return 5;
    case PartKind::Msr:
    case PartKind::Extended: return 6;
    case PartKind::LdmMeta:
    case PartKind::LdmData:
    case PartKind::Spaces:
    case PartKind::Other: return 8;
    default: break;
    }
    if (!p.hasVolume) return 8;
    std::wstring f = Lower(p.fs);
    if (f == L"ntfs") return 0;
    if (f == L"fat32" || f == L"fat") return 1;
    if (f == L"exfat") return 2;
    if (f == L"refs") return 3;
    return 8;
}

wchar_t SystemDriveLetter() {
    wchar_t buf[MAX_PATH] = {};
    if (GetSystemWindowsDirectoryW(buf, MAX_PATH) && buf[0]) return (wchar_t)towupper(buf[0]);
    return L'C';
}

bool IsProtected(const PartInfo& p) {
    if (p.isNew) return false;
    if (p.isBoot || p.isSystem) return true;
    if (p.letter && p.letter == SystemDriveLetter()) return true;
    return false;
}

bool IsDynamicOrSpaces(const DiskInfo& d) {
    for (const auto& p : d.parts)
        if (p.kind == PartKind::LdmMeta || p.kind == PartKind::LdmData || p.kind == PartKind::Spaces) return true;
    return false;
}

bool IsFat32TooBig(uint64_t size) { return size > 32ull * GiB; }
