// ============================================================================
// VoiceLink SAPI Bridge — DLL Entry Point + COM Exports + Registry
// ============================================================================
//
// This file contains:
//   1. DllMain          — Called when the DLL is loaded/unloaded
//   2. DllGetClassObject — COM asks us for a class factory
//   3. DllCanUnloadNow   — COM asks if we can be removed from memory
//   4. DllRegisterServer — regsvr32 calls this to register us
//   5. DllUnregisterServer — regsvr32 /u calls this to unregister us
//
// Plus the registry code that makes our voices appear in Windows.
//
// THE 4 EXPORTS:
//   Every COM in-process server (DLL) must export exactly these 4 functions.
//   They're defined in voicelink.def so the linker exports them with the
//   correct names (no C++ name mangling).
//
// REGISTRATION:
//   When you run "regsvr32 voicelink_sapi.dll", Windows:
//   1. Loads our DLL (DllMain fires)
//   2. Calls DllRegisterServer
//   3. We write registry keys that tell Windows about our COM class and voices
//   4. Unloads the DLL
//
//   After this, our voices appear in any app's voice list.
// ============================================================================

// INITGUID must be defined BEFORE including guids.h in exactly ONE .cpp file.
// This makes DEFINE_GUID create actual GUID storage instead of extern declarations.
#include <initguid.h>

#include "guids.h"
#include "class_factory.h"
#include "debug.h"

#include <windows.h>
#include <sapi.h>
#include <string>
#include <vector>

// ============================================================================
// Global State
// ============================================================================

// Handle to our DLL module (needed to find our DLL path for registry)
HMODULE g_hModule = nullptr;

// Reference counting for DLL unloading.
// COM won't call FreeLibrary on us while either of these is > 0.
LONG g_lockCount = 0;   // Incremented by IClassFactory::LockServer(TRUE)
LONG g_objectCount = 0; // Incremented when a VoiceLinkEngine is created

// ============================================================================
// Voice Definitions
// ============================================================================
//
// Each entry here becomes a SAPI voice token in the registry.
// All voices share the same CLSID (same engine), but the engine reads
// the VoiceLinkVoiceId / VoiceLinkModel attributes to know to know which voice
// and backend to request from the inference server.
//
// MODEL SELECTION (install-time):
//   The installer writes HKLM\SOFTWARE\VoiceLink\SelectedModel = "kokoro"
//   or "piper". DllRegisterServer only registers voices for that model.
//   If the key is missing, both sets are registered.
//
// The Language field uses LCID (Locale ID) in hex:
//   409 = en-US, 809 = en-GB, 80a = es-MX, c0a = es-ES
//
// SAPI uses these attributes for voice selection:
//   - Name: display name in voice picker
//   - Gender: Male or Female
//   - Language: filters by locale
//   - Vendor: groups voices by provider

struct VoiceDefinition
{
    const wchar_t *tokenName;   // Registry key name (e.g., "VoiceLink_af_heart")
    const wchar_t *displayName; // Friendly name (e.g., "VoiceLink Kokoro Heart")
    const wchar_t *voiceId;     // ID sent to inference server (e.g., "af_heart")
    const wchar_t *model;       // Backend: "kokoro" or "piper"
    const wchar_t *gender;      // "Female" or "Male"
    const wchar_t *language;    // LCID in hex (e.g., "409" for en-US)
    const wchar_t *age;         // "Adult", "Child", etc.
};
                  
// All 14 Kokoro voices, matching server/models/kokoro_model.py
static const VoiceDefinition g_kokoroVoices[] = {
    // American English Female voices
    {L"VoiceLink_af_heart", L"Heart (Kokoro)", L"af_heart", L"Female", L"409", L"Adult"},
    {L"VoiceLink_af_bella", L"Bella (Kokoro)", L"af_bella", L"Female", L"409", L"Adult"},
    {L"VoiceLink_af_nicole", L"Nicole (Kokoro)", L"af_nicole", L"Female", L"409", L"Adult"},
    {L"VoiceLink_af_sarah", L"Sarah (Kokoro)", L"af_sarah", L"Female", L"409", L"Adult"},
    {L"VoiceLink_af_sky", L"Sky (Kokoro)", L"af_sky", L"Female", L"409", L"Adult"},

    // American English Male voices
    {L"VoiceLink_am_adam", L"Adam (Kokoro)", L"am_adam", L"Male", L"409", L"Adult"},
    {L"VoiceLink_am_michael", L"Michael (Kokoro)", L"am_michael", L"Male", L"409", L"Adult"},

    // British English Female voices
    {L"VoiceLink_bf_emma", L"Emma (Kokoro)", L"bf_emma", L"Female", L"809", L"Adult"},
    {L"VoiceLink_bf_isabella", L"Isabella (Kokoro)", L"bf_isabella", L"Female", L"809", L"Adult"},

    // British English Male voices
    {L"VoiceLink_bm_george", L"George (Kokoro)", L"bm_george", L"Male", L"809", L"Adult"},
    {L"VoiceLink_bm_lewis", L"Lewis (Kokoro)", L"bm_lewis", L"Male", L"809", L"Adult"},

    // Spanish Female voices
    {L"VoiceLink_ef_dora", L"Dora (Kokoro)", L"ef_dora", L"Female", L"80a", L"Adult"},

    // Spanish Male voices
    {L"VoiceLink_em_alex", L"Alex (Kokoro)", L"em_alex", L"Male", L"80a", L"Adult"},
    {L"VoiceLink_em_santa", L"Santa (Kokoro)", L"em_santa", L"Male", L"80a", L"Adult"},
};

