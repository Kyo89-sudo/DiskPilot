#include "Wmi.h"
#include <cstdio>
#include <cwchar>

#ifdef _MSC_VER
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

static VARIANT MakeBstr(const std::wstring& s) {
    VARIANT v; VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocStringLen(s.data(), (UINT)s.size());
    return v;
}

WArgs::~WArgs() { for (auto& i : items_) VariantClear(&i.v); }

WArgs& WArgs::Str(const wchar_t* n, const std::wstring& s) { items_.push_back({ n, MakeBstr(s) }); return *this; }
WArgs& WArgs::U64(const wchar_t* n, uint64_t x) { items_.push_back({ n, MakeBstr(std::to_wstring(x)) }); return *this; }
WArgs& WArgs::U32(const wchar_t* n, uint32_t x) {
    VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = (LONG)x; items_.push_back({ n, v }); return *this;
}
WArgs& WArgs::U16(const wchar_t* n, uint16_t x) {
    VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = (LONG)x; items_.push_back({ n, v }); return *this;
}
WArgs& WArgs::Bool(const wchar_t* n, bool b) {
    VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; items_.push_back({ n, v }); return *this;
}
WArgs& WArgs::Char16(const wchar_t* n, wchar_t c) {
    VARIANT v; VariantInit(&v); v.vt = VT_I2; v.iVal = (SHORT)c; items_.push_back({ n, v }); return *this;
}

std::wstring HrText(HRESULT hr) {
    wchar_t buf[64];
    swprintf(buf, 64, L"0x%08lX", (unsigned long)hr);
    std::wstring s = buf;
    wchar_t* msg = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, (DWORD)hr, 0, (LPWSTR)&msg, 0, nullptr) && msg) {
        std::wstring m = msg;
        LocalFree(msg);
        while (!m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L' ')) m.pop_back();
        s = m + L" (" + s + L")";
    }
    return s;
}

bool Wmi::Connect(std::wstring& err) {
    CPtr<IWbemLocator> loc;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)loc.Put());
    if (FAILED(hr)) { err = L"Impossibile avviare WMI: " + HrText(hr); return false; }
    hr = loc->ConnectServer(Bstr(L"ROOT\\Microsoft\\Windows\\Storage"), nullptr, nullptr, nullptr, 0, nullptr, nullptr, svc_.Put());
    if (FAILED(hr)) { err = L"Impossibile collegarsi al servizio di archiviazione: " + HrText(hr); return false; }
    CoSetProxyBlanket(svc_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

bool Wmi::Query(const std::wstring& wql, std::vector<WObj>& out, std::wstring& err) {
    out.clear();
    if (!svc_) { err = L"WMI non collegato"; return false; }
    CPtr<IEnumWbemClassObject> en;
    HRESULT hr = svc_->ExecQuery(Bstr(L"WQL"), Bstr(wql), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, en.Put());
    if (FAILED(hr)) { err = L"Query WMI non riuscita: " + HrText(hr); return false; }
    for (;;) {
        IWbemClassObject* o = nullptr;
        ULONG n = 0;
        hr = en->Next(WBEM_INFINITE, 1, &o, &n);
        if (FAILED(hr)) { err = L"Lettura WMI non riuscita: " + HrText(hr); return false; }
        if (n == 0 || !o) break;
        out.push_back(WObj::Attach(o));
    }
    return true;
}

static std::wstring StatusText(uint32_t rv, IWbemClassObject* out) {
    std::wstring ext;
    if (out) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(out->Get(L"ExtendedStatus", 0, &v, nullptr, nullptr))) {
            if (v.vt == VT_UNKNOWN && v.punkVal) {
                WObj es;
                if (SUCCEEDED(v.punkVal->QueryInterface(IID_IWbemClassObject, (void**)es.Put())) && es)
                    ext = WStr(es.Get(), L"Message");
            }
        }
        VariantClear(&v);
    }
    const wchar_t* base;
    switch (rv) {
    case 1: base = L"Operazione non supportata"; break;
    case 2: base = L"Errore non specificato"; break;
    case 3: base = L"Tempo scaduto"; break;
    case 4: base = L"Operazione non riuscita"; break;
    case 5: base = L"Parametro non valido"; break;
    case 4097: base = L"Dimensione non supportata"; break;
    case 40001: base = L"Accesso negato"; break;
    case 40002: base = L"Risorse insufficienti per completare l'operazione"; break;
    default: base = L"Operazione non riuscita"; break;
    }
    std::wstring s = ext.empty() ? std::wstring(base) : ext;
    s += L" (codice " + std::to_wstring(rv) + L")";
    return s;
}

