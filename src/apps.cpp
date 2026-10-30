#include "common.h"
#include <thread>
#include <condition_variable>
#include <deque>
#include <appmodel.h>

std::vector<AppInfo> g_apps;
bool g_catalogReady = false;

// ================================================================ catalogo (shell:AppsFolder = todas las apps, Win32 y de la Store)
static std::wstring ResolveKnownFolder(const std::wstring& p) {
  if (p.size() < 39 || p[0] != L'{') return L"";
  size_t close = p.find(L'}');
  if (close == std::wstring::npos) return L"";
  GUID g;
  if (FAILED(CLSIDFromString(p.substr(0, close + 1).c_str(), &g))) return L"";
  PWSTR base = nullptr;
  if (FAILED(SHGetKnownFolderPath(g, 0, nullptr, &base))) return L"";
  std::wstring r = std::wstring(base) + p.substr(close + 1);
  CoTaskMemFree(base);
  return r;
}

static bool EndsWith(const std::wstring& s, const wchar_t* suf) {
  size_t n = wcslen(suf);
  return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

static bool SkipApp(const std::wstring& name, const std::wstring& id) {
  static const wchar_t* words[] = { L"uninstall", L"desinstal", L"readme", L"léame", L"release notes", L"documentation",
                                    L"license", L"licencia", L"website", L"sitio web" };
  std::wstring ln = Lower(name);
  for (auto w : words) if (ln.find(w) != std::wstring::npos) return true;
  for (auto e : { L".url", L".chm", L".txt", L".pdf", L".htm", L".html", L".ini", L".log", L".rtf" })
    if (EndsWith(id, e)) return true;
  return false;
}

static void CatalogThread() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  auto* out = new std::vector<AppInfo>;
  IShellItem* folder = nullptr;
  if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)))) {
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
      IShellItem* it = nullptr;
      while (en->Next(1, &it, nullptr) == S_OK) {
        PWSTR name = nullptr, parse = nullptr;
        it->GetDisplayName(SIGDN_NORMALDISPLAY, &name);
        it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parse);
        if (name && parse && !SkipApp(name, parse)) {
          AppInfo a;
          a.name = name; a.id = parse;
          std::wstring resolved = ResolveKnownFolder(a.id);
          if (EndsWith(resolved, L".exe")) a.exe = Lower(resolved);
          else if (a.id.find(L'\\') == std::wstring::npos && a.id.find(L':') == std::wstring::npos) a.aumid = Lower(a.id);
          if (a.exe.empty()) {
            IShellItem2* it2 = nullptr;
            if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&it2)))) {
              PWSTR target = nullptr;
              if (SUCCEEDED(it2->GetString(PKEY_Link_TargetParsingPath, &target)) && target) {
                if (EndsWith(target, L".exe")) a.exe = Lower(target);
                CoTaskMemFree(target);
              }
              it2->Release();
            }
          }
          out->push_back(std::move(a));
        }
        CoTaskMemFree(name); CoTaskMemFree(parse);
        it->Release();
      }
      en->Release();
    }
    folder->Release();
  }
  std::sort(out->begin(), out->end(), [](const AppInfo& a, const AppInfo& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
  });
  CoUninitialize();
  PostMessageW(g_msgWnd, WM_APP_CATALOG, 0, (LPARAM)out);
}

void StartCatalog() { std::thread(CatalogThread).detach(); }

const AppInfo* FindApp(const std::wstring& id) {
  for (auto& a : g_apps) if (_wcsicmp(a.id.c_str(), id.c_str()) == 0) return &a;
  return nullptr;
}

const AppInfo* MatchApp(const std::wstring& aumid, const std::wstring& exe) {
  if (!aumid.empty()) for (auto& a : g_apps) if (a.aumid == aumid) return &a;
  if (!exe.empty()) for (auto& a : g_apps) if (a.exe == exe) return &a;
  return nullptr;
}

std::wstring AppShellPath(const AppInfo& a) { return L"shell:AppsFolder\\" + a.id; }

void LaunchApp(const AppInfo& a, bool admin) {
  if (admin && !a.exe.empty()) RunShell(a.exe, L"", L"runas");
  else RunShell(AppShellPath(a));
}