// Piper voices — matching server/models/piper_model.py
// Token names use underscores (registry-safe); voiceId keeps Piper's hyphen form.
static const VoiceDefinition g_piperVoices[] = {
    // American English voices
    {L"VoiceLink_en_US_ryan_high", L"Ryan (Piper)", L"en_US-ryan-high", L"piper", L"Male", L"409", L"Adult"},
    {L"VoiceLink_en_US_hfc_male_medium", L"HFC Male (Piper)", L"en_US-hfc_male-medium", L"piper", L"Male", L"409", L"Adult"},
    {L"VoiceLink_en_US_hfc_female_medium", L"HFC Female (Piper)", L"en_US-hfc_female-medium", L"piper", L"Female", L"409", L"Adult"},
    {L"VoiceLink_en_US_lessac_high", L"Lessac (Piper)", L"en_US-lessac-high", L"piper", L"Female", L"409", L"Adult"},

    // Spanish (Spain)
    {L"VoiceLink_es_ES_davefx_medium", L"Davefx (Piper)", L"es_ES-davefx-medium", L"piper", L"Male", L"c0a", L"Adult"},
    {L"VoiceLink_es_ES_sharvard_medium", L"Sharvard (Piper)", L"es_ES-sharvard-medium", L"piper", L"Male", L"c0a", L"Adult"},
    // Spanish (Mexico)
    {L"VoiceLink_es_MX_claude_high", L"Claude (Piper)", L"es_MX-claude-high", L"piper", L"Female", L"80a", L"Adult"},
};

static constexpr size_t g_kokoroVoiceCount = _countof(g_kokoroVoices);
static constexpr size_t g_piperVoiceCount = _countof(g_piperVoices);

// Read the install-time model selection from the registry.
// Returns "kokoro", "piper", or empty string (register both).
static std::wstring ReadSelectedModel()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VoiceLink", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return {};
    
    wchar_t buf[64] = {};
    DWORD bufSize = sizeof(buf);
    DWORD type = 0;
    LONG result = RegQueryValueExW(hKey, L"SelectedModel", nullptr, &type, reinterpret_cast<BYTE *>(buf), &bufSize);
    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS || type != REG_SZ) 92 return {};
        return buf;
}

// ============================================================================
// DllMain — DLL Entry Point
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        // Save our module handle — we need it later to find our DLL path
        g_hModule = hModule;

        // Tell Windows we don't need DLL_THREAD_ATTACH/DETACH notifications.
        // This is a micro-optimization: Windows won't call DllMain for every
        // new thread, which saves a tiny bit of overhead in multi-threaded apps.
        DisableThreadLibraryCalls(hModule);

        VLOG(L"DLL loaded (DLL_PROCESS_ATTACH)");
        break;

    case DLL_PROCESS_DETACH:
        VLOG(L"DLL unloading (DLL_PROCESS_DETACH)");
        break;
    }
    return TRUE;
}

