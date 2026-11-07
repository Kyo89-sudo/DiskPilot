// DiskPilot - modello di dischi, partizioni e spazio libero
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class Wmi;

constexpr uint64_t MiB = 1024ull * 1024ull;
constexpr uint64_t GiB = MiB * 1024ull;

inline uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
inline uint64_t AlignDown(uint64_t v, uint64_t a) { return v / a * a; }

enum class PartKind { Data, Efi, Msr, Recovery, Extended, Logical, LdmMeta, LdmData, Spaces, Other };

enum : uint16_t { STYLE_RAW = 0, STYLE_MBR = 1, STYLE_GPT = 2 };

struct PartInfo {
    uint64_t offset = 0, size = 0;
    uint32_t number = 0;
    wchar_t letter = 0;
    std::wstring gptType;      // minuscolo, con graffe
    uint16_t mbrType = 0;
    bool isActive = false, isBoot = false, isSystem = false, isHidden = false, isReadOnly = false;
    PartKind kind = PartKind::Data;

    bool hasVolume = false;
    std::wstring volPath;      // percorso GUID del volume (\\?\Volume{...})
    std::wstring fs, label;
    uint64_t volSize = 0, volFree = 0;

    // stato simulato (operazioni in coda)
    bool isNew = false, pending = false;
};

struct DiskInfo {
    uint32_t number = 0;
    std::wstring name;
    uint64_t size = 0;
    uint32_t sector = 512;
    uint16_t style = STYLE_RAW;
    uint16_t bus = 0;
    bool isBoot = false, isSystem = false, offline = false, readOnly = false;
    bool barrier = false;      // c'è un'operazione sull'intero disco in coda
    bool pending = false;
    std::vector<PartInfo> parts;   // ordinate per offset
};

struct Model {
    std::vector<DiskInfo> disks;
};

struct Gap {
    uint64_t offset = 0, size = 0;
    bool inExt = false;
};

// Una fetta della mappa: partizione o spazio libero, in ordine di offset.
struct Seg {
    bool isFree = false;
    const PartInfo* part = nullptr;
    Gap gap;
    uint64_t Offset() const { return isFree ? gap.offset : part->offset; }
    uint64_t Size() const { return isFree ? gap.size : part->size; }
};

bool EnumerateModel(Wmi& w, Model& m, std::wstring& err);

std::vector<Gap> ComputeGaps(const DiskInfo& d);
std::vector<Seg> BuildSegments(const DiskInfo& d);
void SortParts(DiskInfo& d);   // ordina e ricalcola le logiche

DiskInfo* FindDisk(Model& m, uint32_t number);
PartInfo* FindPart(DiskInfo& d, uint64_t offset);
const PartInfo* FindExtendedFor(const DiskInfo& d, uint64_t offset);

// Testi per la UI
std::wstring FormatSize(uint64_t bytes);
std::wstring StyleName(uint16_t style);
std::wstring BusName(uint16_t bus);
std::wstring KindName(PartKind k);
std::wstring PartTitle(const PartInfo& p);      // "E: Dati", "EFI di sistema", ...
std::wstring PartShortName(const PartInfo& p);  // "E:" o "Partizione 3"
std::wstring FsText(const PartInfo& p);
std::wstring TypeText(const DiskInfo& d, const PartInfo& p);
std::wstring StateText(const PartInfo& p);
std::wstring DiskTitle(const DiskInfo& d);
int ColorIndex(const PartInfo& p);  // indice nella tavolozza (vedi Theme)

// Protezioni
wchar_t SystemDriveLetter();
bool IsProtected(const PartInfo& p);     // sistema/avvio: niente elimina/formatta
bool IsDynamicOrSpaces(const DiskInfo& d);
bool IsFat32TooBig(uint64_t size);
