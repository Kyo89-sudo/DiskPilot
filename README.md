<p align="center">
  <img src="assets/icon-256.png" width="160" alt="Icona di DiskPilot">
</p>

<h1 align="center">DiskPilot</h1>

<p align="center">
  Gestione partizioni per Windows in stile EaseUS Partition Master
</p>

<p align="center">
  <img alt="Windows 10/11" src="https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C">
  <img alt="Win32" src="https://img.shields.io/badge/GUI-Win32%20nativa-34C759">
  <img alt="Nessuna dipendenza" src="https://img.shields.io/badge/dipendenze-nessuna-AF52DE">
</p>

---

GUI nativa Win32 (C++20, GDI+),
motore basato sulla **Windows Storage Management API** (WMI, `ROOT\Microsoft\Windows\Storage`),
la stessa usata da Gestione disco e da PowerShell (`Get-Partition`, `New-Partition`, …).

## Funzioni (v0.1)

- Mappa grafica dei dischi con le partizioni colorate per file system e la barra di occupazione
- Elenco dettagliato raggruppato per disco (file system, capacità, usato, libero, tipo, stato)
- **Coda delle operazioni**: le modifiche vengono simulate sulla mappa e i dischi cambiano solo con **Applica**
- Crea partizione (dimensione, inizio/fine dello spazio libero, file system, cluster, etichetta, lettera)
- Elimina partizione (con controllo che il volume non sia in uso)
- Formatta in NTFS, exFAT, FAT32 (fino a 32 GB), ReFS — rapida o completa
- Ridimensiona (riduci/estendi) con i limiti reali calcolati da Windows
- Cambia lettera di unità ed etichetta
- Imposta partizione attiva (MBR)
- Inizializza disco come GPT/MBR, converti un disco vuoto, elimina tutte le partizioni di un disco
- Aggiornamento automatico quando colleghi/scolleghi un disco
- Registro delle operazioni in `%LOCALAPPDATA%\DiskPilot\DiskPilot.log`

### Protezioni

- La partizione di Windows (C:), quella di avvio e la EFI di sistema non si possono eliminare né formattare
- Sul disco di sistema non si può inizializzare, convertire o svuotare
- Dischi dinamici, Spazi di archiviazione, dischi offline o in sola lettura: solo visualizzazione
- Prima di eliminare un volume viene verificato che nessun programma lo stia usando

## Compilazione

Requisiti: Visual Studio 2026 con il carico di lavoro *Sviluppo di applicazioni desktop con C++*
(CMake e Ninja sono già inclusi). Nessuna dipendenza esterna, niente vcpkg.

```bat
build.bat          :: Release  -> build\Release\DiskPilot.exe
build.bat debug    :: Debug
```

In alternativa: *File → Apri → Cartella* in Visual Studio e compilare il target `DiskPilot`.

Il programma chiede i privilegi di amministratore all'avvio (manifest `requireAdministrator`).

## Prove in sicurezza

Per provare le operazioni senza rischi usa un disco virtuale:

1. Gestione disco → *Azione → Crea VHD* (es. 2 GB, `C:\test.vhdx`)
2. Il VHD compare in DiskPilot come disco non inizializzato
3. Al termine: *Azione → Scollega VHD*

## Struttura

| File | Contenuto |
|---|---|
| `src/Wmi.*` | wrapper WMI: connessione, query, chiamata di metodi |
| `src/DiskModel.*` | lettura di dischi/partizioni/volumi, calcolo dello spazio libero, testi |
| `src/Ops.*` | operazioni in coda: simulazione sul modello ed esecuzione reale |
| `src/Dialogs.*` | finestre Crea, Formatta, Ridimensiona, Lettera, Etichetta |
| `src/main.cpp` | finestra principale, mappa dischi, lista, pannello coda, toolbar |
| `res/` | icona, manifest, dialoghi |
| `assets/` | icona 1024 px e lo script che la genera |

## Prossimi passi

- FAT32 oltre 32 GB (formattatore proprio)
- Spostamento partizioni (estensione verso sinistra)
- Clonazione disco/partizione
- Conversione MBR↔GPT senza perdita di dati