// ============================================================================
// DllGetClassObject — COM asks us for a class factory
// ============================================================================
//
// When an app calls CoCreateInstance(CLSID_VoiceLinkEngine, ...), COM:
//   1. Looks up CLSID in registry → finds our DLL path
//   2. Loads our DLL (DllMain fires)
//   3. Calls DllGetClassObject(CLSID_VoiceLinkEngine, IID_IClassFactory, ...)
//   4. We create a VoiceLinkClassFactory and return it
//   5. COM calls factory->CreateInstance() to get the actual engine
//
// The rclsid parameter is the CLSID of the class being requested.
// We only support CLSID_VoiceLinkEngine.

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    VLOG(L"DllGetClassObject called");

    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    // Only our engine class is supported
    if (rclsid != CLSID_VoiceLinkEngine)
    {
        VERR(L"Unknown CLSID requested");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    // Create a class factory
    auto *factory = new (std::nothrow) VoiceLinkClassFactory();
    if (!factory)
    {
        return E_OUTOFMEMORY;
    }

    // Ask the factory for the requested interface (usually IID_IClassFactory)
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release(); // Balance the ref from new (constructor sets it to 1)

    return hr;
}

// ============================================================================
// DllCanUnloadNow — COM asks if we can be removed from memory
// ============================================================================
//
// COM periodically calls this to see if it can free our DLL.
// We return S_OK (yes, unload us) only when:
//   - No VoiceLinkEngine objects exist (g_objectCount == 0)
//   - No server locks are held (g_lockCount == 0)
//
// If we return S_FALSE, COM keeps us loaded.

STDAPI DllCanUnloadNow()
{
    BOOL canUnload = (g_lockCount == 0 && g_objectCount == 0);
    VLOG(L"DllCanUnloadNow: locks=%ld, objects=%ld → %s",
         g_lockCount, g_objectCount, canUnload ? L"YES" : L"NO");
    return canUnload ? S_OK : S_FALSE;
}

// ============================================================================
// Registry Helpers
// ============================================================================

// Get the full path to our DLL
static std::wstring GetDllPath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    return path;
}

// Convert a GUID to string form: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
static std::wstring GuidToString(const GUID &guid)
{
    wchar_t buf[64] = {};
    StringFromGUID2(guid, buf, _countof(buf));
    return buf;
}