bool Wmi::Exec(const std::wstring& objPath, const wchar_t* cls, const wchar_t* method,
               const WArgs* args, WObj* outParams, std::wstring& err) {
    if (!svc_) { err = L"WMI non collegato"; return false; }
    WObj clsObj;
    HRESULT hr = svc_->GetObject(Bstr(cls), 0, nullptr, clsObj.Put(), nullptr);
    if (FAILED(hr)) { err = std::wstring(L"Classe ") + cls + L" non disponibile: " + HrText(hr); return false; }
    WObj inSig;
    hr = clsObj->GetMethod(method, 0, inSig.Put(), nullptr);
    if (FAILED(hr)) { err = std::wstring(L"Metodo ") + method + L" non disponibile: " + HrText(hr); return false; }
    WObj in;
    if (inSig) {
        hr = inSig->SpawnInstance(0, in.Put());
        if (FAILED(hr)) { err = L"SpawnInstance: " + HrText(hr); return false; }
        if (args) {
            for (const auto& it : args->Items()) {
                VARIANT v = it.v; // copia superficiale, Put non la modifica
                hr = in->Put(it.name.c_str(), 0, &v, 0);
                if (FAILED(hr)) { err = L"Parametro " + it.name + L": " + HrText(hr); return false; }
            }
        }
    }
    WObj out;
    hr = svc_->ExecMethod(Bstr(objPath), Bstr(method), 0, nullptr, in.Get(), out.Put(), nullptr);
    if (FAILED(hr)) { err = std::wstring(method) + L": " + HrText(hr); return false; }
    uint32_t rv = out ? WU32(out.Get(), L"ReturnValue") : 0;
    if (outParams) *outParams = out;
    if (rv != 0) { err = StatusText(rv, out.Get()); return false; }
    return true;
}

static bool GetV(IWbemClassObject* o, const wchar_t* n, VARIANT& v) {
    VariantInit(&v);
    return o && SUCCEEDED(o->Get(n, 0, &v, nullptr, nullptr));
}

std::wstring WStr(IWbemClassObject* o, const wchar_t* n) {
    VARIANT v; std::wstring r;
    if (GetV(o, n, v) && v.vt == VT_BSTR && v.bstrVal) r = v.bstrVal;
    VariantClear(&v);
    return r;
}

uint64_t WU64(IWbemClassObject* o, const wchar_t* n) {
    VARIANT v; uint64_t r = 0;
    if (GetV(o, n, v)) {
        switch (v.vt) {
        case VT_BSTR: r = v.bstrVal ? wcstoull(v.bstrVal, nullptr, 10) : 0; break;
        case VT_UI8: r = v.ullVal; break;
        case VT_I8: r = (uint64_t)v.llVal; break;
        case VT_I4: r = (uint32_t)v.lVal; break;
        case VT_UI4: r = v.ulVal; break;
        case VT_I2: r = (uint16_t)v.iVal; break;
        case VT_UI2: r = v.uiVal; break;
        case VT_UI1: r = v.bVal; break;
        case VT_BOOL: r = v.boolVal != VARIANT_FALSE; break;
        default: break;
        }
    }
    VariantClear(&v);
    return r;
}

bool WBool(IWbemClassObject* o, const wchar_t* n) {
    VARIANT v; bool r = false;
    if (GetV(o, n, v) && v.vt == VT_BOOL) r = v.boolVal != VARIANT_FALSE;
    VariantClear(&v);
    return r;
}

std::vector<std::wstring> WStrArr(IWbemClassObject* o, const wchar_t* n) {
    std::vector<std::wstring> r;
    VARIANT v;
    if (GetV(o, n, v) && v.vt == (VT_ARRAY | VT_BSTR) && v.parray) {
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(v.parray, 1, &lo);
        SafeArrayGetUBound(v.parray, 1, &hi);
        for (LONG i = lo; i <= hi; ++i) {
            BSTR b = nullptr;
            if (SUCCEEDED(SafeArrayGetElement(v.parray, &i, &b)) && b) {
                r.emplace_back(b);
                SysFreeString(b);
            }
        }
    }
    VariantClear(&v);
    return r;
}
