// DiskPilot - wrapper minimale per WMI (namespace ROOT\Microsoft\Windows\Storage)
#pragma once
#include <windows.h>
#include <wbemidl.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

template <class T>
class CPtr {
public:
    CPtr() = default;
    CPtr(const CPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    CPtr(CPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    CPtr& operator=(CPtr o) noexcept { std::swap(p_, o.p_); return *this; }
    ~CPtr() { Reset(); }
    static CPtr Attach(T* p) { CPtr c; c.p_ = p; return c; }
    T* Get() const { return p_; }
    T** Put() { Reset(); return &p_; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T* p_ = nullptr;
};

class Bstr {
public:
    explicit Bstr(const wchar_t* s) : b_(SysAllocString(s)) {}
    explicit Bstr(const std::wstring& s) : b_(SysAllocStringLen(s.data(), (UINT)s.size())) {}
    ~Bstr() { SysFreeString(b_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return b_; }
private:
    BSTR b_;
};

using WObj = CPtr<IWbemClassObject>;

// Parametri di ingresso per un metodo WMI.
class WArgs {
public:
    struct Item { std::wstring name; VARIANT v; };
    WArgs() = default;
    WArgs(const WArgs&) = delete;
    WArgs& operator=(const WArgs&) = delete;
    ~WArgs();
    WArgs& Str(const wchar_t* n, const std::wstring& v);
    WArgs& U64(const wchar_t* n, uint64_t v);   // uint64 in WMI viaggia come stringa
    WArgs& U32(const wchar_t* n, uint32_t v);
    WArgs& U16(const wchar_t* n, uint16_t v);
    WArgs& Bool(const wchar_t* n, bool v);
    WArgs& Char16(const wchar_t* n, wchar_t v);
    const std::vector<Item>& Items() const { return items_; }
private:
    std::vector<Item> items_;
};

class Wmi {
public:
    bool Connect(std::wstring& err);
    bool Ok() const { return (bool)svc_; }
    bool Query(const std::wstring& wql, std::vector<WObj>& out, std::wstring& err);
    // Esegue un metodo; ritorna true se ReturnValue == 0.
    bool Exec(const std::wstring& objPath, const wchar_t* cls, const wchar_t* method,
              const WArgs* args, WObj* outParams, std::wstring& err);
private:
    CPtr<IWbemServices> svc_;
};

std::wstring WStr(IWbemClassObject* o, const wchar_t* name);
uint64_t WU64(IWbemClassObject* o, const wchar_t* name);
inline uint32_t WU32(IWbemClassObject* o, const wchar_t* name) { return (uint32_t)WU64(o, name); }
bool WBool(IWbemClassObject* o, const wchar_t* name);
std::vector<std::wstring> WStrArr(IWbemClassObject* o, const wchar_t* name);
inline std::wstring WPath(IWbemClassObject* o) { return WStr(o, L"__PATH"); }
std::wstring HrText(HRESULT hr);
