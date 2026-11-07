#include "Ops.h"
#include "Wmi.h"
#include <winioctl.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>

static const wchar_t* GPT_BASIC_DATA = L"{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}";

// ---------------------------------------------------------------- log

void LogLine(const std::wstring& s) {
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    std::wstring d = std::wstring(dir) + L"\\DiskPilot";
    CreateDirectoryW(d.c_str(), nullptr);
    std::wstring f = d + L"\\DiskPilot.log";
    HANDLE h = CreateFileW(f.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[64];
    swprintf(ts, 64, L"%04u-%02u-%02u %02u:%02u:%02u  ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = ts + s + L"\r\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string u((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), u.data(), n, nullptr, nullptr);
    DWORD wr = 0;
    WriteFile(h, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------- simulazione

void SimulateOp(Model& m, const Op& op) {
    DiskInfo* d = FindDisk(m, op.disk);
    if (!d) return;
    d->pending = true;
    switch (op.type) {
    case OpType::Create: {
        PartInfo p;
        p.offset = op.offset;
        p.size = op.size;
        p.letter = op.letter;
        p.isNew = p.pending = true;
        p.hasVolume = true;
        p.fs = op.fs;
        p.label = op.label;
        p.volSize = op.size;
        p.volFree = op.fs.empty() ? 0 : op.size;
        if (d->style == STYLE_GPT) { p.kind = PartKind::Data; p.gptType = GPT_BASIC_DATA; }
        else {
            p.kind = FindExtendedFor(*d, op.offset) ? PartKind::Logical : PartKind::Data;
            p.mbrType = (op.fs == L"FAT32") ? 0x0C : 0x07;
        }
        d->parts.push_back(p);
        SortParts(*d);
        break;
    }
    case OpType::Delete: {
        d->parts.erase(std::remove_if(d->parts.begin(), d->parts.end(),
                                      [&](const PartInfo& p) { return p.offset == op.offset; }), d->parts.end());
        SortParts(*d);
        break;
    }
    case OpType::Format:
        if (PartInfo* p = FindPart(*d, op.offset)) {
            p->hasVolume = true;
            p->fs = op.fs;
            p->label = op.label;
            p->volSize = p->size;
            p->volFree = p->size;
            p->pending = true;
        }
        break;
    case OpType::Resize:
        if (PartInfo* p = FindPart(*d, op.offset)) {
            int64_t delta = (int64_t)op.size - (int64_t)p->size;
            p->size = op.size;
            if (p->hasVolume) {
                p->volSize = (uint64_t)std::max<int64_t>(0, (int64_t)p->volSize + delta);
                p->volFree = (uint64_t)std::max<int64_t>(0, (int64_t)p->volFree + delta);
            }
            p->pending = true;
        }
        break;
    case OpType::SetLetter:
        if (PartInfo* p = FindPart(*d, op.offset)) { p->letter = op.letter; p->pending = true; }
        break;
    case OpType::SetLabel:
        if (PartInfo* p = FindPart(*d, op.offset)) { p->label = op.label; p->pending = true; }
        break;
    case OpType::SetActive:
        for (auto& p : d->parts) p.isActive = (p.offset == op.offset);
        if (PartInfo* p = FindPart(*d, op.offset)) p->pending = true;
        break;
    case OpType::InitDisk:
    case OpType::ConvertDisk:
        d->style = op.style;
        d->parts.clear();
        d->barrier = true;
        break;
    case OpType::WipeDisk:
        d->parts.clear();
        d->barrier = true;
        break;
    }
}

// ---------------------------------------------------------------- esecuzione

static bool FindDiskObj(Wmi& w, uint32_t n, WObj& out, std::wstring& err) {
    std::vector<WObj> r;
    if (!w.Query(L"SELECT * FROM MSFT_Disk WHERE Number=" + std::to_wstring(n), r, err)) return false;
    if (r.empty()) { err = L"Disco " + std::to_wstring(n) + L" non trovato"; return false; }
    out = r[0];
    return true;
}

// Cerca la partizione per offset; per le logiche appena create accetta
// uno scostamento fino a 2 MB (spazio della EBR).
static bool FindPartObj(Wmi& w, uint32_t disk, uint64_t off, WObj& out, std::wstring& err) {
    std::vector<WObj> r;
    if (!w.Query(L"SELECT * FROM MSFT_Partition WHERE DiskNumber=" + std::to_wstring(disk), r, err)) return false;
    WObj best;
    uint64_t bestDelta = ~0ull;
    for (auto& o : r) {
        uint64_t po = WU64(o.Get(), L"Offset");
        if (po == off) { out = o; return true; }
        if (po > off && po - off <= 2 * MiB && po - off < bestDelta) { best = o; bestDelta = po - off; }
    }
    if (best) { out = best; return true; }
    err = L"Partizione non trovata sul Disco " + std::to_wstring(disk) + L" (offset " + std::to_wstring(off) + L")";
    return false;
}

static std::wstring VolPathOf(IWbemClassObject* part) {
    for (const auto& ap : WStrArr(part, L"AccessPaths")) {
        std::wstring l = ap;
        for (auto& c : l) c = (wchar_t)towlower(c);
        if (l.rfind(L"\\\\?\\volume", 0) == 0) return ap;
    }
    return L"";
}

static bool VolumeOf(Wmi& w, IWbemClassObject* part, WObj& vol, std::wstring& err, int tries = 12) {
    std::wstring q = L"ASSOCIATORS OF {" + WPath(part) + L"} WHERE AssocClass = MSFT_PartitionToVolume";
    for (int i = 0; i < tries; ++i) {
        std::vector<WObj> r;
        if (w.Query(q, r, err) && !r.empty()) { vol = r[0]; return true; }
        Sleep(500);
    }
    if (err.empty()) err = L"Volume della partizione non trovato";
    return false;
}

// Verifica che nessuno stia usando il volume; se riesce lo smonta.
static bool LockAndDismount(const std::wstring& volPath, std::wstring& err) {
    if (volPath.empty()) return true;
    std::wstring p = volPath;
    if (!p.empty() && p.back() == L'\\') p.pop_back();
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return true; // lasciamo decidere al servizio
    DWORD br = 0;
    bool locked = false;
    for (int i = 0; i < 4 && !locked; ++i) {
        locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr) != FALSE;
        if (!locked) Sleep(500);
    }
    if (!locked) {
        CloseHandle(h);
        err = L"Il volume è in uso da un altro programma (file aperti o finestra di Esplora file). "
              L"Chiudi i programmi che lo usano e riprova.";
        return false;
    }
    DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &br, nullptr);
    CloseHandle(h);
    return true;
}

static bool FormatVolume(Wmi& w, IWbemClassObject* vol, const Op& op, std::wstring& err) {
    WArgs a;
    a.Str(L"FileSystem", op.fs);
    a.Str(L"FileSystemLabel", op.label);
    if (op.cluster) a.U32(L"AllocationUnitSize", op.cluster);
    a.Bool(L"Full", !op.quick);
    a.Bool(L"Force", true);
    return w.Exec(WPath(vol), L"MSFT_Volume", L"Format", &a, nullptr, err);
}

static std::wstring LetterPath(wchar_t c) { return std::wstring(1, c) + L":\\"; }

static bool AddLetter(Wmi& w, IWbemClassObject* part, wchar_t c, std::wstring& err) {
    WArgs a;
    a.Str(L"AccessPath", LetterPath(c));
    return w.Exec(WPath(part), L"MSFT_Partition", L"AddAccessPath", &a, nullptr, err);
}

static bool RemoveLetter(Wmi& w, IWbemClassObject* part, wchar_t c, std::wstring& err) {
    WArgs a;
    a.Str(L"AccessPath", LetterPath(c));
    return w.Exec(WPath(part), L"MSFT_Partition", L"RemoveAccessPath", &a, nullptr, err);
}

static bool ExecCreate(Wmi& w, const Op& op, std::wstring& err) {
    WObj disk;
    if (!FindDiskObj(w, op.disk, disk, err)) return false;
    uint16_t style = (uint16_t)WU32(disk.Get(), L"PartitionStyle");
    WArgs a;
    if (op.useMax) a.Bool(L"UseMaximumSize", true);
    else a.U64(L"Size", op.size);
    a.U64(L"Offset", op.offset);
    a.Bool(L"AssignDriveLetter", false); // la lettera arriva dopo la formattazione (niente popup di Esplora)
    if (style == STYLE_GPT) a.Str(L"GptType", GPT_BASIC_DATA);
    else a.U16(L"MbrType", op.fs == L"FAT32" ? 0x0C : 0x07);
    if (!w.Exec(WPath(disk.Get()), L"MSFT_Disk", L"CreatePartition", &a, nullptr, err)) return false;

    WObj part;
    if (!FindPartObj(w, op.disk, op.offset, part, err)) return false;
    if (!op.fs.empty()) {
        WObj vol;
        if (!VolumeOf(w, part.Get(), vol, err)) return false;
        if (!FormatVolume(w, vol.Get(), op, err)) { err = L"Partizione creata, ma formattazione non riuscita: " + err; return false; }
    }
    if (op.letter) {
        // rilegge la partizione: dopo la formattazione l'oggetto può essere cambiato
        if (!FindPartObj(w, op.disk, op.offset, part, err)) return false;
        if (!AddLetter(w, part.Get(), op.letter, err)) { err = L"Partizione creata, ma lettera non assegnata: " + err; return false; }
    }
    return true;
}

static bool ExecWipe(Wmi& w, const Op& op, std::wstring& err) {
    std::vector<WObj> parts;
    if (!w.Query(L"SELECT * FROM MSFT_Partition WHERE DiskNumber=" + std::to_wstring(op.disk), parts, err)) return false;
    for (auto& p : parts)
        if (!LockAndDismount(VolPathOf(p.Get()), err)) return false;
    WObj disk;
    if (!FindDiskObj(w, op.disk, disk, err)) return false;
    WArgs a;
    a.Bool(L"RemoveData", true).Bool(L"RemoveOEM", true).Bool(L"ZeroOutEntireDisk", false);
    if (!w.Exec(WPath(disk.Get()), L"MSFT_Disk", L"Clear", &a, nullptr, err)) return false;
    // Clear lascia il disco non inizializzato: lo reinizializziamo nello stile di prima
    for (int i = 0; i < 6; ++i) {
        if (!FindDiskObj(w, op.disk, disk, err)) return false;
        if (WU32(disk.Get(), L"PartitionStyle") != STYLE_RAW) return true;
        WArgs b;
        b.U16(L"PartitionStyle", op.style);
        std::wstring e2;
        if (w.Exec(WPath(disk.Get()), L"MSFT_Disk", L"Initialize", &b, nullptr, e2)) return true;
        err = e2;
        Sleep(500);
    }
    err = L"Partizioni eliminate, ma il disco non è stato reinizializzato: " + err;
    return false;
}

bool ExecuteOp(Wmi& w, const Op& op, std::wstring& err) {
    LogLine(L"ESEGUI  " + op.desc);
    bool ok = false;
    WObj part, disk, vol;
    switch (op.type) {
    case OpType::Create:
        ok = ExecCreate(w, op, err);
        break;
    case OpType::Delete:
        ok = FindPartObj(w, op.disk, op.offset, part, err) &&
             LockAndDismount(VolPathOf(part.Get()), err) &&
             w.Exec(WPath(part.Get()), L"MSFT_Partition", L"DeleteObject", nullptr, nullptr, err);
        break;
    case OpType::Format:
        ok = FindPartObj(w, op.disk, op.offset, part, err) &&
             VolumeOf(w, part.Get(), vol, err, 4) &&
             FormatVolume(w, vol.Get(), op, err);
        break;
    case OpType::Resize: {
        WArgs a;
        a.U64(L"Size", op.size);
        ok = FindPartObj(w, op.disk, op.offset, part, err) &&
             w.Exec(WPath(part.Get()), L"MSFT_Partition", L"Resize", &a, nullptr, err);
        break;
    }
    case OpType::SetLetter:
        ok = FindPartObj(w, op.disk, op.offset, part, err);
        if (ok && op.oldLetter) ok = RemoveLetter(w, part.Get(), op.oldLetter, err);
        if (ok && op.letter) {
            ok = AddLetter(w, part.Get(), op.letter, err);
            if (!ok && op.oldLetter) { std::wstring e2; AddLetter(w, part.Get(), op.oldLetter, e2); }
        }
        break;
    case OpType::SetLabel:
        ok = FindPartObj(w, op.disk, op.offset, part, err);
        if (ok) {
            std::wstring vp = VolPathOf(part.Get());
            if (vp.empty()) { err = L"La partizione non ha un volume"; ok = false; }
            else if (!SetVolumeLabelW(vp.c_str(), op.label.c_str())) { err = HrText(HRESULT_FROM_WIN32(GetLastError())); ok = false; }
        }
        break;
    case OpType::SetActive: {
        WArgs a;
        a.Bool(L"IsActive", true);
        ok = FindPartObj(w, op.disk, op.offset, part, err) &&
             w.Exec(WPath(part.Get()), L"MSFT_Partition", L"SetAttributes", &a, nullptr, err);
        break;
    }
    case OpType::InitDisk: {
        WArgs a;
        a.U16(L"PartitionStyle", op.style);
        ok = FindDiskObj(w, op.disk, disk, err) &&
             w.Exec(WPath(disk.Get()), L"MSFT_Disk", L"Initialize", &a, nullptr, err);
        break;
    }
    case OpType::ConvertDisk: {
        WArgs a;
        a.U16(L"PartitionStyle", op.style);
        ok = FindDiskObj(w, op.disk, disk, err) &&
             w.Exec(WPath(disk.Get()), L"MSFT_Disk", L"ConvertStyle", &a, nullptr, err);
        break;
    }
    case OpType::WipeDisk:
        ok = ExecWipe(w, op, err);
        break;
    }
    LogLine(ok ? L"   OK" : L"   ERRORE: " + err);
    return ok;
}

bool QuerySupportedSize(Wmi& w, uint32_t disk, uint64_t offset, uint64_t& mn, uint64_t& mx, std::wstring& err) {
    std::vector<WObj> r;
    if (!w.Query(L"SELECT * FROM MSFT_Partition WHERE DiskNumber=" + std::to_wstring(disk), r, err)) return false;
    for (auto& o : r) {
        if (WU64(o.Get(), L"Offset") != offset) continue;
        WObj out;
        if (!w.Exec(WPath(o.Get()), L"MSFT_Partition", L"GetSupportedSize", nullptr, &out, err)) return false;
        mn = WU64(out.Get(), L"SizeMin");
        mx = WU64(out.Get(), L"SizeMax");
        return true;
    }
    err = L"Partizione non trovata";
    return false;
}

void UpdateStorageCache(Wmi& w) {
    std::wstring err;
    w.Exec(L"MSFT_StorageSetting", L"MSFT_StorageSetting", L"UpdateHostStorageCache", nullptr, nullptr, err);
}