// Create a registry key and optionally set its default value.
// If preserveExisting is true and the key already exists, the default
// value is NOT overwritten (preserves user renames).
static HRESULT CreateKeyWithDefault(HKEY hParent, const wchar_t *subkey,
                                    const wchar_t *defaultValue, HKEY *phkResult = nullptr,
                                    bool preserveExisting = false)
{
    HKEY hKey = nullptr;
    DWORD disposition = 0;
    LONG result = RegCreateKeyExW(
        hParent, subkey, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_READ, nullptr,
        &hKey, &disposition);

    if (result != ERROR_SUCCESS)
    {
        VERR(L"RegCreateKeyEx failed for %s: %ld", subkey, result);
        return HRESULT_FROM_WIN32(result);
    }

    if (defaultValue)
    {
        bool shouldWrite = true;

        // If preserving existing names, only write if key is newly created
        // OR if the current value is empty
        if (preserveExisting && disposition == REG_OPENED_EXISTING_KEY)
        {
            // Check if there's already a non-empty value
            DWORD dataSize = 0;
            LONG queryResult = RegQueryValueExW(hKey, nullptr, nullptr, nullptr, nullptr, &dataSize);
            if (queryResult == ERROR_SUCCESS && dataSize > sizeof(wchar_t))
            {
                shouldWrite = false; // Already has a name, preserve it
            }
        }

        if (shouldWrite)
        {
            result = RegSetValueExW(
                hKey, nullptr, 0, REG_SZ,
                reinterpret_cast<const BYTE *>(defaultValue),
                static_cast<DWORD>((wcslen(defaultValue) + 1) * sizeof(wchar_t)));
        }
    }

    if (phkResult)
    {
        *phkResult = hKey;
    }
    else
    {
        RegCloseKey(hKey);
    }

    return (result == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(result);
}

// Set a named string value on an open registry key
static HRESULT SetStringValue(HKEY hKey, const wchar_t *name, const wchar_t *value)
{
    LONG result = RegSetValueExW(
        hKey, name, 0, REG_SZ,
        reinterpret_cast<const BYTE *>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    return (result == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(result);
}

// Recursively delete a registry key and all its subkeys
static HRESULT DeleteKeyRecursive(HKEY hParent, const wchar_t *subkey)
{
    LONG result = RegDeleteTreeW(hParent, subkey);
    if (result == ERROR_FILE_NOT_FOUND)
        return S_OK; // Already gone, that's fine
    return (result == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(result);
}

// ============================================================================
// DllRegisterServer — Register the COM class and SAPI voice tokens
// ============================================================================
//
// When you run "regsvr32 voicelink_sapi.dll", this function creates:
//
// 1. COM Class Registration:
//    HKCR\CLSID\{D7A5E2B1-...}\
//      (Default) = "VoiceLink TTS Engine"
//      \InprocServer32\
//        (Default) = "C:\...\voicelink_sapi.dll"
//        ThreadingModel = "Both"
//
// 2. SAPI Voice Tokens (one per voice):
//    HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens\VoiceLink_af_heart\
//      (Default) = "VoiceLink Kokoro Heart"
//      CLSID = "{D7A5E2B1-...}"
//      VoiceLinkVoiceId = "af_heart"
//      \Attributes\
//        Name = "VoiceLink Kokoro Heart"
//        Gender = "Female"
//        Language = "409"
//        Age = "Adult"
//        Vendor = "VoiceLink"
//
// After registration, every SAPI app will discover our voices.

STDAPI DllRegisterServer()
{
    VLOG(L"DllRegisterServer called");

    std::wstring dllPath = GetDllPath();
    std::wstring clsidStr = GuidToString(CLSID_VoiceLinkEngine);

    VLOG(L"DLL path: %s", dllPath.c_str());
    VLOG(L"CLSID: %s", clsidStr.c_str());

    // -----------------------------------------------------------------------
    // Step 1: Register the COM class in HKEY_CLASSES_ROOT\CLSID
    //
    // This tells COM: "CLSID {D7A5E2B1-...} maps to voicelink_sapi.dll"
    //
    // ThreadingModel = "Both" means our DLL works correctly in both
    // single-threaded apartment (STA) and multi-threaded apartment (MTA).
    // SAPI typically uses STA, but some apps use MTA.
    // -----------------------------------------------------------------------
    {
        std::wstring clsidKeyPath = std::wstring(L"CLSID\\") + clsidStr;

        HKEY hClsid = nullptr;
        HRESULT hr = CreateKeyWithDefault(HKEY_CLASSES_ROOT, clsidKeyPath.c_str(),
                                          L"VoiceLink TTS Engine", &hClsid);
        if (FAILED(hr))
            return hr;

        // InprocServer32 tells COM this is an in-process (DLL) server
        std::wstring inprocPath = clsidKeyPath + L"\\InprocServer32";
        HKEY hInproc = nullptr;
        hr = CreateKeyWithDefault(HKEY_CLASSES_ROOT, inprocPath.c_str(),
                                  dllPath.c_str(), &hInproc);
        if (FAILED(hr))
        {
            RegCloseKey(hClsid);
            return hr;
        }

        SetStringValue(hInproc, L"ThreadingModel", L"Both");
        RegCloseKey(hInproc);
        RegCloseKey(hClsid);

        VLOG(L"COM class registered");
    }

    // -----------------------------------------------------------------------
    // Step 2: Register each voice as a SAPI voice token
    //
    // Windows has TWO speech registries:
    //
    //   1. Classic SAPI 5:
    //      HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens\
    //      Used by: PowerShell, Balabolka, older .NET apps, classic SAPI apps
    //
    //   2. OneCore (Modern Speech API):
    //      HKLM\SOFTWARE\Microsoft\Speech_OneCore\Voices\Tokens\
    //      Used by: Thorium Reader, Chromium/Electron apps, Edge Read Aloud,
    //               Windows Narrator, UWP apps, modern .NET
    //
    // We register in BOTH so every app can discover VoiceLink voices.
    // The NaturalVoiceSAPIAdapter project does the same thing.
    //
    // Install-time model selection (HKLM\SOFTWARE\VoiceLink\SelectedModel):
    //   "kokoro" → only Kokoro voices
    //   "piper"  → only Piper voices
    //   missing  → both (handy during development)
    // -----------------------------------------------------------------------
    std::wstring selectedModel = ReadSelectedModel();
    const bool regKokoro = selectedModel.empty() || _wcsicmp(selectedModel.c_str(), L"kokoro") == 0;
    const bool regPiper = selectedModel.empty() || _wcsicmp(selectedModel.c_str(), L"piper") == 0;

    if (!selectedModel.empty())
        VLOG(L"SelectedModel from registry: %s", selectedModel.c_str());
    else
        VLOG(L"No SelectedModel key — registering both Kokoro and Piper voices");

    // Build the active list of voices to register
    std::vector<const VoiceDefinition *> activeVoices;
    if (regKokoro)
    {
        for (size_t i = 0; i < g_kokoroVoiceCount; ++i)
            activeVoices.push_back(&g_kokoroVoices[i]);
    }
    if (regPiper)
    {
        for (size_t i = 0; i < g_piperVoiceCount; ++i)
            activeVoices.push_back(&g_piperVoices[i]);
    }

    const wchar_t *tokenRoots[] = {
        L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
        L"SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens",
    };

    for (const wchar_t *tokensRoot : tokenRoots)
    {
        VLOG(L"Registering voices under: %s", tokensRoot);

        for (const VoiceDefinition *pVoice : activeVoices)
        {
            const VoiceDefinition &voice = *pVoice;

            std::wstring tokenPath = std::wstring(tokensRoot) + L"\\" + voice.tokenName;

            // Create the voice token key
            // preserveExisting=true: don't overwrite user-renamed display names
            HKEY hToken = nullptr;
            HRESULT hr = CreateKeyWithDefault(HKEY_LOCAL_MACHINE, tokenPath.c_str(),
                                              voice.displayName, &hToken,
                                              true /* preserveExisting */);
            if (FAILED(hr))
            {
                VERR(L"Failed to create token for %s", voice.tokenName);
                continue; // Try the next voice
            }

            // Set token values
            SetStringValue(hToken, L"CLSID", clsidStr.c_str());
            SetStringValue(hToken, L"VoiceLinkVoiceId", voice.voiceId);
            SetStringValue(hToken, L"VoiceLinkServerPort", L"7860");

            // Create the Attributes subkey
            HKEY hAttrs = nullptr;
            std::wstring attrsPath = tokenPath + L"\\Attributes";
            hr = CreateKeyWithDefault(HKEY_LOCAL_MACHINE, attrsPath.c_str(),
                                      nullptr, &hAttrs);
            if (SUCCEEDED(hr) && hAttrs)
            {
                // Only set Name if not already present (preserve renames)
                DWORD nameSize = 0;
                LONG qr = RegQueryValueExW(hAttrs, L"Name", nullptr, nullptr, nullptr, &nameSize);
                if (qr != ERROR_SUCCESS || nameSize <= sizeof(wchar_t))
                {
                    SetStringValue(hAttrs, L"Name", voice.displayName);
                }
                SetStringValue(hAttrs, L"Gender", voice.gender);
                SetStringValue(hAttrs, L"Language", voice.language);
                SetStringValue(hAttrs, L"Age", voice.age);
                SetStringValue(hAttrs, L"Vendor", L"VoiceLink");
                RegCloseKey(hAttrs);
            }

            RegCloseKey(hToken);
            VLOG(L"Registered voice: %s (%s)", voice.displayName, voice.voiceId);
        }
    }

    VLOG(L"DllRegisterServer completed: %zu voices x 2 registries", activeVoices.size());
    return S_OK;
}

// ============================================================================
// DllUnregisterServer — Remove all registry entries
// ============================================================================
//
// Called by "regsvr32 /u voicelink_sapi.dll".
// Removes everything DllRegisterServer created.

STDAPI DllUnregisterServer()
{
    VLOG(L"DllUnregisterServer called");

    std::wstring clsidStr = GuidToString(CLSID_VoiceLinkEngine);

    // -----------------------------------------------------------------------
    // Step 1: Remove the COM class registration
    // -----------------------------------------------------------------------
    {
        std::wstring clsidKeyPath = std::wstring(L"CLSID\\") + clsidStr;
        DeleteKeyRecursive(HKEY_CLASSES_ROOT, clsidKeyPath.c_str());
        VLOG(L"COM class unregistered");
    }

    // -----------------------------------------------------------------------
    // Step 2: Remove all voice tokens from BOTH registries
    // Always remove both Kokoro and Piper tokens so a model switch + re-register
    // does not leave stale tokens behind.
    // -----------------------------------------------------------------------
    const wchar_t *tokenRoots[] = {
        L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
        L"SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens",
    };

    auto unregisterAll = [&](const VoiceDefinition *voices, size_t count, const wchar_t *tokensRoot)
    {
        for (size_t i = 0; i < count; ++i)
        {
            std::wstring tokenPath = std::wstring(tokensRoot) + L"\\" + voices[i].tokenName;
            DeleteKeyRecursive(HKEY_LOCAL_MACHINE, tokenPath.c_str());
            VLOG(L"Unregistered voice: %s from %s", voices[i].tokenName, tokensRoot);
        }
    };

    for (const wchar_t *tokensRoot : tokenRoots)
    {
        unregisterAll(g_kokoroVoices, g_kokoroVoiceCount, tokensRoot);
        unregisterAll(g_piperVoices, g_piperVoiceCount, tokensRoot);
    }

    VLOG(L"DllUnregisterServer completed");
    return S_OK;
}