std::vector<const AppInfo*> SearchApps(const std::wstring& q) {
  std::vector<std::pair<int, const AppInfo*>> r;
  std::wstring lq = Lower(q);
  for (auto& a : g_apps) {
    std::wstring ln = Lower(a.name);
    int score = -1;
    if (ln.rfind(lq, 0) == 0) score = 0;
    else if (ln.find(L" " + lq) != std::wstring::npos) score = 1;
    else if (IContains(a.name, q)) score = 2;
    else {  // iniciales: "vsc" -> Visual Studio Code
      std::wstring ini;
      bool start = true;
      for (wchar_t c : ln) { if (start && iswalnum(c)) ini += c; start = !iswalnum(c); }
      if (ini.rfind(lq, 0) == 0) score = 3;
    }
    if (score >= 0) r.push_back({ score, &a });
  }
  std::stable_sort(r.begin(), r.end(), [](auto& x, auto& y) { return x.first < y.first; });
  std::vector<const AppInfo*> out;
  for (auto& p : r) out.push_back(p.second);
  return out;
}

// ================================================================ ventanas abiertas
std::wstring ExeOfWindow(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return L"";
  wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
  std::wstring r;
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) r = Lower(buf);
  CloseHandle(p);
  return r;
}

// apps empaquetadas (Store) a veces no publican el ID en la ventana; el proceso si lo conoce
static std::wstring ProcessAumid(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return L"";
  wchar_t buf[512]; UINT32 n = 512;
  std::wstring r;
  if (GetApplicationUserModelId(p, &n, buf) == ERROR_SUCCESS) r = Lower(buf);
  CloseHandle(p);
  return r;
}

std::wstring AumidOfWindow(HWND h) {
  IPropertyStore* ps = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&ps)))) {
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(ps->GetValue(PKEY_AppUserModel_ID, &pv)) && pv.vt == VT_LPWSTR) r = Lower(pv.pwszVal);
    PropVariantClear(&pv);
    ps->Release();
  }
  if (r.empty() || !MatchApp(r, L"")) {
    std::wstring pr = ProcessAumid(h);
    if (!pr.empty()) r = pr;
  }
  return r;
}

static bool IsAppWindow(HWND h) {
  if (!IsWindowVisible(h) || IsOwnWindow(h)) return false;
  LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
  if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
  if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
  if (ex & WS_EX_NOACTIVATE && !(ex & WS_EX_APPWINDOW)) return false;
  int cloaked = 0;
  DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
  if (cloaked) return false;
  if (GetWindowTextLengthW(h) == 0) return false;
  wchar_t cls[64];
  GetClassNameW(h, cls, 64);
  for (auto c : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow" })
    if (!wcscmp(cls, c)) return false;
  return true;
}

std::vector<RunningWin> EnumRunning() {
  std::vector<RunningWin> out;
  EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    if (!IsAppWindow(h)) return TRUE;
    RunningWin r{ h };
    r.exe = ExeOfWindow(h);
    r.aumid = AumidOfWindow(h);
    if (r.aumid.empty() && EndsWith(r.exe, L"\\explorer.exe")) r.aumid = L"microsoft.windows.explorer";
    r.key = r.aumid.empty() ? r.exe : r.aumid;
    wchar_t t[256];
    GetWindowTextW(h, t, 256);
    r.title = t;
    ((std::vector<RunningWin>*)lp)->push_back(std::move(r));
    return TRUE;
  }, (LPARAM)&out);
  return out;
}

static std::wstring FileDescription(const std::wstring& exe) {
  DWORD dummy;
  DWORD n = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
  if (!n) return L"";
  std::vector<BYTE> buf(n);
  if (!GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) return L"";
  struct { WORD lang, cp; } *tr; UINT len;
  if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < 4) return L"";
  wchar_t q[64];
  swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
  wchar_t* v; UINT vl;
  if (VerQueryValueW(buf.data(), q, (void**)&v, &vl) && vl > 1) return v;
  return L"";
}

std::wstring AppNameForWindow(HWND h) {
  static std::map<std::wstring, std::wstring> cache;
  std::wstring exe = ExeOfWindow(h), aumid = AumidOfWindow(h);
  if (aumid.empty() && EndsWith(exe, L"\\explorer.exe")) aumid = L"microsoft.windows.explorer";
  if (auto a = MatchApp(aumid, exe)) return a->name;
  if (exe.empty()) return L"";
  auto it = cache.find(exe);
  if (it != cache.end()) return it->second;
  std::wstring d = FileDescription(exe);
  if (d.empty()) {
    d = exe.substr(exe.find_last_of(L'\\') + 1);
    d = d.substr(0, d.size() - 4);
    if (!d.empty()) d[0] = towupper(d[0]);
  }
  return cache[exe] = d;
}

void ActivateWindow(HWND h) {
  if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
  if (!SetForegroundWindow(h)) SwitchToThisWindow(h, TRUE);
}

