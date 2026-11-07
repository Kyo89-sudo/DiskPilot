// DiskPilot - operazioni in coda: simulazione sul modello ed esecuzione reale
#pragma once
#include "DiskModel.h"
#include <string>

class Wmi;

enum class OpType { Create, Delete, Format, Resize, SetLetter, SetLabel, SetActive, InitDisk, ConvertDisk, WipeDisk };

struct Op {
    OpType type = OpType::Create;
    uint32_t disk = 0;
    uint64_t offset = 0;      // partizione bersaglio (o nuova)
    uint64_t size = 0;        // nuova dimensione / dimensione da creare
    bool useMax = false;      // Create: occupa tutto lo spazio libero dal punto scelto
    wchar_t letter = 0;       // Create/SetLetter: 0 = nessuna
    wchar_t oldLetter = 0;
    std::wstring fs, label;   // fs vuoto in Create = non formattare
    uint32_t cluster = 0;     // 0 = predefinita
    bool quick = true;
    uint16_t style = 0;       // InitDisk / ConvertDisk / WipeDisk (stile da ricreare)
    std::wstring desc;        // testo per la coda
};

// Applica l'operazione al modello pianificato (nessuna modifica ai dischi).
void SimulateOp(Model& m, const Op& op);

// Esegue davvero l'operazione. Ritorna false con messaggio in err.
bool ExecuteOp(Wmi& w, const Op& op, std::wstring& err);

// Limiti di ridimensionamento reali per una partizione esistente.
bool QuerySupportedSize(Wmi& w, uint32_t disk, uint64_t offset, uint64_t& minSize, uint64_t& maxSize, std::wstring& err);

// Aggiorna la cache del servizio di archiviazione (dopo le modifiche).
void UpdateStorageCache(Wmi& w);

void LogLine(const std::wstring& s);
