#include "common.h"
#include <shlwapi.h>
#include <thread>
#include <atomic>
#include <ctime>
#include <set>
#include <deque>
#include <memory>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

// Zyntra (creado por Zargch): biblioteca de juegos instalados (Steam, Epic, Xbox, Riot y juegos agregados a mano)
// con caratulas, descripcion y boton Jugar. Se vuelve a escanear sola mientras esta abierta, asi
// que los juegos que se instalan o desinstalan aparecen y desaparecen sin hacer nada.

struct Game {
  std::wstring key, name, platform, ident;  // ident: identidad del paquete de Xbox (en minusculas)
  std::vector<std::wstring> paths;           // carpetas donde vive el juego (para saber si esta abierto)
  std::wstring launch, args, folder, uninstall, store, exe;
  std::wstring cover, hero, heroBlur, logo, desc;
  std::wstring shot;                         // captura del juego: fondo del carrusel (distinta de la caratula)
  ULONGLONG size = 0;
  long long lastPlayed = 0;
  unsigned steamId = 0;
};

// ---------------------------------------------------------------- utilidades
static std::string ReadFileA(const std::wstring& p) {
  HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
  std::string s((size_t)std::min<LONGLONG>(sz.QuadPart, 16 * 1024 * 1024), 0);
  DWORD got = 0;
  ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
  CloseHandle(f);
  s.resize(got);
  return s;
}

static void WriteFileA(const std::wstring& p, const std::string& s) {
  HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w; WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr);
  CloseHandle(f);
}

static std::string ToUtf8(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// nombre normalizado para comparar ("F1® 24" == "f124")
static std::wstring Norm(const std::wstring& s) {
  std::wstring r;
  for (wchar_t c : Lower(s)) if (iswalnum(c) && c < 0x80) r += c;
  return r;
}

static std::wstring SafeKey(std::wstring s) {
  for (auto& c : s) if (!iswalnum(c)) c = L'_';
  return s;
}

static std::wstring CacheDir() {
  std::wstring d = g_dir + L"\\games";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

// valor "clave" "valor" de los archivos .vdf/.acf de Steam
static std::string Vdf(const std::string& s, const char* key, size_t from = 0) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = s.find(k, from);
  if (p == std::string::npos) return "";
  size_t a = s.find('"', p + k.size());
  if (a == std::string::npos) return "";
  size_t b = s.find('"', a + 1);
  return b == std::string::npos ? "" : s.substr(a + 1, b - a - 1);
}

// cadena JSON (con escapes) para una clave dada
static std::string JsonStr(const std::string& s, const char* key, size_t from = 0, size_t* endOut = nullptr) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = s.find(k, from);
  if (p == std::string::npos) return "";
  p = s.find(':', p + k.size());
  if (p == std::string::npos) return "";
  p = s.find_first_not_of(" \t\r\n", p + 1);
  if (p == std::string::npos || s[p] != '"') return "";
  std::string out;
  for (size_t i = p + 1; i < s.size(); i++) {
    char c = s[i];
    if (c == '"') { if (endOut) *endOut = i; break; }
    if (c == '\\' && i + 1 < s.size()) {
      char e = s[++i];
      if (e == 'n') out += '\n'; else if (e == 't') out += ' '; else if (e == 'r') {}
      else if (e == 'u' && i + 4 < s.size()) {
        unsigned cp = strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
          unsigned lo = strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16);
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          i += 6;
        }
        std::wstring w;
        if (cp >= 0x10000) { w += (wchar_t)(0xD800 + ((cp - 0x10000) >> 10)); w += (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
        else w += (wchar_t)cp;
        out += ToUtf8(w);
      } else out += e;
    } else out += c;
  }
  return out;
}

static std::wstring StripHtml(std::wstring s) {
  std::wstring r;
  bool tag = false;
  for (wchar_t c : s) { if (c == L'<') tag = true; else if (c == L'>') tag = false; else if (!tag) r += c; }
  struct { const wchar_t* e; const wchar_t* v; } ents[] = { { L"&quot;", L"\"" }, { L"&amp;", L"&" }, { L"&#39;", L"'" }, { L"&lt;", L"<" }, { L"&gt;", L">" }, { L"&nbsp;", L" " } };
  for (auto& e : ents) for (size_t p; (p = r.find(e.e)) != std::wstring::npos;) r.replace(p, wcslen(e.e), e.v);
  return r;
}

// ---------------------------------------------------------------- escaneo de juegos instalados
static std::wstring FindInDir(const std::wstring& dir, std::initializer_list<const wchar_t*> names) {
  // busca el primer archivo con alguno de esos nombres (Steam a veces los guarda en una subcarpeta)
  for (auto n : names) {
    if (Exists(dir + L"\\" + n)) return dir + L"\\" + n;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    std::wstring found;
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') {
        std::wstring p = dir + L"\\" + fd.cFileName + L"\\" + n;
        if (Exists(p)) { found = p; break; }
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!found.empty()) return found;
  }
  return L"";
}

static void ScanSteam(std::vector<Game>& out) {
  wchar_t sp[MAX_PATH] = {}; DWORD sz = sizeof sp;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, sp, &sz) != ERROR_SUCCESS) return;
  std::wstring steam = sp;
  for (auto& c : steam) if (c == L'/') c = L'\\';
  std::string vdf = ReadFileA(steam + L"\\steamapps\\libraryfolders.vdf");
  std::vector<std::wstring> libs = { steam };
  for (size_t p = 0; (p = vdf.find("\"path\"", p)) != std::string::npos; p += 6) {
    std::wstring l = Utf8(Vdf(vdf, "path", p));
    for (size_t i; (i = l.find(L"\\\\")) != std::wstring::npos;) l.erase(i, 1);
    if (std::find_if(libs.begin(), libs.end(), [&](const std::wstring& x) { return !_wcsicmp(x.c_str(), l.c_str()); }) == libs.end()) libs.push_back(l);
  }
  std::wstring cache = steam + L"\\appcache\\librarycache";
  for (auto& lib : libs) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((lib + L"\\steamapps\\appmanifest_*.acf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      std::string acf = ReadFileA(lib + L"\\steamapps\\" + fd.cFileName);
      unsigned id = (unsigned)strtoul(Vdf(acf, "appid").c_str(), nullptr, 10);
      std::wstring name = Utf8(Vdf(acf, "name"));
      int flags = atoi(Vdf(acf, "StateFlags").c_str());
      if (!id || name.empty() || !(flags & 4)) continue;  // 4 = totalmente instalado
      std::wstring ln = Lower(name);
      if (id == 228980 || ln.find(L"redistributable") != std::wstring::npos || ln.find(L"runtime") != std::wstring::npos ||
          ln.find(L"dedicated server") != std::wstring::npos || ln.find(L"steamvr") != std::wstring::npos || ln.find(L"proton") != std::wstring::npos) continue;
      Game g;
      g.key = L"steam_" + std::to_wstring(id);
      g.name = name; g.platform = L"Steam"; g.steamId = id;
      g.launch = L"steam://rungameid/" + std::to_wstring(id);
      g.uninstall = L"steam://uninstall/" + std::to_wstring(id);
      g.store = L"https://store.steampowered.com/app/" + std::to_wstring(id);
      g.folder = lib + L"\\steamapps\\common\\" + Utf8(Vdf(acf, "installdir"));
      g.paths.push_back(Lower(g.folder));
      g.size = _strtoui64(Vdf(acf, "SizeOnDisk").c_str(), nullptr, 10);
      g.lastPlayed = _strtoi64(Vdf(acf, "LastPlayed").c_str(), nullptr, 10);
      std::wstring c = cache + L"\\" + std::to_wstring(id);
      g.cover = FindInDir(c, { L"library_600x900.jpg", L"library_capsule.jpg" });
      if (g.cover.empty() && Exists(cache + L"\\" + std::to_wstring(id) + L"_library_600x900.jpg")) g.cover = cache + L"\\" + std::to_wstring(id) + L"_library_600x900.jpg";
      g.hero = FindInDir(c, { L"library_hero.jpg" });
      g.heroBlur = FindInDir(c, { L"library_hero_blur.jpg" });
      g.logo = FindInDir(c, { L"logo.png" });
      out.push_back(g);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
}

static void ScanEpic(std::vector<Game>& out) {
  std::wstring dir = KnownPath(FOLDERID_ProgramData) + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests";
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*.item").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::string j = ReadFileA(dir + L"\\" + fd.cFileName);
    std::string app = JsonStr(j, "AppName"), main = JsonStr(j, "MainGameAppName"), exe = JsonStr(j, "LaunchExecutable");
    if (j.find("\"bIsIncompleteInstall\": true") != std::string::npos || exe.empty() || (!main.empty() && main != app)) continue;
    Game g;
    g.name = Utf8(JsonStr(j, "DisplayName"));
    g.key = L"epic_" + SafeKey(Utf8(app));
    g.platform = L"Epic";
    std::wstring ns = Utf8(JsonStr(j, "CatalogNamespace")), item = Utf8(JsonStr(j, "CatalogItemId")), an = Utf8(app);
    g.launch = L"com.epicgames.launcher://apps/" + ns + L"%3A" + item + L"%3A" + an + L"?action=launch&silent=true";
    g.uninstall = L"com.epicgames.launcher://store/library";
    g.folder = Utf8(JsonStr(j, "InstallLocation"));
    std::wstring ex = Utf8(exe);
    for (auto& c : ex) if (c == L'/') c = L'\\';
    g.exe = g.folder + L"\\" + ex;
    g.size = _strtoui64(j.c_str() + std::min(j.size(), j.find("\"InstallSize\"") == std::string::npos ? j.size() : j.find("\"InstallSize\"") + 14), nullptr, 10);
    g.paths.push_back(Lower(g.folder));
    if (!g.name.empty() && Exists(g.folder)) out.push_back(g);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

static std::string XmlAttr(const std::string& s, const char* tag, const char* attr) {
  // "<Executable" seguido de espacio o salto de linea (y no "<ExecutableList>")
  size_t t = 0;
  std::string open = std::string("<") + tag;
  for (;; t++) {
    t = s.find(open, t);
    if (t == std::string::npos) return "";
    char nx = t + open.size() < s.size() ? s[t + open.size()] : 0;
    if (nx == ' ' || nx == '\r' || nx == '\n' || nx == '\t') break;
  }
  if (t == std::string::npos) return "";
  size_t e = s.find('>', t);
  std::string a = std::string(" ") + attr + "=\"";
  size_t p = s.find(a, t);
  if (p == std::string::npos || p > e) return "";
  p += a.size();
  return s.substr(p, s.find('"', p) - p);
}

// Xbox / Game Pass: las carpetas de juegos que declara cada disco (.GamingRoot) y el registro de
// Gaming Services (todas las instalaciones de juegos de Xbox con su ruta real, incluso en WindowsApps)
static std::vector<std::wstring> XboxCandidateDirs() {
  std::vector<std::wstring> dirs;
  DWORD drives = GetLogicalDrives();
  for (int d = 0; d < 26; d++) {
    if (!(drives & (1 << d))) continue;
    std::wstring root = std::wstring(1, (wchar_t)(L'A' + d)) + L":\\";
    std::string gr = ReadFileA(root + L".GamingRoot");
    std::vector<std::wstring> folders;
    if (gr.size() > 8) {
      std::wstring s((const wchar_t*)(gr.data() + 8), (gr.size() - 8) / 2), cur;
      for (wchar_t c : s) { if (c) cur += c; else { if (!cur.empty()) folders.push_back(cur); cur.clear(); } }
      if (!cur.empty()) folders.push_back(cur);
    }
    if (Exists(root + L"XboxGames")) folders.push_back(L"XboxGames");
    for (auto& f : folders) {
      std::wstring base = root + (f[0] == L'\\' ? f.substr(1) : f);
      WIN32_FIND_DATAW fd;
      HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
      if (h == INVALID_HANDLE_VALUE) continue;
      do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && _wcsicmp(fd.cFileName, L"GameSave"))
          dirs.push_back(base + L"\\" + fd.cFileName);
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
  }
  HKEY rootKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\GamingServices\\PackageRepository\\Root", 0, KEY_READ | KEY_WOW64_64KEY, &rootKey) == ERROR_SUCCESS) {
    wchar_t name[512];
    for (DWORD i = 0; RegEnumKeyW(rootKey, i, name, 512) == ERROR_SUCCESS; i++) {
      HKEY sub;
      if (RegOpenKeyExW(rootKey, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
      wchar_t child[512];
      for (DWORD j = 0; RegEnumKeyW(sub, j, child, 512) == ERROR_SUCCESS; j++) {
        wchar_t val[1024]; DWORD sz = sizeof val;
        if (RegGetValueW(sub, child, L"Root", RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS) {
          std::wstring p = val;
          if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
          while (!p.empty() && p.back() == L'\\') p.pop_back();
          if (!p.empty()) dirs.push_back(p);
        }
      }
      RegCloseKey(sub);
    }
    RegCloseKey(rootKey);
  }
  return dirs;
}

static void ScanXbox(std::vector<Game>& out) {
  for (auto& gdir : XboxCandidateDirs()) {
    std::wstring content = gdir + L"\\Content";
    std::string cfg = ReadFileA(content + L"\\MicrosoftGame.config");
    if (cfg.empty()) { content = gdir; cfg = ReadFileA(content + L"\\MicrosoftGame.config"); }
    if (cfg.empty()) continue;
    std::wstring ident = Lower(Utf8(XmlAttr(cfg, "Identity", "Name")));
    if (ident.empty()) continue;
    // la misma instalacion puede aparecer dos veces (carpeta de juegos + paquete): se unen
    auto dup = std::find_if(out.begin(), out.end(), [&](const Game& g) { return g.ident == ident; });
    if (dup != out.end()) { dup->paths.push_back(Lower(gdir)); continue; }
    Game g;
    g.ident = ident;
    g.key = L"xbox_" + SafeKey(ident);
    g.platform = L"Xbox";
    g.folder = gdir;
    g.paths.push_back(Lower(gdir));
    const AppInfo* app = nullptr;
    for (auto& a : g_apps)
      if (a.aumid.rfind(ident + L"_", 0) == 0) { app = &a; break; }
    std::wstring disp = Utf8(XmlAttr(cfg, "ShellVisuals", "DefaultDisplayName"));
    if (disp.empty() || disp.rfind(L"ms-resource", 0) == 0) {
      if (app) disp = app->name;
      else {
        disp = gdir.substr(gdir.find_last_of(L'\\') + 1);
        size_t us = disp.find(L'_');
        if (us != std::wstring::npos && disp.find(L'.') < us) disp = disp.substr(disp.find(L'.') + 1, us - disp.find(L'.') - 1);
        for (size_t p; (p = disp.find(L"- ")) != std::wstring::npos;) disp.replace(p, 2, L": ");
      }
    }
    g.name = disp;
    std::wstring logo = Utf8(XmlAttr(cfg, "ShellVisuals", "Square480x480Logo"));
    if (logo.empty()) logo = Utf8(XmlAttr(cfg, "ShellVisuals", "Square150x150Logo"));
    if (!logo.empty() && Exists(content + L"\\" + logo)) g.logo = content + L"\\" + logo;
    std::wstring splash = Utf8(XmlAttr(cfg, "ShellVisuals", "SplashScreenImage"));
    if (!splash.empty() && Exists(content + L"\\" + splash)) g.hero = content + L"\\" + splash;
    if (app) g.launch = AppShellPath(*app);
    else {
      // carpeta de paquete "Nombre_version_arq__editor" -> AUMID "Nombre_editor!IdEjecutable"
      std::wstring folder = gdir.substr(gdir.find_last_of(L'\\') + 1);
      size_t dd = folder.rfind(L"__");
      std::wstring exeId = Utf8(XmlAttr(cfg, "Executable", "Id"));
      if (dd != std::wstring::npos && !exeId.empty())
        g.launch = L"shell:AppsFolder\\" + folder.substr(0, folder.find(L'_')) + L"_" + folder.substr(dd + 2) + L"!" + exeId;
    }
    g.uninstall = L"ms-settings:appsfeatures";
    if (!g.launch.empty()) out.push_back(g);
  }
}

static void ScanRiot(std::vector<Game>& out) {
  std::wstring pd = KnownPath(FOLDERID_ProgramData) + L"\\Riot Games";
  std::string inst = ReadFileA(pd + L"\\RiotClientInstalls.json");
  std::wstring rc = Utf8(JsonStr(inst, "rc_default"));
  for (auto& c : rc) if (c == L'/') c = L'\\';
  if (rc.empty() || !Exists(rc)) return;
  struct { const wchar_t* id; const wchar_t* product; const wchar_t* name; } known[] = {
    { L"valorant.live", L"valorant", L"VALORANT" }, { L"league_of_legends.live", L"league_of_legends", L"League of Legends" },
    { L"bacon.live", L"bacon", L"Legends of Runeterra" }, { L"lion.live", L"lion", L"2XKO" },
  };
  for (auto& k : known) {
    std::string y = ReadFileA(pd + L"\\Metadata\\" + k.id + L"\\" + k.id + L".product_settings.yaml");
    size_t p = y.find("product_install_full_path:");
    if (p == std::string::npos) continue;
    size_t a = y.find('"', p), b = a == std::string::npos ? a : y.find('"', a + 1);
    if (b == std::string::npos) continue;
    std::wstring dir = Utf8(y.substr(a + 1, b - a - 1));
    for (auto& c : dir) if (c == L'/') c = L'\\';
    if (!Exists(dir)) continue;
    Game g;
    g.key = std::wstring(L"riot_") + k.product;
    g.name = k.name; g.platform = L"Riot";
    g.launch = rc;
    g.args = std::wstring(L"--launch-product=") + k.product + L" --launch-patchline=live";
    g.folder = dir;
    g.paths.push_back(Lower(dir));
    g.exe = rc;
    g.uninstall = L"ms-settings:appsfeatures";
    out.push_back(g);
  }
}

static std::wstring IniPath() { return g_dir + L"\\config.ini"; }

static std::vector<std::wstring> IniList(const wchar_t* prefix) {
  std::vector<std::wstring> v;
  int n = GetPrivateProfileIntW(L"Games", (std::wstring(prefix) + L"_count").c_str(), 0, IniPath().c_str());
  for (int i = 0; i < n; i++) {
    wchar_t b[1024];
    GetPrivateProfileStringW(L"Games", (std::wstring(prefix) + std::to_wstring(i)).c_str(), L"", b, 1024, IniPath().c_str());
    if (b[0]) v.push_back(b);
  }
  return v;
}

static void IniSaveList(const wchar_t* prefix, const std::vector<std::wstring>& v) {
  WritePrivateProfileStringW(L"Games", (std::wstring(prefix) + L"_count").c_str(), std::to_wstring(v.size()).c_str(), IniPath().c_str());
  for (size_t i = 0; i < v.size(); i++)
    WritePrivateProfileStringW(L"Games", (std::wstring(prefix) + std::to_wstring(i)).c_str(), v[i].c_str(), IniPath().c_str());
}

// carpetas que no dicen el nombre del juego ("D:\Juego\Binaries\Win64\juego.exe")
static bool GenericDir(const std::wstring& n) {
  static const wchar_t* g[] = { L"bin", L"binaries", L"win64", L"win32", L"wingdk", L"x64", L"x86", L"game", L"games", L"juegos", L"common",
                                L"steamapps", L"program files", L"program files (x86)", L"launcher", L"release", L"shipping", L"app", L"client" };
  std::wstring l = Lower(n);
  if (l.size() <= 2 || l.find(L':') != std::wstring::npos) return true;  // raiz de una unidad
  for (auto s : g) if (l == s) return true;
  return false;
}

static std::wstring ProductName(const std::wstring& exe) {
  DWORD h = 0, n = GetFileVersionInfoSizeW(exe.c_str(), &h);
  if (!n) return L"";
  std::vector<BYTE> buf(n);
  if (!GetFileVersionInfoW(exe.c_str(), 0, n, buf.data())) return L"";
  struct { WORD lang, cp; }* tr = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < 4) return L"";
  wchar_t q[64];
  swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\ProductName", tr->lang, tr->cp);
  wchar_t* v = nullptr;
  if (!VerQueryValueW(buf.data(), q, (void**)&v, &len) || !v) return L"";
  std::wstring s = v;
  while (!s.empty() && iswspace(s.back())) s.pop_back();
  return s;
}

// nombre de un juego añadido a mano: el que puso el usuario, si no la carpeta del juego
// ("D:\Dragon Ball Sparking ZERO\SparkingZERO.exe"), el nombre de producto del exe o el nombre del archivo
static std::wstring CustomName(const std::wstring& key, const std::wstring& p) {
  wchar_t b[260];
  GetPrivateProfileStringW(L"Games", (L"customname_" + key).c_str(), L"", b, 260, IniPath().c_str());
  if (*b) return b;
  std::wstring dir = p.substr(0, p.find_last_of(L'\\'));
  for (int up = 0; up < 4 && dir.size() > 3; up++) {
    std::wstring n = dir.substr(dir.find_last_of(L'\\') + 1);
    if (!GenericDir(n)) return n;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
  }
  std::wstring base = p.substr(p.find_last_of(L'\\') + 1);
  size_t dot = base.find_last_of(L'.');
  if (dot != std::wstring::npos) base = base.substr(0, dot);
  if (p.size() > 4 && Lower(p.substr(p.size() - 4)) == L".exe") {
    std::wstring pn = ProductName(p);
    if (pn.size() > 2 && !GenericDir(pn)) return pn;
  }
  return base;
}

static void ScanCustom(std::vector<Game>& out) {
  for (auto& p : IniList(L"custom")) {
    if (!Exists(p)) continue;
    Game g;
    g.key = L"custom_" + SafeKey(Lower(p));
    g.name = CustomName(g.key, p);
    g.platform = L"Otros";
    g.launch = p; g.exe = p;
    g.folder = p.substr(0, p.find_last_of(L'\\'));
    g.paths.push_back(Lower(g.folder));
    out.push_back(g);
  }
}

// ---------------------------------------------------------------- "jugado por ultima vez"
static long long FileTimeToUnix(long long ft) { return ft > 116444736000000000LL ? (ft - 116444736000000000LL) / 10000000 : 0; }

// Historial de Windows (UserAssist): accesos directos, ejecutables, apps de la Store y enlaces
// steam:// que se abrieron desde el escritorio, el menu Inicio o la barra de tareas
static std::vector<std::pair<std::wstring, long long>> UserAssist() {
  std::vector<std::pair<std::wstring, long long>> out;
  HKEY ua;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist", 0, KEY_READ, &ua) != ERROR_SUCCESS) return out;
  wchar_t guid[128];
  for (DWORD i = 0; RegEnumKeyW(ua, i, guid, 128) == ERROR_SUCCESS; i++) {
    HKEY cnt;
    if (RegOpenKeyExW(ua, (std::wstring(guid) + L"\\Count").c_str(), 0, KEY_READ, &cnt) != ERROR_SUCCESS) continue;
    for (DWORD j = 0;; j++) {
      wchar_t name[1024]; DWORD nl = 1024;
      BYTE data[128]; DWORD dl = sizeof data, type;
      LONG r = RegEnumValueW(cnt, j, name, &nl, nullptr, &type, data, &dl);
      if (r == ERROR_NO_MORE_ITEMS) break;
      if (r != ERROR_SUCCESS || dl < 68) continue;
      std::wstring n = name;
      for (auto& c : n) {  // ROT13
        if (c >= L'a' && c <= L'z') c = L'a' + (c - L'a' + 13) % 26;
        else if (c >= L'A' && c <= L'Z') c = L'A' + (c - L'A' + 13) % 26;
      }
      long long ft; memcpy(&ft, data + 60, 8);
      long long t = FileTimeToUnix(ft);
      if (!t) continue;
      // "{GUID de carpeta conocida}\resto" -> ruta real
      if (n.size() > 39 && n[0] == L'{' && n[37] == L'}') {
        GUID g;
        PWSTR base = nullptr;
        if (SUCCEEDED(CLSIDFromString(n.substr(0, 38).c_str(), &g)) && SUCCEEDED(SHGetKnownFolderPath(g, 0, nullptr, &base))) n = std::wstring(base) + n.substr(38);
        CoTaskMemFree(base);
      }
      out.push_back({ Lower(n), t });
    }
    RegCloseKey(cnt);
  }
  RegCloseKey(ua);
  return out;
}

static long long LastPlayedFrom(const Game& g, const std::vector<std::pair<std::wstring, long long>>& ua) {
  long long best = g.lastPlayed;
  std::wstring steamUrl = g.steamId ? L"steam://rungameid/" + std::to_wstring(g.steamId) : L"", nn = Norm(g.name);
  for (auto& e : ua) {
    const std::wstring& n = e.first;
    bool hit = false;
    if (!steamUrl.empty() && n == steamUrl) hit = true;
    else if (!g.ident.empty() && (n.rfind(g.ident + L"_", 0) == 0 || n.find(L"\\windowsapps\\" + g.ident + L"_") != std::wstring::npos)) hit = true;
    else if (n.size() > 4 && (n.compare(n.size() - 4, 4, L".lnk") == 0 || n.compare(n.size() - 4, 4, L".url") == 0)) {
      std::wstring b = n.substr(n.find_last_of(L'\\') + 1);
      hit = Norm(b.substr(0, b.size() - 4)) == nn;
    } else for (auto& p : g.paths) if (!p.empty() && n.rfind(p + L"\\", 0) == 0) { hit = true; break; }
    if (hit) best = std::max(best, e.second);
  }
  wchar_t b[32];  // lo que Zyntra vio abierto
  GetPrivateProfileStringW(L"Games", (L"played_" + g.key).c_str(), L"0", b, 32, IniPath().c_str());
  return std::max(best, _wtoi64(b));
}

// juegos y carpetas conocidos, para marcar como "jugado ahora" lo que este abierto
static std::mutex s_trackMx;
static std::vector<std::pair<std::wstring, std::vector<std::wstring>>> s_track;

static std::vector<Game> ScanAll() {
  std::vector<Game> all, out;
  ScanSteam(all); ScanRiot(all); ScanEpic(all); ScanXbox(all); ScanCustom(all);
  auto hidden = IniList(L"hidden");
  auto ua = UserAssist();
  std::vector<std::wstring> seen;
  for (auto& g : all) {
    std::wstring n = Norm(g.name);
    if (std::find(hidden.begin(), hidden.end(), g.key) != hidden.end()) continue;
    if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;  // el mismo juego en dos tiendas
    seen.push_back(n);
    g.lastPlayed = LastPlayedFrom(g, ua);
    std::wstring custom = CacheDir() + L"\\" + g.key + L"_custom.jpg";  // caratula elegida por el usuario (opcional)
    if (Exists(custom)) g.cover = custom;
    out.push_back(g);
  }
  std::lock_guard<std::mutex> lk(s_trackMx);
  s_track.clear();
  for (auto& g : out) s_track.push_back({ g.key, g.paths });
  return out;
}

void GameMode_StartTracking() {
  std::thread([] { CoInitializeEx(nullptr, COINIT_MULTITHREADED); ScanAll(); CoUninitialize(); }).detach();
}

// se llama cada pocos segundos: si un juego conocido tiene una ventana abierta, se anota la hora
void GameMode_Track() {
  static std::map<std::wstring, long long> lastWrite;
  std::vector<std::pair<std::wstring, std::vector<std::wstring>>> track;
  { std::lock_guard<std::mutex> lk(s_trackMx); track = s_track; }
  if (track.empty()) return;
  long long now = (long long)time(nullptr);
  for (auto& r : EnumRunning()) {
    for (auto& t : track) {
      bool in = false;
      for (auto& p : t.second) if (!p.empty() && r.exe.rfind(p + L"\\", 0) == 0) { in = true; break; }
      if (!in || now - lastWrite[t.first] < 60) continue;
      lastWrite[t.first] = now;
      WritePrivateProfileStringW(L"Games", (L"played_" + t.first).c_str(), std::to_wstring(now).c_str(), IniPath().c_str());
    }
  }
}

// ---------------------------------------------------------------- caratulas y descripciones
static bool IsImage(const std::string& b) {
  return b.size() > 1000 && ((unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8 || (unsigned char)b[0] == 0x89 && b[1] == 'P');
}

static unsigned SteamSearch(const std::wstring& name) {
  std::wstring q;
  for (unsigned char c : ToUtf8(name)) { if (isalnum(c)) q += (wchar_t)c; else { wchar_t b[4]; swprintf_s(b, L"%%%02X", c); q += b; } }
  std::string j = HttpGet(L"store.steampowered.com", L"/api/storesearch/?l=spanish&cc=US&term=" + q);
  std::wstring want = Norm(name);
  for (size_t p = 0; (p = j.find("\"id\":", p)) != std::string::npos; p += 5) {
    unsigned id = (unsigned)strtoul(j.c_str() + p + 5, nullptr, 10);
    std::wstring n = Norm(Utf8(JsonStr(j, "name", p)));
    if (!n.empty() && (n == want || (want.size() > 4 && (n.find(want) == 0 || want.find(n) == 0)))) return id;
  }
  return 0;
}

// Tienda de Microsoft (Xbox / Game Pass y casi cualquier juego de PC): caratula vertical (Poster),
// fondo panoramico (SuperHeroArt) y descripcion en español
static bool SimilarName(const std::wstring& a, const std::wstring& b) {
  std::wstring x = Norm(a), y = Norm(b);
  if (x.empty() || y.empty()) return false;
  if (x == y || (x.size() > 4 && y.find(x) == 0) || (y.size() > 4 && x.find(y) == 0)) return true;
  auto first = [](const std::wstring& s) { std::wstring w; for (wchar_t c : Lower(s)) { if (iswalnum(c)) w += c; else if (!w.empty()) break; } return w; };
  std::wstring fa = first(a), fb = first(b);
  return fa.size() >= 5 && fa == fb;
}

static bool MsStore(const std::wstring& name, std::string& poster, std::string& hero, std::string& desc) {
  std::wstring q;
  for (unsigned char c : ToUtf8(name)) { if (isalnum(c)) q += (wchar_t)c; else { wchar_t b[4]; swprintf_s(b, L"%%%02X", c); q += b; } }
  std::string s = HttpGet(L"displaycatalog.mp.microsoft.com", L"/v7.0/productFamilies/autosuggest?market=US&languages=es-es&productFamilyNames=Games&query=" + q);
  std::string id;
  for (size_t p = 0; (p = s.find("\"ProductId\"", p)) != std::string::npos; p += 11) {
    std::string pid = JsonStr(s, "ProductId", p);
    size_t t = s.rfind("\"Title\"", p);
    std::string title = JsonStr(s, "Title", t == std::string::npos ? p : t);
    if (SimilarName(name, Utf8(title))) { id = pid; break; }
  }
  if (id.empty()) return false;
  std::string j = HttpGet(L"displaycatalog.mp.microsoft.com", L"/v7.0/products?market=US&languages=es-es&bigIds=" + Utf8(id));
  auto img = [&](const char* purpose) -> std::string {
    size_t p = j.find(std::string("\"ImagePurpose\":\"") + purpose + "\"");
    return p == std::string::npos ? "" : JsonStr(j, "Uri", p);
  };
  poster = img("Poster");
  if (poster.empty()) poster = img("BoxArt");
  hero = img("SuperHeroArt");
  if (hero.empty()) hero = img("TitledHeroArt");
  desc = JsonStr(j, "ShortDescription");
  if (desc.empty()) desc = JsonStr(j, "ProductDescription");
  if (desc.size() > 700) desc = desc.substr(0, desc.find(". ", 500) == std::string::npos ? 700 : desc.find(". ", 500) + 1);
  return !poster.empty();
}

static bool StoreImage(const std::string& uri, const wchar_t* size, const std::wstring& file) {
  if (uri.size() < 3) return false;
  std::wstring u = Utf8(uri.rfind("//", 0) == 0 ? uri.substr(2) : uri);
  size_t slash = u.find(L'/');
  if (slash == std::wstring::npos) return false;
  std::string data = HttpGet(u.substr(0, slash).c_str(), u.substr(slash) + size);
  if (!IsImage(data)) return false;
  WriteFileA(file, data);
  return true;
}

// borra las busquedas fallidas en cache (y con everything, todo lo descargado) para volver a buscar
static void ForgetMisses(const std::wstring& key, bool everything) {
  std::wstring base = CacheDir() + L"\\" + key;
  auto drop = [&](const wchar_t* ext, const char* miss) {
    std::wstring f = base + ext;
    if (Exists(f) && (everything || ReadFileA(f) == miss)) DeleteFileW(f.c_str());
  };
  drop(L".ms", "0"); drop(L".id", "0"); drop(L".msid", "0"); drop(L".txt", " ");
  drop(L".gameplay", "-"); drop(L".shot", "-"); drop(L".rep", "-"); drop(L".gal", "0");
  DeleteFileW((base + L".trailer").c_str());  // marcador del formato anterior de videos
  if (everything)
    for (auto ext : { L"_cover.jpg", L"_hero.jpg", L"_shot.jpg", L"_gameplay.mp4", L"_trailer.mp4" }) DeleteFileW((base + ext).c_str());
}

static void FetchShot(Game& g, unsigned steamId);

static void Enrich(Game& g) {
  std::wstring dir = CacheDir(), base = dir + L"\\" + g.key;
  // cambio el nombre con que se busca (p. ej. un juego añadido al que se le puso nombre): se busca de nuevo
  std::string q = ToUtf8(g.name);
  if (ReadFileA(base + L".q") != q) {
    ForgetMisses(g.key, g.key.rfind(L"custom_", 0) == 0 && Exists(base + L".q"));
    WriteFileA(base + L".q", q);
  }
  // fuera de Steam: primero la tienda de Microsoft (una sola vez por juego; el resultado queda en cache)
  if (!g.steamId) {
    std::wstring marker = base + L".ms";
    if (!Exists(marker)) {
      std::string poster, hero, desc;
      bool ok = MsStore(g.name, poster, hero, desc);
      if (ok) {
        StoreImage(poster, L"?w=600&h=900&q=90", base + L"_cover.jpg");
        if (!hero.empty()) StoreImage(hero, L"?w=1280&h=720&q=85", base + L"_hero.jpg");
        if (!desc.empty()) WriteFileA(base + L".txt", desc);
      }
      WriteFileA(marker, ok ? "1" : "0");
    }
    if (g.cover.empty() && Exists(base + L"_cover.jpg")) g.cover = base + L"_cover.jpg";
    if ((g.hero.empty() || g.platform == L"Xbox") && Exists(base + L"_hero.jpg")) g.hero = base + L"_hero.jpg";
  }
  unsigned id = g.steamId;
  std::wstring idFile = base + L".id";
  if (!id) {
    std::string cached = ReadFileA(idFile);
    if (!cached.empty()) id = (unsigned)strtoul(cached.c_str(), nullptr, 10);
    else { id = SteamSearch(g.name); WriteFileA(idFile, std::to_string(id)); }
  }
  if (g.cover.empty()) {
    std::wstring f = base + L"_cover.jpg";
    if (!Exists(f) && id) {
      std::string img = HttpGet(L"cdn.akamai.steamstatic.com", L"/steam/apps/" + std::to_wstring(id) + L"/library_600x900.jpg");
      if (IsImage(img)) WriteFileA(f, img);
    }
    if (Exists(f)) g.cover = f;
  }
  if (g.hero.empty() && id) {
    std::wstring f = base + L"_hero.jpg";
    if (!Exists(f)) {
      std::string img = HttpGet(L"cdn.akamai.steamstatic.com", L"/steam/apps/" + std::to_wstring(id) + L"/library_hero.jpg");
      if (IsImage(img)) WriteFileA(f, img);
    }
    if (Exists(f)) g.hero = f;
  }
  std::wstring df = base + L".txt";
  std::string d = ReadFileA(df);
  if (d.empty() && id) {
    std::string j = HttpGet(L"store.steampowered.com", L"/api/appdetails?filters=basic&l=spanish&cc=us&appids=" + std::to_wstring(id));
    d = JsonStr(j, "short_description");
    if (d.empty()) d = " ";
    WriteFileA(df, d);
  }
  g.desc = StripHtml(Utf8(d));
  while (!g.desc.empty() && iswspace(g.desc.back())) g.desc.pop_back();
  FetchShot(g, id);
}

// ---------------------------------------------------------------- videos de gameplay del carrusel
// Steam y la tienda de Microsoft publican sus videos solo como streaming (HLS de MP4 fragmentado).
// Se elige el video que mas parece gameplay (no el trailer cinematico) y se descarga una vez su
// variante liviana sin audio (segmento de inicio + ~24 s tomados desde un tercio del video, donde
// ya se esta jugando) a la cache como un .mp4 normal, que Media Foundation reproduce de forma nativa.
static const UINT WM_TRAILER_READY = WM_APP + 81, WM_VIDEO_FRAME = WM_APP + 82;

static std::wstring TrailerFile(const std::wstring& key) { return CacheDir() + L"\\" + key + L"_gameplay.mp4"; }

static bool SplitUrl(const std::string& url, std::wstring& host, std::wstring& path) {
  size_t s = url.find("://");
  if (s == std::string::npos) return false;
  size_t p = url.find('/', s + 3);
  if (p == std::string::npos) return false;
  host = Utf8(url.substr(s + 3, p - s - 3));
  path = Utf8(url.substr(p));
  return true;
}

static std::string Fetch(const std::string& url) {
  std::wstring host, path;
  return SplitUrl(url, host, path) ? HttpGet(host.c_str(), path) : "";
}

// URI de una lista HLS relativa a la lista que la contiene
static std::string ResolveUri(const std::string& list, const std::string& uri) {
  if (uri.find("://") != std::string::npos) return uri;
  std::string base = list.substr(0, list.find('?'));
  if (!uri.empty() && uri[0] == '/') return base.substr(0, base.find('/', base.find("://") + 3)) + uri;
  return base.substr(0, base.find_last_of('/') + 1) + uri;
}

static std::vector<std::string> Lines(const std::string& s) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t e = s.find('\n', pos);
    std::string line = s.substr(pos, (e == std::string::npos ? s.size() : e) - pos);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    out.push_back(line);
    if (e == std::string::npos) break;
    pos = e + 1;
  }
  return out;
}

// descarga ~24 s de un video HLS (lista maestra o de variante) a un .mp4
static bool DownloadClip(const std::string& master, const std::wstring& out) {
  std::string m = Fetch(master), variant = master;
  if (m.find("#EXTM3U") == std::string::npos) return false;
  if (m.find("#EXT-X-STREAM-INF") != std::string::npos) {
    // variante de video mas liviana con al menos 480 lineas (360 si no hay); sin audio
    std::string pick;
    int bestH = 1 << 30, bestLow = 0;
    std::string low;
    auto L = Lines(m);
    for (size_t i = 0; i + 1 < L.size(); i++) {
      if (L[i].rfind("#EXT-X-STREAM-INF", 0) != 0) continue;
      size_t r = L[i].find("RESOLUTION=");
      int w = 0, h = 0;
      if (r != std::string::npos) sscanf_s(L[i].c_str() + r + 11, "%dx%d", &w, &h);
      const std::string& uri = L[i + 1];
      if (uri.empty() || uri[0] == '#') continue;
      if (h >= 480 && h < bestH) { bestH = h; pick = uri; }
      if (h >= 360 && h < 480 && h > bestLow) { bestLow = h; low = uri; }
    }
    if (pick.empty()) pick = low;
    if (pick.empty()) return false;
    variant = ResolveUri(master, pick);
    m = Fetch(variant);
  }
  // segmento de inicio (EXT-X-MAP) y trozos con su duracion
  std::string init;
  std::vector<std::pair<std::string, float>> chunks;
  float dur = 0;
  for (auto& line : Lines(m)) {
    if (line.rfind("#EXT-X-MAP:", 0) == 0 && init.empty()) {
      size_t a = line.find("URI=\"");
      if (a != std::string::npos) init = line.substr(a + 5, line.find('"', a + 5) - a - 5);
    } else if (line.rfind("#EXTINF:", 0) == 0) dur = (float)atof(line.c_str() + 8);
    else if (!line.empty() && line[0] != '#') { chunks.push_back({ line, dur > 0 ? dur : 4.f }); dur = 0; }
  }
  if (init.empty() || chunks.size() < 3) return false;
  float total = 0;
  for (auto& c : chunks) total += c.second;
  // desde ~35% del video (los primeros segundos suelen ser logos y escenas de historia)
  size_t first = 1;
  if (total > 60) { float t = 0; while (first < chunks.size() - 1 && t < total * .35f) t += chunks[first++].second; }
  std::wstring part = out + L".part";
  HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  // cada trozo se escribe directo al archivo (sin juntar varios MB en memoria)
  auto put = [&](const std::string& s) { DWORD w = 0; return !s.empty() && WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size(); };
  bool good = put(Fetch(ResolveUri(variant, init)));
  float got = 0;
  for (size_t i = first; good && i < chunks.size() && got < 24; i++) { good = put(Fetch(ResolveUri(variant, chunks[i].first))); got += chunks[i].second; }
  LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
  CloseHandle(f);
  bool ok = good && sz.QuadPart > 100000 && MoveFileExW(part.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!ok) DeleteFileW(part.c_str());
  return ok;
}

// cuanto parece gameplay un video por su titulo (los traileres cinematicos quedan al final)
static int GameplayScore(std::string t) {
  for (auto& c : t) c = (char)tolower((unsigned char)c);
  int s = 0;
  if (t.find("gameplay") != std::string::npos || t.find("game play") != std::string::npos) s += 10;
  for (auto w : { "walkthrough", "demo", "first look", "features", "overview", "showcase", "jugabilidad" }) if (t.find(w) != std::string::npos) s += 4;
  for (auto w : { "cinematic", "cinematica", "cgi", "announce", "teaser", "story", "opening", "intro", "live action", "accolades", "pre-order", "preorder" }) if (t.find(w) != std::string::npos) s -= 5;
  if (t.find("trailer") != std::string::npos || t.find("tráiler") != std::string::npos) s -= 1;
  return s;
}

// Steam: entre los videos de la ficha, el que mas parece gameplay
static std::string SteamClipUrl(unsigned steamId) {
  std::string j = HttpGet(L"store.steampowered.com", L"/api/appdetails?cc=us&appids=" + std::to_wstring(steamId));
  std::string best;
  int bestScore = -1000;
  for (size_t p = 0; (p = j.find("\"hls_h264\"", p)) != std::string::npos; p += 10) {
    std::string url = JsonStr(j, "hls_h264", p);
    size_t n = j.rfind("\"name\"", p);
    int score = GameplayScore(n == std::string::npos ? "" : JsonStr(j, "name", n));
    if (!url.empty() && score > bestScore) { bestScore = score; best = url; }
  }
  return best;
}

// Tienda de Microsoft: id del producto (en cache) y su video que mas parece gameplay
static std::string MsProductId(const std::wstring& key, const std::wstring& name) {
  std::wstring f = CacheDir() + L"\\" + key + L".msid";
  std::string id = ReadFileA(f);
  if (!id.empty()) return id == "0" ? "" : id;
  std::wstring q;
  for (unsigned char c : ToUtf8(name)) { if (isalnum(c)) q += (wchar_t)c; else { wchar_t b[4]; swprintf_s(b, L"%%%02X", c); q += b; } }
  std::string s = HttpGet(L"displaycatalog.mp.microsoft.com", L"/v7.0/productFamilies/autosuggest?market=US&languages=es-es&productFamilyNames=Games&query=" + q);
  if (s.empty()) return "";  // sin conexion: no se anota como fallido
  for (size_t p = 0; (p = s.find("\"ProductId\"", p)) != std::string::npos; p += 11) {
    std::string pid = JsonStr(s, "ProductId", p);
    size_t t = s.rfind("\"Title\"", p);
    if (SimilarName(name, Utf8(JsonStr(s, "Title", t == std::string::npos ? p : t)))) { id = pid; break; }
  }
  WriteFileA(f, id.empty() ? "0" : id);
  return id;
}

// captura del juego para el fondo del carrusel (la caratula y el banner suelen ser el mismo arte):
// de Steam o de la tienda de Microsoft, la segunda captura (la primera suele ser un logo o menu)
static void FetchShot(Game& g, unsigned steamId) {
  std::wstring base = CacheDir() + L"\\" + g.key, f = base + L"_shot.jpg", marker = base + L".shot";
  if (!Exists(f) && !Exists(marker)) {
    bool ok = false, online = false;
    if (steamId) {
      std::string j = HttpGet(L"store.steampowered.com", L"/api/appdetails?filters=screenshots&cc=us&appids=" + std::to_wstring(steamId));
      online = !j.empty();
      std::vector<std::string> urls;
      for (size_t p = 0; (p = j.find("\"path_full\"", p)) != std::string::npos && urls.size() < 3; p += 11) urls.push_back(JsonStr(j, "path_full", p));
      for (size_t i = urls.size() > 1 ? 1 : 0; !ok && i < urls.size(); i++) {
        std::string img = Fetch(urls[i]);
        if (IsImage(img)) { WriteFileA(f, img); ok = true; }
      }
    }
    if (!ok) {
      std::string id = MsProductId(g.key, g.name);
      if (!id.empty()) {
        std::string j = HttpGet(L"displaycatalog.mp.microsoft.com", L"/v7.0/products?market=US&languages=es-es&bigIds=" + Utf8(id));
        online = online || !j.empty();
        std::vector<std::string> uris;
        for (size_t p = 0; (p = j.find("\"ImagePurpose\":\"Screenshot\"", p)) != std::string::npos && uris.size() < 3; p += 10) uris.push_back(JsonStr(j, "Uri", p));
        for (size_t i = uris.size() > 1 ? 1 : 0; !ok && i < uris.size(); i++) ok = StoreImage(uris[i], L"?w=1600&h=900&q=85", f);
      }
    }
    if (!ok && online) WriteFileA(marker, "-");  // sin conexion no se anota como fallido
  }
  if (Exists(f)) g.shot = f;
}

// reputacion y galeria para la ficha (se piden al abrirla, una vez por juego):
// {key}.rep = "S|positivas|negativas" (reseñas de Steam, via SteamSpy) o "M|promedio|votos" (tienda de
// Microsoft) o "-"; {key}_g0..5.jpg = capturas de gameplay; {key}.gal = cuantas hay
static const UINT WM_FICHA_DATA = WM_APP + 84;

static void FetchFichaData(HWND notify, std::wstring key, std::wstring name, unsigned steamId) {
  std::wstring base = CacheDir() + L"\\" + key;
  bool changed = false;
  if (!Exists(base + L".rep")) {
    std::string rep;
    if (steamId) {
      std::string j = HttpGet(L"steamspy.com", L"/api.php?request=appdetails&appid=" + std::to_wstring(steamId));
      size_t p = j.find("\"positive\":"), n = j.find("\"negative\":");
      if (p != std::string::npos && n != std::string::npos) {
        unsigned long long pos = strtoull(j.c_str() + p + 11, nullptr, 10), neg = strtoull(j.c_str() + n + 11, nullptr, 10);
        if (pos + neg > 0) rep = "S|" + std::to_string(pos) + "|" + std::to_string(neg);
      }
    }
    if (rep.empty()) {
      std::string id = MsProductId(key, name);
      if (!id.empty()) {
        std::string j = HttpGet(L"displaycatalog.mp.microsoft.com", L"/v7.0/products?market=US&languages=es-es&bigIds=" + Utf8(id));
        size_t p = j.find("\"AggregateTimeSpan\":\"AllTime\"");
        if (p != std::string::npos) {
          size_t a = j.find("\"AverageRating\":", p), c = j.find("\"RatingCount\":", p);
          if (a != std::string::npos && c != std::string::npos) {
            double avg = atof(j.c_str() + a + 16);
            unsigned long long cnt = strtoull(j.c_str() + c + 14, nullptr, 10);
            if (cnt) { char b[64]; sprintf_s(b, "M|%.1f|%llu", avg, cnt); rep = b; }
          }
        }
      }
    }
    if (!rep.empty() || !HttpGet(L"store.steampowered.com", L"/robots.txt").empty()) { WriteFileA(base + L".rep", rep.empty() ? "-" : rep); changed = true; }
  }
  if (!Exists(base + L".gal")) {
    int n = 0;
    if (steamId) {
      std::string j = HttpGet(L"store.steampowered.com", L"/api/appdetails?filters=screenshots&cc=us&appids=" + std::to_wstring(steamId));
      for (size_t p = 0; n < 6 && (p = j.find("\"path_thumbnail\"", p)) != std::string::npos; p += 16) {
        std::string img = Fetch(JsonStr(j, "path_thumbnail", p));
        if (IsImage(img)) WriteFileA(base + L"_g" + std::to_wstring(n++) + L".jpg", img);
      }
    }
    if (!n) {
      std::string id = MsProductId(key, name);
      if (!id.empty()) {
        std::string j = HttpGet(L"displaycatalog.mp.microsoft.com", L"/v7.0/products?market=US&languages=es-es&bigIds=" + Utf8(id));
        for (size_t p = 0; n < 6 && (p = j.find("\"ImagePurpose\":\"Screenshot\"", p)) != std::string::npos; p += 10)
          if (StoreImage(JsonStr(j, "Uri", p), L"?w=640&h=360&q=85", base + L"_g" + std::to_wstring(n) + L".jpg")) n++;
      }
    }
    if (n || !HttpGet(L"store.steampowered.com", L"/robots.txt").empty()) { WriteFileA(base + L".gal", std::to_string(n)); changed = true; }
  }
  if (changed && IsWindow(notify)) PostMessageW(notify, WM_FICHA_DATA, 0, (LPARAM) new std::wstring(key));
}

static std::string MsClipUrl(const std::wstring& key, const std::wstring& name) {
  std::string id = MsProductId(key, name);
  if (id.empty()) return "";
  std::string j = HttpGet(L"displaycatalog.mp.microsoft.com", L"/v7.0/products?market=US&languages=es-es&bigIds=" + Utf8(id));
  size_t v = j.find("\"CMSVideos\"");
  if (v == std::string::npos) return "";
  std::string best;
  int bestScore = -1000;
  for (size_t p = v; (p = j.find("\"HLS\"", p)) != std::string::npos; p += 5) {
    std::string url = JsonStr(j, "HLS", p);
    size_t t = j.rfind("\"Title\"", p);
    int score = GameplayScore(t == std::string::npos || t < v ? "" : JsonStr(j, "Title", t));
    if (!url.empty() && score > bestScore) { bestScore = score; best = url; }
  }
  return best;
}

static bool DownloadTrailer(const std::wstring& key, unsigned steamId, const std::wstring& name) {
  std::wstring out = TrailerFile(key), marker = CacheDir() + L"\\" + key + L".gameplay";
  if (Exists(out)) return true;
  if (Exists(marker)) return false;  // ya se intento: este juego no tiene video
  DeleteFileW((CacheDir() + L"\\" + key + L"_trailer.mp4").c_str());  // formato anterior (trailer)
  DeleteFileW((CacheDir() + L"\\" + key + L".trailer").c_str());
  bool online = !HttpGet(L"store.steampowered.com", L"/robots.txt").empty();
  if (!online) return false;  // sin internet no se marca como fallido
  bool ok = false;
  if (steamId) { std::string u = SteamClipUrl(steamId); ok = !u.empty() && DownloadClip(u, out); }
  if (!ok) { std::string u = MsClipUrl(key, name); ok = !u.empty() && DownloadClip(u, out); }
  if (!ok) WriteFileA(marker, "-");
  return ok;
}

// un solo hilo descarga los videos pendientes, de a uno
struct ClipJob { std::wstring key, name; unsigned steamId; };
static std::mutex s_trMx;
static std::deque<ClipJob> s_trJobs;
static std::set<std::wstring> s_trQueued;
static bool s_trBusy = false;

static void RequestTrailer(HWND notify, const std::wstring& key, unsigned steamId, const std::wstring& name, bool urgent = false) {
  std::lock_guard<std::mutex> lk(s_trMx);
  if (s_trQueued.count(key)) {
    if (urgent)  // el juego elegido pasa al frente de la cola
      for (auto it = s_trJobs.begin(); it != s_trJobs.end(); ++it)
        if (it->key == key) { ClipJob j = *it; s_trJobs.erase(it); s_trJobs.push_front(j); break; }
    return;
  }
  s_trQueued.insert(key);
  if (urgent) s_trJobs.push_front({ key, name, steamId }); else s_trJobs.push_back({ key, name, steamId });
  if (s_trBusy) return;
  s_trBusy = true;
  std::thread([notify] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
      ClipJob job;
      {
        std::lock_guard<std::mutex> lk(s_trMx);
        if (s_trJobs.empty()) { s_trBusy = false; break; }
        job = s_trJobs.front();
        s_trJobs.pop_front();
      }
      if (DownloadTrailer(job.key, job.steamId, job.name) && IsWindow(notify))
        PostMessageW(notify, WM_TRAILER_READY, 0, (LPARAM)new std::wstring(job.key));
    }
    CoUninitialize();
  }).detach();
}

// Reproductor: decodifica en su hilo (Media Foundation, por software), respeta los tiempos del
// video y deja el ultimo cuadro listo; la ventana solo copia ese cuadro y repinta el recuadro.
class TrailerPlayer {
public:
  std::wstring key;
  std::atomic<bool> stop{ false }, paused{ false };
  std::mutex mx;
  std::vector<BYTE> frame;
  UINT fw = 0, fh = 0;
  bool fresh = false;
  std::thread th;

  void Start(const std::wstring& k, const std::wstring& file, HWND notify) {
    key = k;
    th = std::thread([this, file, notify] { Run(file, notify); });
  }
  ~TrailerPlayer() { stop = true; if (th.joinable()) th.join(); }

  void Run(std::wstring file, HWND notify) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFAttributes* at = nullptr;
    MFCreateAttributes(&at, 1);
    at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    IMFSourceReader* r = nullptr;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(file.c_str(), at, &r))) {
      r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
      r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
      IMFMediaType* mt = nullptr;
      MFCreateMediaType(&mt);
      mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
      HRESULT hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt);
      mt->Release();
      UINT32 w = 0, h = 0;
      IMFMediaType* cur = nullptr;
      if (SUCCEEDED(hr) && SUCCEEDED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
        cur->Release();
      }
      DWORD start = GetTickCount();
      LONGLONG base = -1;
      std::vector<BYTE> buf((size_t)w * h * 4);
      while (!stop && w && h) {
        if (paused) {  // en pausa no se decodifica nada
          DWORD p0 = GetTickCount();
          while (paused && !stop) Sleep(60);
          start += GetTickCount() - p0;
          continue;
        }
        DWORD flags = 0; LONGLONG ts = 0; IMFSample* s = nullptr;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
          // el decodificador alinea el ancho (p. ej. 854 -> 864): se toma el area visible real
          IMFMediaType* nt = nullptr;
          if (SUCCEEDED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &nt))) {
            UINT32 nw = 0, nh = 0;
            MFGetAttributeSize(nt, MF_MT_FRAME_SIZE, &nw, &nh);
            MFVideoArea ap{};
            if (SUCCEEDED(nt->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&ap, sizeof ap, nullptr)) && ap.Area.cx > 0 && ap.Area.cy > 0) {
              nw = std::min<UINT32>(nw, (UINT32)ap.Area.cx); nh = std::min<UINT32>(nh, (UINT32)ap.Area.cy);
            }
            if (nw && nh) { w = std::min(w ? w : nw, nw); h = std::min(h ? h : nh, nh); buf.assign((size_t)w * h * 4, 0); }
            nt->Release();
          }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {  // bucle
          PROPVARIANT pv; PropVariantInit(&pv); pv.vt = VT_I8; pv.hVal.QuadPart = 0;
          r->SetCurrentPosition(GUID_NULL, pv);
          base = -1; start = GetTickCount();
          if (s) s->Release();
          continue;
        }
        if (!s) continue;
        if (base < 0) base = ts;
        LONGLONG due = (ts - base) / 10000;
        while (!stop && !paused && (LONGLONG)(GetTickCount() - start) < due) Sleep(4);
        IMFMediaBuffer* mb = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&mb))) {
          IMF2DBuffer* b2 = nullptr;
          BYTE* scan = nullptr; LONG pitch = 0;
          BYTE* lin = nullptr; DWORD len = 0;
          if (SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan, &pitch))) {
            for (UINT y = 0; y < h; y++) memcpy(&buf[(size_t)y * w * 4], scan + (LONG)y * pitch, (size_t)w * 4);
            b2->Unlock2D();
          } else if (SUCCEEDED(mb->Lock(&lin, nullptr, &len))) {
            // bufer contiguo: el decodificador alinea el ancho (854 -> 864) y/o el alto (540 -> 544).
            // El paso de fila es el ancho alineado a 1, 16, 32 o 64 px que reparte el bufer en ~h filas.
            size_t row = (size_t)w * 4, step = row;
            for (UINT al : { 1u, 16u, 32u, 64u }) {
              size_t s = (size_t)((w + al - 1) / al * al) * 4, rows = s ? len / s : 0;
              if (s && len % s == 0 && rows >= h && rows < (size_t)h + 64) { step = s; break; }
            }
            for (UINT y = 0; y < h && (size_t)y * step + row <= len; y++) memcpy(&buf[(size_t)y * row], lin + (size_t)y * step, row);
            mb->Unlock();
          }
          if (b2) b2->Release();
          mb->Release();
          {
            std::lock_guard<std::mutex> lk(mx);
            frame.swap(buf);
            if (buf.size() != frame.size()) buf.resize(frame.size());
            fw = w; fh = h; fresh = true;
          }
          PostMessageW(notify, WM_VIDEO_FRAME, 0, 0);
        }
        s->Release();
      }
      r->Release();
    }
    at->Release();
    MFShutdown();
    CoUninitialize();
  }
};

// ---------------------------------------------------------------- ventana
static const wchar_t* kGameClass = L"ZyntraWindow";
static const wchar_t* kAumid = L"Zargch.Zyntra";  // AppUserModelID: el mismo que ponen los accesos del instalador
static const UINT WM_GAMES_LIST = WM_APP + 80;
struct GBtn { D2D1_RECT_F r; std::function<void()> act; };

// paleta "futurista premium": grafito + naranja electrico como color de marca
static D2D1_COLOR_F Hex(unsigned rgb, float a = 1) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a); }
static D2D1_COLOR_F MixC(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) { return D2D1::ColorF(Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)); }
namespace Z {
static const D2D1_COLOR_F bg = Hex(0x101114), panel = Hex(0x17191E), elev = Hex(0x202329), border = Hex(0x30343B), borderHi = Hex(0x4A4F59);
static const D2D1_COLOR_F text = Hex(0xF4F5F7), text2 = Hex(0xA1A6B0), text3 = Hex(0x6E737D);
static const D2D1_COLOR_F brand = Hex(0xFF642E), brandHi = Hex(0xFF854F), brandLo = Hex(0xE04F1C);
static const D2D1_COLOR_F ok = Hex(0x3DBE6B), err = Hex(0xFF4D4F);
}

// interpolacion con salida suave; solo exige repintar mientras dura
struct Tween {
  float from = 0, to = 0, dur = 150;
  DWORD t0 = 0;
  float Get() const {
    if (!t0) return to;
    float t = Clamp((GetTickCount() - t0) / dur, 0, 1);
    return Lerp(from, to, 1 - (1 - t) * (1 - t) * (1 - t));
  }
  bool Active() const { return t0 && GetTickCount() - t0 < dur; }
  void Go(float target, float d = 150) { from = Get(); to = target; dur = d; t0 = GetTickCount(); }
  void Set(float v) { from = to = v; t0 = 0; }
};

enum LaunchState { LS_NONE, LS_START, LS_RUN, LS_ERR };

class GameMode {
public:
  HWND hwnd = nullptr;
  int w = 0, h = 0;
  std::vector<Game> games, view;
  std::map<std::wstring, ID2D1Bitmap*> imgs;
  int decodesLeft = 0;
  int sel = -1, hover = -1, hoverPrev = -1;
  float scroll = 0, scrollTarget = 0;
  std::wstring search, filter = L"Todos";
  bool recent = true, hoverTL = false, caret = true, searchActive = false;
  std::vector<GBtn> btns;
  int hotBtn = -1, btnPrev = -1, pressBtn = -1;
  D2D1_RECT_F searchRect{}, playRect{};
  std::atomic<bool> scanning{ false };
  bool refreshing = false, pendingRescan = false;  // boton Actualizar
  bool firstScan = true;
  bool panelOpen = true;
  Tween hoverIn, hoverOut, btnIn, btnOut, selAnim, panelAnim;
  // estado del boton Jugar (se comprueba si el juego realmente se abrio)
  LaunchState ls = LS_NONE;
  std::wstring lsKey;
  std::vector<std::wstring> lsPaths;
  DWORD lsT0 = 0;
  // texto de la descripcion: el layout se reutiliza mientras no cambie el juego
  IDWriteTextLayout* descLayout = nullptr;
  std::wstring descKey;
  D2D1_RECT_F paintClip{};
  // carrusel destacado (juegos jugados mas recientemente) con trailer
  std::vector<std::wstring> slides;
  int slide = 0, prevSlide = -1;
  std::wstring featured;  // juego elegido que se muestra en el carrusel
  Tween slideFade;
  bool bannerHover = false;
  bool trailers = true;  // [Games] trailers en config.ini
  std::unique_ptr<TrailerPlayer> player;
  ID2D1Bitmap* videoBmp = nullptr;
  D2D1_RECT_F videoRect{};
  // ficha a pantalla completa: caratula grande con el video del juego desenfocado detras
  std::wstring ficha;              // clave del juego abierto en la ficha (vacio = cerrada)
  Tween fichaPage;                 // 0 = caratula y video, 1 = reputacion y galeria (se pasa deslizando)
  std::string fichaRep;            // contenido de {key}.rep
  std::vector<std::wstring> fichaGal;  // capturas de la galeria
  std::wstring fichaBig;           // captura ampliada
  D2D1_RECT_F fichaVideoRect{};
  D2D1_RECT_F pressRect{};         // boton pulsado (la lista se rehace en cada repintado)
  static constexpr float TB = 68, GAP = 22;
  float Wd() const { return Dip(w); }
  float Hd() const { return Dip(h); }
  // todo crece con la ventana (maximizada o a pantalla completa se aprovecha el espacio)
  float Rp() const { return Clamp(Wd() * .24f, 384, 480); }                // ancho del panel de ficha
  float PanelX() const { return Wd() - Rp() * panelAnim.Get(); }           // borde izquierdo animado del panel
  float LayoutRight() const { return Wd() - (panelOpen ? Rp() : 0); }     // la cuadricula usa el estado final
  float GridL() const { return 28; }
  float GridR() const { return LayoutRight() - 20; }
  float GridW() const { return GridR() - GridL(); }
  float TargetCw() const { return Clamp(GridW() / 8.f, 156, 228); }        // ancho deseado de una caratula
  int Cols() const { return std::max(1, (int)((GridW() + GAP) / (TargetCw() + GAP))); }
  float Cw() const { int c = Cols(); return std::min(260.f, (GridW() - (c - 1) * GAP) / c); }  // llena la fila
  float Ch() const { return Cw() * 1.5f; }
  // resolucion de decodificado en pasos de 40 DIPs: redimensionar la ventana no llena la cache con tamaños sueltos
  int CoverPx(float w) const { return Px(ceilf(w / 40) * 40); }  // tamaño real en pantalla (antes el doble: 4 veces la memoria)
  float CellH() const { return Ch() + 44; }
  float Bh() const { return Clamp(std::min(GridW() * .32f, (Hd() - TB) * .55f), 300, 560); }  // alto del carrusel (con margen inferior)
  bool CarouselOn() const { return !slides.empty() && filter == L"Todos" && search.empty(); }
  float BannerH() const { return CarouselOn() ? Bh() : 0.f; }
  D2D1_RECT_F BannerRect() const { float y = TB + 20 - scroll; return { GridL(), y, GridR(), y + Bh() - 26 }; }
  const Game* SlideGame(int i) const {
    if (i < 0 || i >= (int)slides.size()) return nullptr;
    for (auto& g : games) if (g.key == slides[i]) return &g;
    return nullptr;
  }

  // ------------------------------------------------------------ datos
  void Rescan() {
    if (scanning.exchange(true)) return;
    HWND hw = hwnd;
    std::thread([hw] {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      auto* list = new std::vector<Game>(ScanAll());
      PostMessageW(hw, WM_GAMES_LIST, 0, (LPARAM)new std::vector<Game>(*list));  // lista rapida
      for (auto& g : *list) Enrich(g);                                             // caratulas y textos
      PostMessageW(hw, WM_GAMES_LIST, 1, (LPARAM)list);
      CoUninitialize();
    }).detach();
  }

  // boton Actualizar: vuelve a buscar en internet lo que falto (caratula, banner, informacion, video)
  void Refresh() {
    for (auto& g : games) ForgetMisses(g.key, false);
    { std::lock_guard<std::mutex> lk(s_trMx); s_trQueued.clear(); }
    refreshing = true;
    if (scanning) pendingRescan = true;  // hay un escaneo en curso: se repite al terminar
    else Rescan();
    InvalidateDip({ 0, 0, Wd(), TB }, 0);
  }

  static bool SameGames(const std::vector<Game>& a, const std::vector<Game>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
      const Game &x = a[i], &y = b[i];
      if (x.key != y.key || x.name != y.name || x.lastPlayed != y.lastPlayed || x.cover != y.cover || x.hero != y.hero ||
          x.shot != y.shot || x.desc != y.desc || x.platform != y.platform || x.launch != y.launch) return false;
    }
    return true;
  }

  void SetGames(std::vector<Game>&& n, bool done) {
    std::wstring selKey = sel >= 0 && sel < (int)view.size() ? view[sel].key : L"";
    // conserva caratulas/descripciones ya conocidas mientras llega el enriquecimiento
    if (!done)
      for (auto& g : n)
        for (auto& o : games)
          if (o.key == g.key) { if (g.cover.empty()) g.cover = o.cover; if (g.hero.empty()) g.hero = o.hero; if (g.desc.empty()) g.desc = o.desc; if (g.shot.empty()) g.shot = o.shot; }
    if (done) scanning = false;
    // el escaneo periodico casi siempre trae la misma lista: sin cambios no se recalcula ni se repinta nada
    if (!games.empty() && SameGames(games, n)) { if (done) firstScan = false; return; }
    games = std::move(n);
    Filter();
    sel = -1;
    for (int i = 0; i < (int)view.size(); i++) if (view[i].key == selKey) sel = i;
    if (sel < 0 && !view.empty() && firstScan) { sel = 0; selAnim.Set(0); selAnim.Go(1, 220); Animate(); }
    if (done) firstScan = false;
    DropDescLayout();
    BuildSlides();
    Invalidate();
  }

  // los 5 juegos jugados mas recientemente que tengan alguna imagen
  void BuildSlides() {
    std::vector<const Game*> v;
    for (auto& g : games) if (!g.cover.empty() || !g.hero.empty()) v.push_back(&g);
    std::stable_sort(v.begin(), v.end(), [](const Game* a, const Game* b) { return a->lastPlayed > b->lastPlayed; });
    std::wstring cur = slide < (int)slides.size() ? slides[slide] : L"";
    slides.clear();
    for (size_t i = 0; i < v.size() && i < 5; i++) slides.push_back(v[i]->key);
    // el juego elegido en la biblioteca se queda en el carrusel aunque no sea de los recientes
    if (!featured.empty() && std::find(slides.begin(), slides.end(), featured) == slides.end() &&
        std::any_of(games.begin(), games.end(), [&](const Game& g) { return g.key == featured; }))
      slides.insert(slides.begin(), featured);
    for (auto& k : slides) RequestClip(k, k == featured);  // todos los slides con video
    slide = 0;
    for (int i = 0; i < (int)slides.size(); i++) if (slides[i] == cur) slide = i;
    ClampScroll();
    UpdateVideo();
  }

  void RequestClip(const std::wstring& key, bool urgent) {
    for (auto& g : games)
      if (g.key == key) {
        unsigned id = g.steamId;
        if (!id) id = (unsigned)strtoul(ReadFileA(CacheDir() + L"\\" + key + L".id").c_str(), nullptr, 10);
        RequestTrailer(hwnd, key, id, g.name, urgent);
        return;
      }
  }

  // al elegir un juego, el carrusel lo muestra con su video e informacion
  void Feature(const std::wstring& key) {
    if (key.empty() || !CarouselOn()) return;
    featured = key;
    auto it = std::find(slides.begin(), slides.end(), key);
    if (it == slides.end()) {
      slides.insert(slides.begin(), key);
      if (slide >= 0) slide++;  // la diapositiva actual se desplazo un lugar
      it = slides.begin();
    }
    RequestClip(key, true);
    GoSlide((int)(it - slides.begin()));
    SetTimer(hwnd, 8, 45000, nullptr);  // se queda en el juego elegido un rato antes de seguir rotando
  }

  void GoSlide(int i) {
    if (slides.empty()) return;
    i = (i + (int)slides.size()) % (int)slides.size();
    if (i == slide) return;
    prevSlide = slide;
    slide = i;
    slideFade.Set(0);
    slideFade.Go(1, 380);
    SetTimer(hwnd, 8, 12000, nullptr);  // reinicia el tiempo de la diapositiva
    UpdateVideo();
    InvalidateDip(BannerRect(), 4);
    Animate();
  }

  // el video solo corre si se ve: ventana al frente, sin minimizar y con el carrusel en pantalla
  const Game* FindGame(const std::wstring& key) const {
    for (auto& g : games) if (g.key == key) return &g;
    return nullptr;
  }

  void UpdateVideo() {
    // con la ficha abierta se reproduce el video de ese juego (de fondo), si no el del carrusel
    const Game* g = !ficha.empty() ? FindGame(ficha) : SlideGame(slide);
    bool shown = !ficha.empty() || (trailers && CarouselOn() && scroll < Bh() - 40);
    bool visible = g && shown && !IsIconic(hwnd) && GetForegroundWindow() == hwnd;
    std::wstring file = g ? TrailerFile(g->key) : L"";
    if (player && (!g || player->key != g->key)) {  // cambio de juego: se libera el reproductor anterior
      player.reset();
      if (videoBmp) { videoBmp->Release(); videoBmp = nullptr; }
    }
    if (visible && !player && Exists(file)) {
      player = std::make_unique<TrailerPlayer>();
      player->Start(g->key, file, hwnd);
    }
    if (player) player->paused = !visible;
    if (CarouselOn()) SetTimer(hwnd, 8, 12000, nullptr); else KillTimer(hwnd, 8);
  }

  void OnVideoFrame() {
    if (!player) return;
    std::lock_guard<std::mutex> lk(player->mx);
    if (!player->fresh || player->frame.empty()) return;
    player->fresh = false;
    D2D1_SIZE_U sz = D2D1::SizeU(player->fw, player->fh);
    if (videoBmp && (videoBmp->GetPixelSize().width != sz.width || videoBmp->GetPixelSize().height != sz.height)) { videoBmp->Release(); videoBmp = nullptr; }
    if (!videoBmp) {
      auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
      g_rt->CreateBitmap(sz, player->frame.data(), sz.width * 4, bp, &videoBmp);
    } else videoBmp->CopyFromMemory(nullptr, player->frame.data(), sz.width * 4);
    InvalidateDip(ficha.empty() ? videoRect : fichaVideoRect, 1);  // solo el recuadro del video
  }

  void Filter() {
    view.clear();
    for (auto& g : games)
      if ((filter == L"Todos" || g.platform == filter) && (search.empty() || IContains(g.name, search))) view.push_back(g);
    std::sort(view.begin(), view.end(), [this](const Game& a, const Game& b) {
      if (recent && a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
      return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, a.name.c_str(), -1, b.name.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    if (sel >= (int)view.size()) sel = (int)view.size() - 1;
    hover = hoverPrev = -1;
    ClampScroll();
    UpdateVideo();  // con filtro o busqueda el carrusel se oculta y el video se pausa
  }

  void Select(int i) {
    if (i == sel) return;
    if (sel >= 0) InvalidateCard(sel);
    sel = i;
    if (sel >= 0 && sel < (int)view.size()) Feature(view[sel].key);
    DropDescLayout();
    selAnim.Set(0);
    selAnim.Go(1, 220);
    InvalidateCard(sel);
    InvalidatePanel();
    Animate();
  }

  // decodifica cada caratula una vez, al tamaño de pantalla; la cache tiene un tope
  ID2D1Bitmap* Img(const std::wstring& path, int maxW) {
    if (path.empty()) return nullptr;
    std::wstring key = path + L"|" + std::to_wstring(maxW);
    auto it = imgs.find(key);
    if (it != imgs.end()) return it->second;
    if (decodesLeft <= 0) { SetTimer(hwnd, 4, 30, nullptr); return nullptr; }  // decodifica de a poco
    decodesLeft--;
    if (imgs.size() >= 96) ReleaseImages();
    ID2D1Bitmap* bmp = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))) {
      UINT iw, ih; fr->GetSize(&iw, &ih);
      UINT tw = std::min<UINT>(iw, maxW), th = std::max<UINT>(1, (UINT)((double)ih * tw / iw));
      IWICBitmapScaler* sc = nullptr; g_wic->CreateBitmapScaler(&sc);
      sc->Initialize(fr, tw, th, WICBitmapInterpolationModeHighQualityCubic);
      IWICFormatConverter* cv = nullptr; g_wic->CreateFormatConverter(&cv);
      cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
      g_rt->CreateBitmapFromWicBitmap(cv, nullptr, &bmp);
      cv->Release(); sc->Release();
    }
    if (fr) fr->Release();
    if (dec) dec->Release();
    imgs[key] = bmp;
    return bmp;
  }

  void ReleaseImages() {
    for (auto& kv : imgs) if (kv.second) kv.second->Release();
    imgs.clear();
  }

  void DropDescLayout() {
    if (descLayout) { descLayout->Release(); descLayout = nullptr; }
    descKey.clear();
  }

  // ------------------------------------------------------------ lanzar y comprobar
  static bool AnyRunning(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return false;
    for (auto& r : EnumRunning())
      for (auto& p : paths)
        if (!p.empty() && r.exe.rfind(p + L"\\", 0) == 0) return true;
    return false;
  }

  void Launch(const Game& g) {
    // mismo mecanismo de siempre (URI de la tienda, AppsFolder o ejecutable), pero mirando el resultado
    HINSTANCE r = ShellExecuteW(hwnd, nullptr, g.launch.c_str(), g.args.empty() ? nullptr : g.args.c_str(), nullptr, SW_SHOWNORMAL);
    lsKey = g.key;
    lsPaths = g.paths;
    lsT0 = GetTickCount();
    if ((INT_PTR)r <= 32) {
      ls = LS_ERR;
      SetTimer(hwnd, 5, 4000, nullptr);
    } else {
      ls = AnyRunning(lsPaths) ? LS_RUN : LS_START;
      SetTimer(hwnd, 5, 1500, nullptr);
      if (ls == LS_START) SetTimer(hwnd, 7, 450, nullptr);
    }
    InvalidatePanel();
  }

  void LaunchTick() {
    if (ls == LS_ERR) { ls = LS_NONE; KillTimer(hwnd, 5); }
    else if (ls == LS_START) {
      if (AnyRunning(lsPaths)) { ls = LS_RUN; KillTimer(hwnd, 7); SetTimer(hwnd, 5, 3000, nullptr); }
      else if (GetTickCount() - lsT0 > 45000) { ls = LS_NONE; KillTimer(hwnd, 5); KillTimer(hwnd, 7); }  // no se pudo confirmar: sin falso error
    } else if (ls == LS_RUN) {
      if (!AnyRunning(lsPaths)) { ls = LS_NONE; KillTimer(hwnd, 5); }
    } else KillTimer(hwnd, 5);
    InvalidatePanel();
  }

  void Hide(const Game& g) {
    auto hidden = IniList(L"hidden");
    hidden.push_back(g.key);
    IniSaveList(L"hidden", hidden);
    games.erase(std::remove_if(games.begin(), games.end(), [&](const Game& x) { return x.key == g.key; }), games.end());
    Filter(); Invalidate();
  }

  void AddGame() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"Juegos y accesos directos", L"*.exe;*.lnk;*.url" } };
    d->SetFileTypes(1, f);
    d->SetTitle(L"Añadir un juego a Zyntra");
    DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_NODEREFERENCELINKS);
    if (SUCCEEDED(d->Show(hwnd))) {
      IShellItem* it = nullptr;
      if (SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
          auto custom = IniList(L"custom");
          custom.push_back(p);
          IniSaveList(L"custom", custom);
          CoTaskMemFree(p);
          Rescan();
        }
        it->Release();
      }
    }
    d->Release();
  }

  // carátula propia: se copia a la cache como {clave}_custom.jpg y tiene prioridad sobre las demas
  void ChooseCover(const Game& g) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"Imágenes", L"*.jpg;*.jpeg;*.png;*.webp;*.bmp" } };
    d->SetFileTypes(1, f);
    d->SetTitle((L"Carátula para " + g.name).c_str());
    if (SUCCEEDED(d->Show(hwnd))) {
      IShellItem* it = nullptr;
      if (SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
          std::wstring dst = CacheDir() + L"\\" + g.key + L"_custom.jpg";
          if (CopyFileW(p, dst.c_str(), FALSE)) {
            for (auto it2 = imgs.begin(); it2 != imgs.end();)  // la ruta es la misma: hay que olvidar la version anterior
              if (it2->first.rfind(dst + L"|", 0) == 0) { if (it2->second) it2->second->Release(); it2 = imgs.erase(it2); } else ++it2;
            for (auto* list : { &games, &view }) for (auto& x : *list) if (x.key == g.key) x.cover = dst;
          }
          CoTaskMemFree(p);
        }
        it->Release();
      }
    }
    d->Release();
    Invalidate();
  }

  // video propio (para juegos sin video en Steam ni en la tienda de Microsoft): se copia a la cache
  void ChooseVideo(const Game& g) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { L"Videos MP4", L"*.mp4;*.m4v;*.mov" } };
    d->SetFileTypes(1, f);
    d->SetTitle((L"Video de gameplay para " + g.name).c_str());
    if (SUCCEEDED(d->Show(hwnd))) {
      IShellItem* it = nullptr;
      if (SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
          if (player && player->key == g.key) { player.reset(); if (videoBmp) { videoBmp->Release(); videoBmp = nullptr; } }
          if (CopyFileW(p, TrailerFile(g.key).c_str(), FALSE)) {
            DeleteFileW((CacheDir() + L"\\" + g.key + L".gameplay").c_str());
            Feature(g.key);
            UpdateVideo();
          }
          CoTaskMemFree(p);
        }
        it->Release();
      }
    }
    d->Release();
    Invalidate();
  }

  void ResetCover(const Game& g) {
    DeleteFileW((CacheDir() + L"\\" + g.key + L"_custom.jpg").c_str());
    Rescan();
  }

  // ------------------------------------------------------------ geometria
  void ClampScroll() {
    int rows = ((int)view.size() + Cols() - 1) / Cols();
    float content = BannerH() + rows * (CellH() + GAP) + 24;
    float mx = std::max(0.f, content - (Hd() - TB));
    scrollTarget = Clamp(scrollTarget, 0, mx);
    scroll = Clamp(scroll, 0, mx);
  }
  D2D1_RECT_F Card(int i) {
    int c = Cols();
    float used = c * Cw() + (c - 1) * GAP, x0 = GridL() + ((GridR() - GridL()) - used) / 2;
    float x = x0 + (i % c) * (Cw() + GAP), y = TB + 20 + BannerH() + (i / c) * (CellH() + GAP) - scroll;
    return { x, y, x + Cw(), y + Ch() };
  }
  int CardAt(float mx, float my) {
    if (my < TB || mx > GridR() + 10 || mx >= PanelX()) return -1;
    int c = Cols();
    float used = c * Cw() + (c - 1) * GAP, x0 = GridL() + ((GridR() - GridL()) - used) / 2;
    int col = (int)floorf((mx - x0) / (Cw() + GAP)), row = (int)floorf((my - TB - 20 - BannerH() + scroll) / (CellH() + GAP));
    if (col < 0 || col >= c || row < 0) return -1;
    int i = row * c + col;
    if (i >= (int)view.size()) return -1;
    D2D1_RECT_F r = Card(i);
    return (mx < r.right && my < r.bottom + 40) ? i : -1;
  }
  D2D1_RECT_F TL(int k) { float cx = 22 + k * 20.f; return { cx - 7, 20, cx + 7, 34 }; }
  static bool In(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
  static bool Hit(D2D1_RECT_F a, D2D1_RECT_F b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }

  // ------------------------------------------------------------ repintado parcial
  void Invalidate() { InvalidateRect(hwnd, nullptr, FALSE); }
  void InvalidateDip(D2D1_RECT_F r, float pad = 10) {
    RECT rc{ Px(r.left - pad), Px(r.top - pad), Px(r.right + pad), Px(r.bottom + pad) };
    InvalidateRect(hwnd, &rc, FALSE);
  }
  void InvalidateCard(int i) { if (i >= 0 && i < (int)view.size()) { D2D1_RECT_F r = Card(i); r.bottom += 44; InvalidateDip(r, 12); } }
  void InvalidatePanel() { InvalidateDip({ PanelX(), 0, Wd(), Hd() }, 2); }
  void InvalidateGrid() { InvalidateDip({ 0, TB, PanelX(), Hd() }, 0); }
  void InvalidateBtn(int i) { if (i >= 0 && i < (int)btns.size()) InvalidateDip(btns[i].r, 4); }

  void Animate() { SetTimer(hwnd, 6, 15, nullptr); }

  void AnimTick() {
    bool any = false;
    if (hoverIn.Active() || hoverOut.Active()) { InvalidateCard(hover); InvalidateCard(hoverPrev); any = true; }
    if (btnIn.Active() || btnOut.Active()) { InvalidateBtn(hotBtn); InvalidateBtn(btnPrev); any = true; }
    if (selAnim.Active()) { InvalidateCard(sel); InvalidatePanel(); any = true; }
    if (panelAnim.Active()) { Invalidate(); any = true; }
    if (slideFade.Active()) { InvalidateDip(BannerRect(), 4); any = true; }
    if (!any) KillTimer(hwnd, 6);  // nada se mueve: no se repinta
  }

  float HoverAmt(int i) const { return i == hover ? hoverIn.Get() : i == hoverPrev ? hoverOut.Get() : 0.f; }
  float BtnAmt(int i) const { return i == hotBtn ? btnIn.Get() : i == btnPrev ? btnOut.Get() : 0.f; }

  void SetHover(int i) {
    if (i == hover) return;
    InvalidateCard(hover);
    hoverOut.Set(hoverIn.Get()); hoverOut.Go(0, 160);
    hoverPrev = hover;
    hover = i;
    hoverIn.Set(0); hoverIn.Go(1, 140);
    InvalidateCard(hover);
    Animate();
  }

  void SetHotBtn(int i) {
    if (i == hotBtn) return;
    btnOut.Set(btnIn.Get()); btnOut.Go(0, 160);
    btnPrev = hotBtn;
    hotBtn = i;
    btnIn.Set(0); btnIn.Go(1, 120);
    InvalidateBtn(btnPrev); InvalidateBtn(hotBtn);
    Animate();
  }

  void Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rp = ps.rcPaint;
    if (rp.right > rp.left && rp.bottom > rp.top) {
      decodesLeft = 3;
      btns.clear();
      // solo se rasteriza la zona invalida: el render target se ata a ese rectangulo
      g_rt->BindDC(dc, &rp);
      g_rt->BeginDraw();
      g_rt->SetTransform(D2D1::Matrix3x2F::Translation(-Dip(rp.left), -Dip(rp.top)));
      g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
      paintClip = { Dip(rp.left), Dip(rp.top), Dip(rp.right), Dip(rp.bottom) };
      g_rt->PushAxisAlignedClip(paintClip, D2D1_ANTIALIAS_MODE_ALIASED);
      Draw(g_rt);
      g_rt->PopAxisAlignedClip();
      g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
      g_rt->EndDraw();
    }
    EndPaint(hwnd, &ps);
  }

  void AddBtn(D2D1_RECT_F r, std::function<void()> f) { btns.push_back({ r, f }); }

  // ------------------------------------------------------------ dibujo
  void Draw(ID2D1RenderTarget* rt) {
    if (!ficha.empty()) { DrawFicha(rt); if (!ficha.empty()) return; }
    rt->FillRectangle({ 0, 0, Wd(), Hd() }, Brush(WithA(Z::bg, .97f)));
    // iluminacion localizada muy suave detras de la biblioteca
    ID2D1GradientStopCollection* st = nullptr;
    D2D1_GRADIENT_STOP gs[2] = { { 0, WithA(Z::brand, .07f) }, { 1, WithA(Z::brand, 0) } };
    rt->CreateGradientStopCollection(gs, 2, &st);
    ID2D1RadialGradientBrush* rg = nullptr;
    rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({ 180, 0 }, { 0, 0 }, 620, 340), st, &rg);
    rt->FillRectangle({ 0, 0, LayoutRight(), Hd() }, rg);
    rg->Release(); st->Release();
    DrawGrid(rt);
    if (panelAnim.Get() > .001f) DrawPanel(rt);
    DrawTop(rt);
  }

  // filtro / boton con estados normal, hover y seleccionado
  void Chip(ID2D1RenderTarget* rt, D2D1_RECT_F r, const std::wstring& t, bool on, std::function<void()> f) {
    if (!Hit(r, paintClip)) { AddBtn(r, f); return; }  // fuera de la zona a repintar: solo se registra
    float hv = BtnAmt((int)btns.size());
    float rad = (r.bottom - r.top) / 2;
    if (on) {
      FillRound(rt, r, rad, WithA(Z::brand, .16f + .06f * hv));
      StrokeRound(rt, { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, rad, WithA(Z::brand, .7f), 1);
    } else if (hv > .01f) FillRound(rt, r, rad, WithA(Z::elev, hv));
    DrawText(rt, t, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), r, on ? Z::brandHi : MixC(Z::text2, Z::text, hv), AL_CENTER);
    AddBtn(r, f);
  }

  void ToolBtn(ID2D1RenderTarget* rt, D2D1_RECT_F r, const wchar_t* glyph, const std::wstring& t, std::function<void()> f) {
    if (!Hit(r, paintClip)) { AddBtn(r, f); return; }
    float hv = BtnAmt((int)btns.size());
    FillRound(rt, r, 10, MixC(Z::elev, Hex(0x2A2E35), hv));
    StrokeRound(rt, { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, 10, MixC(Z::border, Z::borderHi, hv), 1);
    if (t.empty()) DrawSymbol(rt, glyph, r, 13, MixC(Z::text2, Z::text, hv));
    else {
      DrawSymbol(rt, glyph, { r.left + 8, r.top, r.left + 30, r.bottom }, 12, MixC(Z::text2, Z::text, hv));
      DrawText(rt, t, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { r.left + 30, r.top, r.right - 8, r.bottom }, Z::text);
    }
    AddBtn(r, f);
  }

  void DrawTop(ID2D1RenderTarget* rt) {
    float right = PanelX();
    // si la barra no esta en la zona a repintar (p. ej. un cuadro de video) solo se calculan sus botones
    AddBtn({ 80, 0, 214, TB }, [this] { ShowAbout(hwnd); });  // logo y nombre: Acerca de (version, autor, licencia)
    bool vis = Hit({ 0, 0, right, TB }, paintClip);
    if (vis) {
    rt->FillRectangle({ 0, 0, right, TB }, Brush(WithA(Z::bg, .94f)));
    rt->FillRectangle({ 0, TB - 1, right, TB }, Brush(Z::border));
    D2D1_COLOR_F tl[3] = { RGBA(255, 95, 87), RGBA(254, 188, 46), RGBA(40, 200, 64) };
    for (int k = 0; k < 3; k++) {
      D2D1_RECT_F r = TL(k);
      float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
      rt->FillEllipse(D2D1::Ellipse({ cx, cy }, 6.5f, 6.5f), Brush(tl[k]));
      if (hoverTL) {
        D2D1_COLOR_F g = RGBA(0, 0, 0, .55f);
        if (k == 0) { rt->DrawLine({ cx - 2.5f, cy - 2.5f }, { cx + 2.5f, cy + 2.5f }, Brush(g), 1.2f); rt->DrawLine({ cx + 2.5f, cy - 2.5f }, { cx - 2.5f, cy + 2.5f }, Brush(g), 1.2f); }
        if (k == 1) rt->DrawLine({ cx - 3, cy }, { cx + 3, cy }, Brush(g), 1.3f);
        if (k == 2) { rt->DrawLine({ cx - 3, cy }, { cx + 3, cy }, Brush(g), 1.2f); rt->DrawLine({ cx, cy - 3 }, { cx, cy + 3 }, Brush(g), 1.2f); }
      }
    }
    // identidad: el logo de Zyntra y el nombre
    DrawSystemIcon(rt, SI_KIND_GAMES, { 84, 19, 114, 49 }, 1, false);
    DrawText(rt, L"ZYNTRA", Font(18, DWRITE_FONT_WEIGHT_BOLD, true), { 124, 0, 300, TB }, Z::text);
    }
    // filtros por tienda (solo las que tienen juegos); si no caben junto al buscador, se omiten los ultimos
    std::vector<std::pair<std::wstring, float>> chips;
    for (auto f : { L"Todos", L"Steam", L"Epic", L"Xbox", L"Riot", L"Otros" }) {
      std::wstring fs = f;
      if (fs != L"Todos" && std::none_of(games.begin(), games.end(), [&](const Game& g) { return g.platform == fs; })) continue;
      chips.push_back({ fs, TextWidth(fs, Font(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28 });
    }
    float r = right - 20;
    float panelBtnW = 36, addW = 36, sortW = 118, refW = 134, gap = 8;
    float tools = 0, x0 = 290, chipsW = 0;
    auto layout = [&] { tools = r - panelBtnW - gap - addW - gap - sortW - gap - refW - gap; };  // borde derecho del buscador
    layout();
    // ventana estrecha: Actualizar y el orden quedan solo con su icono y, si aun no cabe, se quitan los filtros
    float minLeft = x0 + (chips.empty() ? 0 : chips[0].second + 6) + 12 + 150;
    bool compactRef = tools < minLeft;
    if (compactRef) { refW = 36; layout(); }
    bool compactSort = tools < minLeft;
    if (compactSort) { sortW = 36; layout(); }
    if (tools < minLeft) chips.clear();
    for (auto& c : chips) chipsW += c.second + 6;
    while (chips.size() > 1 && x0 + chipsW + 12 + 170 > tools) { chipsW -= chips.back().second + 6; chips.pop_back(); }
    float x = x0;
    for (auto& c : chips) {
      std::wstring fs = c.first;
      Chip(rt, { x, 19, x + c.second, 49 }, fs, filter == fs, [this, fs] { filter = fs; scroll = scrollTarget = 0; Filter(); Invalidate(); });
      x += c.second + 6;
    }
    // buscador
    if (chips.empty()) x = 230;  // sin filtros, el buscador empieza junto al logo
    D2D1_RECT_F sr{ std::max(x + 10, tools - 260), 17, tools, 51 };
    bool showSearch = sr.right - sr.left >= 110;  // demasiado estrecha: sin buscador
    searchRect = showSearch ? sr : D2D1_RECT_F{};
    bool focus = searchActive || !search.empty();
    if (vis && showSearch) {
    FillRound(rt, sr, 10, Z::elev);
    StrokeRound(rt, { sr.left + .5f, sr.top + .5f, sr.right - .5f, sr.bottom - .5f }, 10, focus ? WithA(Z::brand, .75f) : Z::border, 1);
    DrawSymbol(rt, L"", { sr.left + 8, sr.top, sr.left + 32, sr.bottom }, 12, focus ? Z::brandHi : Z::text2);
    std::wstring s = search.empty() ? L"Buscar juego" : search;
    D2D1_RECT_F tr{ sr.left + 34, sr.top, sr.right - 12, sr.bottom };
    DrawText(rt, s, Font(13), tr, search.empty() ? Z::text3 : Z::text);
    if (focus && caret) {
      float cx = tr.left + (search.empty() ? 0 : TextWidth(search, Font(13))) + 1;
      rt->FillRectangle({ cx, sr.top + 10, cx + 1.5f, sr.bottom - 10 }, Brush(Z::brand));
    }
    }
    // actualizar, orden, añadir y panel
    float bx = tools + gap;
    ToolBtn(rt, { bx, 17, bx + refW, 51 }, L"", compactRef ? L"" : refreshing ? L"Actualizando…" : L"Actualizar", [this] { if (!refreshing) Refresh(); });
    bx += refW + gap;
    ToolBtn(rt, { bx, 17, bx + sortW, 51 }, L"", compactSort ? L"" : recent ? L"Recientes" : L"A–Z", [this] { recent = !recent; Filter(); Invalidate(); });
    bx += sortW + gap;
    ToolBtn(rt, { bx, 17, bx + addW, 51 }, L"", L"", [this] { AddMenu(); });
    bx += addW + gap;
    ToolBtn(rt, { bx, 17, bx + panelBtnW, 51 }, panelOpen ? L"" : L"", L"", [this] { TogglePanel(); });
  }

  void AddMenu() {
    std::vector<MenuItem> v;
    MenuItem a; a.text = L"Añadir juego…"; a.action = [this] { AddGame(); }; v.push_back(a);
    auto hidden = IniList(L"hidden");
    if (!hidden.empty()) {
      MenuItem h; h.text = L"Mostrar juegos ocultos (" + std::to_wstring(hidden.size()) + L")";
      h.action = [this] { IniSaveList(L"hidden", {}); Rescan(); };
      v.push_back(h);
    }
    POINT p; GetCursorPos(&p);
    ShowMenu(v, p.x, p.y);
  }

  void TogglePanel() {
    panelOpen = !panelOpen;
    panelAnim.Go(panelOpen ? 1.f : 0.f, 220);
    WritePrivateProfileStringW(L"Games", L"panel", panelOpen ? L"1" : L"0", IniPath().c_str());
    ClampScroll();
    Invalidate();
    Animate();
  }

  void DrawCover(ID2D1RenderTarget* rt, const Game& g, D2D1_RECT_F r, float rad, int px) {
    ID2D1Bitmap* b = Img(g.cover, px);
    PushRoundClip(rt, r, rad);
    if (b) {
      // recorte tipo "cover" para respetar la proporcion 2:3
      D2D1_SIZE_F s = b->GetSize();
      float k = std::max((r.right - r.left) / s.width, (r.bottom - r.top) / s.height);
      float bw = s.width * k, bh = s.height * k, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
      DrawBitmapHQ(rt, b, { cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2 });
    } else {
      // caratula generada: degradado con el color del nombre, logo/icono y titulo
      unsigned hsh = 0;
      for (wchar_t c : g.name) hsh = hsh * 31 + c;
      float hue = (hsh % 360) / 360.f;
      auto hsv = [](float h, float s, float v) {
        float r = fabsf(h * 6 - 3) - 1, gg = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
        return D2D1::ColorF(v * (1 - s + s * Clamp(r, 0, 1)), v * (1 - s + s * Clamp(gg, 0, 1)), v * (1 - s + s * Clamp(b, 0, 1)));
      };
      ID2D1GradientStopCollection* st = nullptr;
      D2D1_GRADIENT_STOP gs[2] = { { 0, hsv(hue, .45f, .42f) }, { 1, hsv(fmodf(hue + .08f, 1), .6f, .14f) } };
      rt->CreateGradientStopCollection(gs, 2, &st);
      ID2D1LinearGradientBrush* lg = nullptr;
      rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ r.left, r.top }, { r.right, r.bottom }), st, &lg);
      rt->FillRectangle(r, lg);
      lg->Release(); st->Release();
      float s = (r.right - r.left) * .46f, cx = (r.left + r.right) / 2, cy = r.top + (r.bottom - r.top) * .4f;
      D2D1_RECT_F ir{ cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2 };
      if (ID2D1Bitmap* lb = Img(g.logo, px)) DrawBitmapHQ(rt, lb, ir);
      else if (!g.exe.empty()) { if (ID2D1Bitmap* ib = GetIcon(g.exe, g.name, Px(96))) DrawBitmapHQ(rt, ib, ir); }
      else DrawText(rt, g.name.substr(0, 1), Font(s * .8f, DWRITE_FONT_WEIGHT_BOLD, true), ir, WithA(Z::text, .9f), AL_CENTER);
      IDWriteTextLayout* tl = nullptr;
      auto f = Font((r.right - r.left) * .1f, DWRITE_FONT_WEIGHT_BOLD);
      g_dw->CreateTextLayout(g.name.c_str(), (UINT32)g.name.size(), f, r.right - r.left - 20, 80, &tl);
      if (tl) {
        tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        rt->DrawTextLayout({ r.left + 10, r.bottom - (r.bottom - r.top) * .3f }, tl, Brush(Z::text), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        tl->Release();
      }
    }
    PopClip(rt);
  }

  // ------------------------------------------------------------ carrusel destacado
  struct BannerLay { D2D1_RECT_F r, cover, text, play, info, video, prev, next, vbtn; std::vector<D2D1_RECT_F> dots; bool video_on; float k = 1; };
  BannerLay LayoutBanner() {
    BannerLay b;
    b.r = BannerRect();
    float W = b.r.right - b.r.left, H = b.r.bottom - b.r.top;
    // escala respecto al carrusel base (ventana normal), en pasos de 0.1: las fuentes se guardan en cache por tamaño
    float k = b.k = roundf(Clamp(H / 274, 1, 2) * 10) / 10;
    b.video_on = W >= 860;
    // el video ocupa casi todo el alto del carrusel (16:9) y como mucho la mitad del ancho
    float vh = b.video_on ? H - 52 * k : 0, vw = std::min(vh * 16 / 9, W * .5f);
    vh = vw * 9 / 16;
    b.video = { b.r.right - 30 * k - vw, b.r.top + (H - vh) / 2 - 8, b.r.right - 30 * k, b.r.top + (H - vh) / 2 - 8 + vh };
    b.vbtn = { b.video.right - 42, b.video.top + 10, b.video.right - 12, b.video.top + 40 };
    b.cover = { b.r.left + 30 * k, b.r.top + 28 * k, b.r.left + 30 * k + 144 * k, b.r.top + 28 * k + 216 * k };
    float tx = b.cover.right + 28 * k, tr = b.video_on ? b.video.left - 32 * k : b.r.right - 30 * k;
    b.text = { tx, b.r.top + 26 * k, tr, b.r.top + 200 * k };
    b.play = { tx, b.r.top + 204 * k, tx + 136 * k, b.r.top + 204 * k + 40 * std::sqrt(k) };
    b.info = { tx + 146 * k, b.play.top, tx + 146 * k + 118 * k, b.play.bottom };
    float cy = (b.r.top + b.r.bottom) / 2;
    b.prev = { b.r.left + 8, cy - 18, b.r.left + 44, cy + 18 };
    b.next = { b.r.right - 44, cy - 18, b.r.right - 8, cy + 18 };
    float n = (float)slides.size(), dx = 16, x0 = (b.r.left + b.r.right) / 2 - (n - 1) * dx / 2;
    for (size_t i = 0; i < slides.size(); i++) b.dots.push_back({ x0 + i * dx - 7, b.r.bottom - 22, x0 + i * dx + 7, b.r.bottom - 8 });
    return b;
  }

  void ShowSlideInfo(const Game& g) { OpenFicha(g); }

  // ------------------------------------------------------------ ficha a pantalla completa
  void OpenFicha(const Game& g) {
    for (int i = 0; i < (int)view.size(); i++) if (view[i].key == g.key) { Select(i); break; }
    ficha = g.key;
    fichaPage.Set(0);
    LoadFichaData();
    RequestClip(g.key, true);  // si aun no tiene video, se descarga primero
    unsigned id = g.steamId;
    if (!id) id = (unsigned)strtoul(ReadFileA(CacheDir() + L"\\" + g.key + L".id").c_str(), nullptr, 10);
    if (!Exists(CacheDir() + L"\\" + g.key + L".rep") || !Exists(CacheDir() + L"\\" + g.key + L".gal"))
      std::thread(FetchFichaData, hwnd, g.key, g.name, id).detach();
    SetHover(-1); SetHotBtn(-1);
    UpdateVideo();
    Invalidate();
  }

  void CloseFicha() {
    if (ficha.empty()) return;
    ficha.clear();
    fichaGal.clear();
    KillTimer(hwnd, 10);
    UpdateVideo();
    Invalidate();
  }

  // reputacion y galeria desde la cache (FetchFichaData las descarga)
  void LoadFichaData() {
    std::wstring base = CacheDir() + L"\\" + ficha;
    fichaRep = ReadFileA(base + L".rep");
    fichaGal.clear();
    int n = atoi(ReadFileA(base + L".gal").c_str());
    for (int i = 0; i < n && i < 6; i++) {
      std::wstring f = base + L"_g" + std::to_wstring(i) + L".jpg";
      if (Exists(f)) fichaGal.push_back(f);
    }
  }

  // 0 = caratula y video lado a lado; 1 = video a la izquierda con reseñas y galeria
  void FichaPage(int p) {
    if ((p ? 1.f : 0.f) == fichaPage.to) return;
    fichaPage.Go(p ? 1.f : 0.f, 420);
    SetTimer(hwnd, 10, 15, nullptr);
  }

  void DrawImageCover(ID2D1RenderTarget* rt, ID2D1Bitmap* bm, D2D1_RECT_F r, float a) {
    D2D1_SIZE_F s = bm->GetSize();
    float k = std::max((r.right - r.left) / s.width, (r.bottom - r.top) / s.height), bw = s.width * k, bh = s.height * k;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    rt->DrawBitmap(bm, { cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2 }, a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }

  static D2D1_RECT_F LerpR(D2D1_RECT_F a, D2D1_RECT_F b, float t) {
    return { Lerp(a.left, b.left, t), Lerp(a.top, b.top, t), Lerp(a.right, b.right, t), Lerp(a.bottom, b.bottom, t) };
  }

  // texto con ajuste de linea y "…" si no cabe; devuelve la altura usada
  float DrawPara(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (s.empty() || r.bottom - r.top < 8) return 0;
    IDWriteTextLayout* tl = nullptr;
    g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, r.right - r.left, r.bottom - r.top, &tl);
    if (!tl) return 0;
    tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TRIMMING trm{ DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0 };
    IDWriteInlineObject* ell = nullptr;
    g_dw->CreateEllipsisTrimmingSign(tl, &ell);
    tl->SetTrimming(&trm, ell);
    if (ell) ell->Release();
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    rt->DrawTextLayout({ r.left, r.top }, tl, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    tl->Release();
    return std::min(m.height, r.bottom - r.top);
  }

  void DrawReputation(ID2D1RenderTarget* rt, D2D1_RECT_F r, float k, float a) {
    FillRound(rt, r, 14, RGBA(255, 255, 255, .07f * a));
    StrokeRound(rt, { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, 13.5f, RGBA(255, 255, 255, .12f * a), 1);
    float x = r.left + 20 * k, w = r.right - 20 * k;
    DrawText(rt, L"REPUTACIÓN", Font(11 * k, DWRITE_FONT_WEIGHT_BOLD), { x, r.top + 14 * k, w, r.top + 30 * k }, WithA(Z::text2, a));
    float y = r.top + 36 * k;
    if (fichaRep.size() > 2 && fichaRep[0] == 'S') {  // reseñas de Steam: mismo criterio que la tienda
      unsigned long long pos = 0, neg = 0;
      sscanf_s(fichaRep.c_str() + 2, "%llu|%llu", &pos, &neg);
      unsigned long long tot = pos + neg;
      double pct = tot ? 100.0 * pos / tot : 0;
      const wchar_t* lbl = pct >= 95 && tot >= 500 ? L"Extremadamente positivas" : pct >= 80 ? (tot >= 50 ? L"Muy positivas" : L"Positivas")
                         : pct >= 70 ? L"Mayormente positivas" : pct >= 40 ? L"Variadas" : pct >= 20 ? L"Mayormente negativas"
                         : tot >= 500 ? L"Extremadamente negativas" : L"Negativas";
      D2D1_COLOR_F col = pct >= 70 ? Hex(0x66C0F4) : pct >= 40 ? Hex(0xB9A074) : Hex(0xC35C2C);
      DrawText(rt, lbl, Font(roundf(22 * k), DWRITE_FONT_WEIGHT_BOLD), { x, y, w, y + 30 * k }, WithA(col, a));
      y += 36 * k;
      FillRound(rt, { x, y, w, y + 8 }, 4, RGBA(255, 255, 255, .12f * a));
      FillRound(rt, { x, y, x + (w - x) * (float)(pct / 100), y + 8 }, 4, WithA(col, a));
      y += 18;
      wchar_t b[128];
      swprintf_s(b, L"%.0f%% de %s reseñas de jugadores en Steam", pct, Thousands(tot).c_str());
      DrawText(rt, b, Font(13 * k), { x, y, w, y + 20 * k }, WithA(Z::text2, a));
    } else if (fichaRep.size() > 2 && fichaRep[0] == 'M') {  // tienda de Microsoft: estrellas de 1 a 5
      double avg = 0; unsigned long long cnt = 0;
      sscanf_s(fichaRep.c_str() + 2, "%lf|%llu", &avg, &cnt);
      wchar_t b[128];
      swprintf_s(b, L"%.1f", avg);
      DrawText(rt, b, Font(roundf(30 * k), DWRITE_FONT_WEIGHT_BOLD, true), { x, y - 4, x + 70 * k, y + 34 * k }, WithA(Z::text, a));
      float sx = x + 66 * k, ss = 20 * k;
      for (int i = 0; i < 5; i++) {
        float fill = Clamp((float)avg - i, 0, 1);
        D2D1_RECT_F sr{ sx + i * (ss + 4), y + 4, sx + i * (ss + 4) + ss, y + 4 + ss };
        DrawSymbol(rt, L"", sr, ss * .8f, RGBA(255, 255, 255, .25f * a));                  // estrella vacia
        if (fill > 0) {
          rt->PushAxisAlignedClip({ sr.left, sr.top, sr.left + (sr.right - sr.left) * fill, sr.bottom }, D2D1_ANTIALIAS_MODE_ALIASED);
          DrawSymbol(rt, L"", sr, ss * .8f, WithA(Hex(0xFFC83D), a));                       // estrella llena
          rt->PopAxisAlignedClip();
        }
      }
      y += 40 * k;
      swprintf_s(b, L"%s valoraciones en Microsoft Store", Thousands(cnt).c_str());
      DrawText(rt, b, Font(13 * k), { x, y, w, y + 20 * k }, WithA(Z::text2, a));
    } else {
      DrawText(rt, fichaRep == "-" ? L"Sin reseñas disponibles" : L"Buscando reseñas…", Font(15 * k), { x, y, w, y + 24 * k }, WithA(Z::text2, a));
    }
  }

  static std::wstring Thousands(unsigned long long v) {
    std::wstring s = std::to_wstring(v), o;
    for (size_t i = 0; i < s.size(); i++) { if (i && (s.size() - i) % 3 == 0) o += L'.'; o += s[i]; }
    return o;
  }

  void DrawFicha(ID2D1RenderTarget* rt) {
    const Game* g = FindGame(ficha);
    if (!g) { ficha.clear(); return; }
    size_t firstBtn = btns.size();
    float W = Wd(), H = Hd(), t = fichaPage.Get(), k = Clamp(H / 900, .85f, 1.5f);
    float mx = std::max(40.f, W * .06f);
    // fondo: la captura del juego decodificada diminuta y estirada (desenfoque sin costo) y oscurecida
    rt->FillRectangle({ 0, 0, W, H }, Brush(Z::bg));
    if (ID2D1Bitmap* bg = Img(!g->shot.empty() ? g->shot : !g->hero.empty() ? g->hero : g->cover, 40)) {
      D2D1_SIZE_F s = bg->GetSize();
      float kk = std::max(W / s.width, H / s.height) * 1.1f, bw = s.width * kk, bh = s.height * kk;
      rt->DrawBitmap(bg, { (W - bw) / 2, (H - bh) / 2, (W + bw) / 2, (H + bh) / 2 }, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    rt->FillRectangle({ 0, 0, W, H }, Brush(RGBA(8, 9, 12, .62f)));

    // encabezado fijo en las dos secciones: tienda y "jugado hace", titulo, Jugar y cerrar
    float y = 28 * k;
    std::wstring meta = g->platform;
    std::wstring ago = Ago(g->lastPlayed);
    if (!ago.empty()) meta += L"  ·  Jugado: " + ago;
    DrawText(rt, meta, Font(13 * k, DWRITE_FONT_WEIGHT_SEMI_BOLD), { mx, y, W - 320 * k, y + 20 * k }, Z::brandHi);
    DrawText(rt, g->name, Font(roundf(36 * k), DWRITE_FONT_WEIGHT_BOLD, true), { mx, y + 22 * k, W - 320 * k, y + 72 * k }, Z::text);
    D2D1_RECT_F xr{ W - 64, 24 * k, W - 22, 24 * k + 42 };
    D2D1_RECT_F pr{ xr.left - 16 - 190 * k, 26 * k, xr.left - 16, 26 * k + 52 * k };
    int pi = (int)btns.size();
    Game copy = *g;
    AddBtn(pr, [this, copy] { if (!(ls == LS_START && lsKey == copy.key)) Launch(copy); });
    LaunchState lsNow = lsKey == g->key ? ls : LS_NONE;
    D2D1_COLOR_F pc = lsNow == LS_ERR ? WithA(Z::err, .9f) : lsNow == LS_RUN ? WithA(Z::ok, .9f) : MixC(Z::brand, Z::brandHi, BtnAmt(pi));
    if (pressBtn == pi) pc = Z::brandLo;
    FillRound(rt, pr, 14, pc);
    std::wstring pl = lsNow == LS_START ? L"Iniciando…" : lsNow == LS_RUN ? L"En ejecución" : lsNow == LS_ERR ? L"Error" : L"Jugar";
    auto fp = Font(roundf(17 * k), DWRITE_FONT_WEIGHT_BOLD);
    float tw = TextWidth(pl, fp), pcx = (pr.left + pr.right) / 2;
    DrawSymbol(rt, lsNow == LS_RUN ? L"" : L"", { pcx - tw / 2 - 28 * k, pr.top, pcx - tw / 2 - 6, pr.bottom }, 14 * k, RGBA(255, 255, 255));
    DrawText(rt, pl, fp, { pcx - tw / 2, pr.top, pcx + tw / 2 + 4, pr.bottom }, RGBA(255, 255, 255));
    int xi = (int)btns.size();
    AddBtn(xr, [this] { CloseFicha(); });
    rt->FillEllipse(D2D1::Ellipse({ (xr.left + xr.right) / 2, (xr.top + xr.bottom) / 2 }, 21, 21), Brush(RGBA(0, 0, 0, .35f + .25f * BtnAmt(xi))));
    DrawSymbol(rt, L"", xr, 14, Z::text);

    // zona de contenido
    float top = 120 * k, bottom = H - 56 * k, ch = bottom - top, cw = W - 2 * mx;
    // seccion 1: caratula y video lado a lado, descripcion debajo del video
    float coverH = std::min(ch, (cw * .34f) * 1.5f), coverW = coverH / 1.5f;
    D2D1_RECT_F cover0{ mx, top, mx + coverW, top + coverH };
    float vx0 = cover0.right + 36 * k, vw0 = W - mx - vx0, vh0 = std::min(vw0 * 9 / 16, coverH * .72f);
    vw0 = vh0 * 16 / 9;
    D2D1_RECT_F video0{ vx0, top, vx0 + vw0, top + vh0 };
    // seccion 2: el video pasa a la izquierda; a la derecha reputacion, descripcion y galeria
    float vw1 = cw * .46f, vh1 = vw1 * 9 / 16;
    D2D1_RECT_F video1{ mx, top, mx + vw1, top + vh1 };
    D2D1_RECT_F vr = LerpR(video0, video1, t);

    // caratula (sale hacia la izquierda al pasar a la seccion 2)
    if (t < .999f) {
      float a = 1 - t, dx = -t * (coverW + mx);
      D2D1_RECT_F cr{ cover0.left + dx, cover0.top, cover0.right + dx, cover0.bottom };
      FillRound(rt, { cr.left - 4, cr.top + 14, cr.right + 4, cr.bottom + 22 }, 22, RGBA(0, 0, 0, .45f * a));
      FillRound(rt, cr, 18, Z::elev);
      DrawCover(rt, *g, cr, 18, CoverPx(coverW));
      StrokeRound(rt, { cr.left + .5f, cr.top + .5f, cr.right - .5f, cr.bottom - .5f }, 17.5f, RGBA(255, 255, 255, .22f), 1);
      // descripcion bajo el video
      float dy = vr.bottom + 22 * k;
      DrawText(rt, L"ACERCA DEL JUEGO", Font(11 * k, DWRITE_FONT_WEIGHT_BOLD), { video0.left, dy, video0.right, dy + 16 * k }, WithA(Z::text2, a));
      DrawPara(rt, g->desc.empty() ? L"Sin descripción. Pulsa Actualizar en la biblioteca para buscarla." : g->desc, Font(roundf(16 * k)),
               { video0.left, dy + 24 * k, video0.right, cover0.bottom }, WithA(Z::text, .85f * a));
    }

    // video (nitido) o, mientras se descarga, la captura del juego
    fichaVideoRect = vr;
    FillRound(rt, { vr.left - 2, vr.top + 10, vr.right + 2, vr.bottom + 16 }, 18, RGBA(0, 0, 0, .4f));
    PushRoundClip(rt, vr, 16);
    FillRound(rt, vr, 0, RGBA(0, 0, 0));
    bool playing = videoBmp && player && player->key == g->key;
    if (playing) DrawImageCover(rt, videoBmp, vr, 1);
    else if (ID2D1Bitmap* sb = Img(!g->shot.empty() ? g->shot : !g->hero.empty() ? g->hero : g->cover, Px(900))) DrawImageCover(rt, sb, vr, 1);
    PopClip(rt);
    StrokeRound(rt, { vr.left + .5f, vr.top + .5f, vr.right - .5f, vr.bottom - .5f }, 15.5f, RGBA(255, 255, 255, .18f), 1);
    std::wstring tag = playing ? L"GAMEPLAY" : Exists(TrailerFile(g->key)) ? L"GAMEPLAY" : L"BUSCANDO VIDEO…";
    auto ft = Font(10.5f, DWRITE_FONT_WEIGHT_BOLD);
    float tgw = TextWidth(tag, ft) + 18;
    FillRound(rt, { vr.left + 12, vr.top + 12, vr.left + 12 + tgw, vr.top + 32 }, 6, RGBA(0, 0, 0, .55f));
    DrawText(rt, tag, ft, { vr.left + 12, vr.top + 12, vr.left + 12 + tgw, vr.top + 32 }, playing ? Z::brandHi : Z::text2, AL_CENTER);

    // seccion 2 (entra desde abajo)
    if (t > .001f) {
      float a = t, dy = (1 - t) * 60 * k;
      float rx = video1.right + 36 * k, rr = W - mx;
      DrawReputation(rt, { rx, top + dy, rr, top + dy + 130 * k }, k, a);
      // descripcion bajo el video, del lado izquierdo
      float ly = video1.bottom + 22 * k + dy;
      DrawText(rt, L"ACERCA DEL JUEGO", Font(11 * k, DWRITE_FONT_WEIGHT_BOLD), { video1.left, ly, video1.right, ly + 16 * k }, WithA(Z::text2, a));
      DrawPara(rt, g->desc.empty() ? L"Sin descripción." : g->desc, Font(roundf(15 * k)), { video1.left, ly + 24 * k, video1.right, bottom }, WithA(Z::text, .85f * a));
      // galeria de capturas de gameplay
      float gy = top + dy + 150 * k;
      DrawText(rt, L"GALERÍA", Font(11 * k, DWRITE_FONT_WEIGHT_BOLD), { rx, gy, rr, gy + 16 * k }, WithA(Z::text2, a));
      gy += 26 * k;
      if (fichaGal.empty()) DrawText(rt, fichaRep.empty() ? L"Buscando imágenes…" : L"Sin imágenes disponibles", Font(14 * k), { rx, gy, rr, gy + 22 * k }, WithA(Z::text2, a));
      int cols = 2;
      float gap = 12 * k, tw2 = (rr - rx - gap * (cols - 1)) / cols, th2 = tw2 * 9 / 16;
      for (size_t i = 0; i < fichaGal.size(); i++) {
        float x = rx + (i % cols) * (tw2 + gap), yy = gy + (i / cols) * (th2 + gap);
        if (yy + th2 > bottom + dy) break;
        D2D1_RECT_F ir{ x, yy, x + tw2, yy + th2 };
        PushRoundClip(rt, ir, 10);
        FillRound(rt, ir, 0, RGBA(255, 255, 255, .06f));
        if (ID2D1Bitmap* bm = Img(fichaGal[i], Px(ceilf(tw2 / 80) * 80))) DrawImageCover(rt, bm, ir, a);
        PopClip(rt);
        std::wstring f = fichaGal[i];
        AddBtn(ir, [this, f] { fichaBig = f; Invalidate(); });  // ampliar
      }
    }

    // indicador de seccion
    D2D1_RECT_F hr{ W / 2 - 150, H - 44 * k, W / 2 + 150, H - 16 * k };
    AddBtn(hr, [this] { FichaPage(fichaPage.to > .5f ? 0 : 1); });
    DrawText(rt, t < .5f ? L"  Desliza para ver reputación y galería" : L"  Volver a la carátula", Font(12.5f * k),
             hr, WithA(Z::text, .55f + .3f * BtnAmt((int)btns.size() - 1)), AL_CENTER);

    // captura ampliada
    if (!fichaBig.empty()) {
      rt->FillRectangle({ 0, 0, W, H }, Brush(RGBA(0, 0, 0, .82f)));
      if (ID2D1Bitmap* bm = Img(fichaBig, Px(640))) {
        D2D1_SIZE_F s = bm->GetSize();
        float kk = std::min((W - 120) / s.width, (H - 120) / s.height), bw = s.width * kk, bh = s.height * kk;
        D2D1_RECT_F br{ (W - bw) / 2, (H - bh) / 2, (W + bw) / 2, (H + bh) / 2 };
        PushRoundClip(rt, br, 14);
        rt->DrawBitmap(bm, br, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        PopClip(rt);
      }
      btns.resize(firstBtn);  // con la captura ampliada solo vale el clic para cerrarla
      AddBtn({ 0, 0, W, H }, [this] { fichaBig.clear(); Invalidate(); });
    }
  }

  void BannerButtons(const BannerLay& b) {
    const Game* g = SlideGame(slide);
    if (!g) return;
    Game copy = *g;
    AddBtn(b.play, [this, copy] { Launch(copy); });
    AddBtn(b.info, [this, copy] { ShowSlideInfo(copy); });
    AddBtn(b.prev, [this] { GoSlide(slide - 1); });
    AddBtn(b.next, [this] { GoSlide(slide + 1); });
    for (size_t i = 0; i < b.dots.size(); i++) AddBtn(b.dots[i], [this, i] { GoSlide((int)i); });
    if (b.video_on && Exists(TrailerFile(g->key)))
      AddBtn(b.vbtn, [this] {  // pausar / reanudar los traileres (se recuerda)
        trailers = !trailers;
        WritePrivateProfileStringW(L"Games", L"trailers", trailers ? L"1" : L"0", IniPath().c_str());
        UpdateVideo();
        InvalidateDip(videoRect, 2);
      });
    AddBtn(b.r, [this, copy] { ShowSlideInfo(copy); });  // clic en el fondo: abrir la ficha
  }

  void DrawSlideArt(ID2D1RenderTarget* rt, const Game& g, D2D1_RECT_F r, float a) {
    // fondo: una captura del juego, asi no repite la caratula que se ve al lado
    std::wstring art = !g.shot.empty() ? g.shot : !g.hero.empty() ? g.hero : g.cover;
    if (ID2D1Bitmap* bm = Img(art, Px(900))) {
      D2D1_SIZE_F s = bm->GetSize();
      float k = std::max((r.right - r.left) / s.width, (r.bottom - r.top) / s.height);
      float bw = s.width * k, bh = s.height * k, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
      // ya esta escalado cerca de su tamaño final: la interpolacion lineal basta y es mucho mas barata
      rt->DrawBitmap(bm, { cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2 }, .55f * a, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
  }

  void DrawBanner(ID2D1RenderTarget* rt, const BannerLay& b, int firstBtn) {
    const Game* g = SlideGame(slide);
    if (!g || !Hit(b.r, paintClip)) return;
    float t = slideFade.Get();
    PushRoundClip(rt, b.r, 18);
    FillRound(rt, b.r, 0, Z::panel);
    if (const Game* pg = SlideGame(prevSlide); pg && t < 1) DrawSlideArt(rt, *pg, b.r, 1 - t);  // fundido cruzado
    DrawSlideArt(rt, *g, b.r, t);
    // degradado para que el texto se lea sobre cualquier arte
    ID2D1GradientStopCollection* st = nullptr;
    D2D1_GRADIENT_STOP gs[3] = { { 0, WithA(Z::panel, .97f) }, { .5f, WithA(Z::panel, .78f) }, { 1, WithA(Z::panel, .35f) } };
    rt->CreateGradientStopCollection(gs, 3, &st);
    ID2D1LinearGradientBrush* lg = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ b.r.left, 0 }, { b.r.right, 0 }), st, &lg);
    rt->FillRectangle(b.r, lg);
    lg->Release(); st->Release();
    float dx = (1 - t) * 14;
    // caratula
    D2D1_RECT_F cr{ b.cover.left + dx, b.cover.top, b.cover.right + dx, b.cover.bottom };
    if (Hit(cr, paintClip)) {
      FillRound(rt, { cr.left - 2, cr.top + 8, cr.right + 2, cr.bottom + 12 }, 14, RGBA(0, 0, 0, .4f));
      FillRound(rt, cr, 12, Z::elev);
      DrawCover(rt, *g, cr, 12, CoverPx(144 * b.k));
      StrokeRound(rt, { cr.left + .5f, cr.top + .5f, cr.right - .5f, cr.bottom - .5f }, 11.5f, WithA(Z::borderHi, .8f), 1);
    }
    // textos
    if (Hit(b.text, paintClip)) {
      float x = b.text.left + dx, w = b.text.right - b.text.left;
      DrawText(rt, L"DESTACADO", Font(11 * b.k, DWRITE_FONT_WEIGHT_BOLD), { x, b.text.top, x + w, b.text.top + 16 * b.k }, WithA(Z::brand, t));
      float y = b.text.top + 22 * b.k;
      IDWriteTextLayout* tl = nullptr;
      g_dw->CreateTextLayout(g->name.c_str(), (UINT32)g->name.size(), Font(26 * b.k, DWRITE_FONT_WEIGHT_BOLD, true), w, 68 * b.k, &tl);
      if (tl) {
        tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS m; tl->GetMetrics(&m);
        rt->DrawTextLayout({ x, y }, tl, Brush(WithA(Z::text, t)), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        y += std::min(m.height, 68.f * b.k) + 6 * b.k;
        tl->Release();
      }
      std::wstring meta = g->platform;
      std::wstring ago = Ago(g->lastPlayed);
      if (!ago.empty()) meta += L"  ·  Jugado: " + ago;
      DrawText(rt, meta, Font(12.5f * b.k, DWRITE_FONT_WEIGHT_SEMI_BOLD), { x, y, x + w, y + 18 * b.k }, WithA(Z::text2, t));
      y += 26 * b.k;
      if (!g->desc.empty() && y + 20 < b.text.bottom) {
        g_dw->CreateTextLayout(g->desc.c_str(), (UINT32)g->desc.size(), Font(13 * std::sqrt(b.k)), w, b.text.bottom - y, &tl);
        if (tl) {
          tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
          tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
          tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
          DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0 };
          IDWriteInlineObject* ell = nullptr;
          g_dw->CreateEllipsisTrimmingSign(tl, &ell);
          tl->SetTrimming(&tr, ell);
          if (ell) ell->Release();
          rt->DrawTextLayout({ x, y }, tl, Brush(WithA(Z::text2, .9f * t)), D2D1_DRAW_TEXT_OPTIONS_CLIP);
          tl->Release();
        }
      }
    }
    // botones Jugar y Ver ficha
    if (Hit({ b.play.left, b.play.top, b.info.right, b.info.bottom }, paintClip)) {
      float hv = BtnAmt(firstBtn), hi = BtnAmt(firstBtn + 1);
      LaunchState lsNow = lsKey == g->key ? ls : LS_NONE;
      D2D1_COLOR_F pc = lsNow == LS_ERR ? WithA(Z::err, .9f) : lsNow == LS_RUN ? WithA(Z::ok, .9f) : MixC(Z::brand, Z::brandHi, hv);
      if (pressBtn == firstBtn) pc = Z::brandLo;
      FillRound(rt, b.play, 10, pc);
      std::wstring pl = lsNow == LS_START ? L"Iniciando…" : lsNow == LS_RUN ? L"En ejecución" : lsNow == LS_ERR ? L"Error" : L"Jugar";
      auto fp = Font(14 * std::sqrt(b.k), DWRITE_FONT_WEIGHT_BOLD);
      float tw = TextWidth(pl, fp), cx = (b.play.left + b.play.right) / 2;
      DrawSymbol(rt, lsNow == LS_RUN ? L"" : L"", { cx - tw / 2 - 24, b.play.top, cx - tw / 2 - 4, b.play.bottom }, 13, RGBA(255, 255, 255));
      DrawText(rt, pl, fp, { cx - tw / 2, b.play.top, cx + tw / 2 + 4, b.play.bottom }, RGBA(255, 255, 255));
      FillRound(rt, b.info, 10, MixC(WithA(Z::elev, .9f), Hex(0x2A2E35), hi));
      StrokeRound(rt, { b.info.left + .5f, b.info.top + .5f, b.info.right - .5f, b.info.bottom - .5f }, 9.5f, MixC(Z::border, Z::borderHi, hi), 1);
      DrawText(rt, L"Ver ficha", Font(13 * std::sqrt(b.k), DWRITE_FONT_WEIGHT_SEMI_BOLD), b.info, Z::text, AL_CENTER);
    }
    PopClip(rt);
    // trailer (o arte del juego si no hay)
    videoRect = b.video_on ? b.video : D2D1_RECT_F{};
    if (b.video_on && Hit(b.video, paintClip)) {
      DrawShadowLite(rt, b.video);
      PushRoundClip(rt, b.video, 14);
      FillRound(rt, b.video, 0, RGBA(0, 0, 0));
      bool playing = videoBmp && player && player->key == g->key;
      if (playing) {
        D2D1_SIZE_F s = videoBmp->GetSize();
        float k = std::max((b.video.right - b.video.left) / s.width, (b.video.bottom - b.video.top) / s.height);
        float bw = s.width * k, bh = s.height * k, cx = (b.video.left + b.video.right) / 2, cy = (b.video.top + b.video.bottom) / 2;
        rt->DrawBitmap(videoBmp, { cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2 }, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      } else {
        std::wstring art = !g->hero.empty() ? g->hero : g->cover;
        if (ID2D1Bitmap* bm = Img(art, Px(900))) {
          D2D1_SIZE_F s = bm->GetSize();
          float k = std::max((b.video.right - b.video.left) / s.width, (b.video.bottom - b.video.top) / s.height);
          float bw = s.width * k, bh = s.height * k, cx = (b.video.left + b.video.right) / 2, cy = (b.video.top + b.video.bottom) / 2;
          DrawBitmapHQ(rt, bm, { cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2 }, t);
        }
      }
      // etiqueta
      std::wstring tag = playing || Exists(TrailerFile(g->key)) ? L"GAMEPLAY" : L"ARTE DEL JUEGO";
      auto ft = Font(10.5f, DWRITE_FONT_WEIGHT_BOLD);
      float tw = TextWidth(tag, ft) + 18;
      FillRound(rt, { b.video.left + 12, b.video.top + 12, b.video.left + 12 + tw, b.video.top + 32 }, 6, RGBA(0, 0, 0, .55f));
      DrawText(rt, tag, ft, { b.video.left + 12, b.video.top + 12, b.video.left + 12 + tw, b.video.top + 32 }, playing ? Z::brandHi : Z::text2, AL_CENTER);
      if (Exists(TrailerFile(g->key))) {  // pausar / reproducir
        D2D1_RECT_F vb = b.vbtn;
        rt->FillEllipse(D2D1::Ellipse({ (vb.left + vb.right) / 2, (vb.top + vb.bottom) / 2 }, 15, 15), Brush(RGBA(0, 0, 0, .55f)));
        DrawSymbol(rt, trailers ? L"" : L"", vb, 11, Z::text);
      }
      PopClip(rt);
      StrokeRound(rt, { b.video.left + .5f, b.video.top + .5f, b.video.right - .5f, b.video.bottom - .5f }, 13.5f, WithA(Z::borderHi, .7f), 1);
    }
    StrokeRound(rt, { b.r.left + .5f, b.r.top + .5f, b.r.right - .5f, b.r.bottom - .5f }, 17.5f, Z::border, 1);
    // indicadores y flechas
    for (size_t i = 0; i < b.dots.size(); i++) {
      D2D1_RECT_F d = b.dots[i];
      float cx = (d.left + d.right) / 2, cy = (d.top + d.bottom) / 2;
      if ((int)i == slide) FillRound(rt, { cx - 9, cy - 3, cx + 9, cy + 3 }, 3, Z::brand);
      else rt->FillEllipse(D2D1::Ellipse({ cx, cy }, 3, 3), Brush(WithA(Z::text, .35f)));
    }
    if (bannerHover && slides.size() > 1) {
      for (int k = 0; k < 2; k++) {
        D2D1_RECT_F a = k ? b.next : b.prev;
        float hv = BtnAmt(firstBtn + 2 + k);
        rt->FillEllipse(D2D1::Ellipse({ (a.left + a.right) / 2, (a.top + a.bottom) / 2 }, 18, 18), Brush(RGBA(0, 0, 0, .45f + .25f * hv)));
        DrawSymbol(rt, k ? L"" : L"", a, 13, Z::text);
      }
    }
  }

  void DrawShadowLite(ID2D1RenderTarget* rt, D2D1_RECT_F r) {
    FillRound(rt, { r.left - 2, r.top + 8, r.right + 2, r.bottom + 12 }, 16, RGBA(0, 0, 0, .35f));
  }

  void DrawGrid(ID2D1RenderTarget* rt) {
    D2D1_RECT_F area{ 0, TB, PanelX(), Hd() };
    // los botones del carrusel se registran aunque esta zona no se repinte
    BannerLay bl{};
    int bannerFirst = (int)btns.size();
    bool banner = CarouselOn() && BannerRect().bottom > TB;
    if (banner) { bl = LayoutBanner(); BannerButtons(bl); }
    if (!Hit(area, paintClip)) return;
    rt->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    if (banner) DrawBanner(rt, bl, bannerFirst);
    if (view.empty()) {
      const wchar_t* t = games.empty() ? (scanning ? L"Buscando tus juegos…" : L"No se encontraron juegos") : L"Sin resultados";
      DrawText(rt, t, Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), area, Z::text2, AL_CENTER);
      if (!search.empty()) DrawText(rt, L"Prueba con otro nombre o pulsa Esc para limpiar la búsqueda", Font(12.5f), { 0, (TB + Hd()) / 2 + 14, PanelX(), (TB + Hd()) / 2 + 34 }, Z::text3, AL_CENTER);
    }
    float selT = selAnim.Get();
    for (int i = 0; i < (int)view.size(); i++) {
      D2D1_RECT_F r = Card(i);
      if (r.bottom + 44 < TB) continue;
      if (r.top > Hd()) break;
      if (!Hit({ r.left - 12, r.top - 12, r.right + 12, r.bottom + 46 }, paintClip)) continue;  // fuera de la zona a repintar
      bool on = i == sel;
      float hv = HoverAmt(i);
      if (on) {  // sombra solo en la seleccionada: dos capas planas, sin trazos encadenados
        FillRound(rt, { r.left - 2, r.top + 6, r.right + 2, r.bottom + 10 }, 14, RGBA(0, 0, 0, .35f));
        FillRound(rt, { r.left - 6, r.top + 2, r.right + 6, r.bottom + 16 }, 18, RGBA(0, 0, 0, .18f));
      }
      FillRound(rt, r, 12, Z::elev);  // base mientras carga la caratula
      DrawCover(rt, view[i], r, 12, CoverPx(Cw()));
      if (hv > .01f) FillRound(rt, r, 12, RGBA(255, 255, 255, .06f * hv));  // luminosidad al pasar, sin escalar
      StrokeRound(rt, { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, 11.5f, MixC(WithA(Z::border, .9f), WithA(Z::borderHi, 1), hv), 1);
      if (on) {  // contorno naranja fino separado de la portada + marca bajo el titulo (no depende solo del color)
        float a = .35f + .65f * selT;
        StrokeRound(rt, { r.left - 3.5f, r.top - 3.5f, r.right + 3.5f, r.bottom + 3.5f }, 15, WithA(Z::brand, a), 2);
      }
      DrawText(rt, view[i].name, Font(12.5f, on ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD), { r.left, r.bottom + 9, r.right, r.bottom + 27 },
               on ? Z::text : MixC(WithA(Z::text, .86f), Z::text, hv), AL_CENTER);
      if (on) FillRound(rt, { (r.left + r.right) / 2 - 12, r.bottom + 30, (r.left + r.right) / 2 + 12, r.bottom + 33 }, 1.5f, WithA(Z::brand, .4f + .6f * selT));
      else DrawText(rt, view[i].platform, Font(10.5f), { r.left, r.bottom + 26, r.right, r.bottom + 40 }, Z::text3, AL_CENTER);
    }
    rt->PopAxisAlignedClip();
  }

  static std::wstring Ago(long long t) {
    if (t <= 0) return L"";
    long long d = (long long)time(nullptr) - t;
    if (d < 3600) return L"Hace un momento";
    if (d < 86400) return L"Hace " + std::to_wstring(d / 3600) + L" h";
    if (d < 86400 * 2) return L"Ayer";
    if (d < 86400 * 60) return L"Hace " + std::to_wstring(d / 86400) + L" días";
    return L"Hace " + std::to_wstring(d / (86400 * 30)) + L" meses";
  }

  static std::wstring Drive(const std::wstring& p) { return p.size() >= 2 && p[1] == L':' ? p.substr(0, 2) : L""; }

  void DrawPanel(ID2D1RenderTarget* rt) {
    float px = PanelX();
    D2D1_RECT_F p{ px, 0, px + Rp(), Hd() };
    if (!Hit(p, paintClip)) {  // fuera de la zona a repintar: solo se registran sus botones
      if (sel >= 0 && sel < (int)view.size()) RegisterPanelButtons(view[sel], p);
      return;
    }
    rt->FillRectangle(p, Brush(Z::panel));
    rt->FillRectangle({ p.left, 0, p.left + 1, Hd() }, Brush(Z::border));
    float L = p.left + 24, R = p.right - 24;
    if (sel < 0 || sel >= (int)view.size()) {
      float cy = Hd() / 2;
      DrawSystemIcon(rt, SI_KIND_GAMES, { p.left + Rp() / 2 - 44, cy - 110, p.left + Rp() / 2 + 44, cy - 22 }, 1, false);
      DrawText(rt, std::to_wstring(games.size()) + (games.size() == 1 ? L" juego en tu biblioteca" : L" juegos en tu biblioteca"),
               Font(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), { p.left, cy, p.right, cy + 24 }, Z::text, AL_CENTER);
      DrawText(rt, L"Selecciona un juego para ver su ficha", Font(13), { p.left, cy + 26, p.right, cy + 46 }, Z::text2, AL_CENTER);
      return;
    }
    const Game& g = view[sel];
    float fade = selAnim.Get(), dy = (1 - fade) * 8;
    // A. area visual: arte panoramico (o caratula) con degradado hacia el panel
    const float HH = 214;
    std::wstring art = !g.hero.empty() ? g.hero : !g.heroBlur.empty() ? g.heroBlur : g.cover;
    if (ID2D1Bitmap* bg = Img(art, Px(ceilf(Rp() / 40) * 40))) {
      D2D1_SIZE_F s = bg->GetSize();
      float k = std::max(Rp() / s.width, HH / s.height), bw = s.width * k, bh = s.height * k, cx = p.left + Rp() / 2;
      rt->PushAxisAlignedClip({ p.left + 1, 0, p.right, HH }, D2D1_ANTIALIAS_MODE_ALIASED);
      DrawBitmapHQ(rt, bg, { cx - bw / 2, HH / 2 - bh / 2, cx + bw / 2, HH / 2 + bh / 2 }, .85f * fade + .15f);
      rt->PopAxisAlignedClip();
    }
    ID2D1GradientStopCollection* st = nullptr;
    D2D1_GRADIENT_STOP gs[3] = { { 0, WithA(Z::panel, .15f) }, { .55f, WithA(Z::panel, .55f) }, { 1, Z::panel } };
    rt->CreateGradientStopCollection(gs, 3, &st);
    ID2D1LinearGradientBrush* lg = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, HH }), st, &lg);
    rt->FillRectangle({ p.left + 1, 0, p.right, HH }, lg);
    lg->Release(); st->Release();
    // portada superpuesta + titulo (B. informacion principal)
    D2D1_RECT_F cr{ L, 128 + dy, L + 96, 272 + dy };
    FillRound(rt, { cr.left - 2, cr.top + 6, cr.right + 2, cr.bottom + 10 }, 12, RGBA(0, 0, 0, .4f));
    FillRound(rt, cr, 10, Z::elev);
    DrawCover(rt, g, cr, 10, Px(150));
    StrokeRound(rt, { cr.left + .5f, cr.top + .5f, cr.right - .5f, cr.bottom - .5f }, 9.5f, WithA(Z::borderHi, .8f), 1);
    float tx = cr.right + 16, ty = 176 + dy;
    IDWriteTextLayout* tl = nullptr;
    g_dw->CreateTextLayout(g.name.c_str(), (UINT32)g.name.size(), Font(20, DWRITE_FONT_WEIGHT_BOLD, true), R - tx, 54, &tl);
    if (tl) {
      tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
      tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
      DWRITE_TEXT_METRICS m; tl->GetMetrics(&m);
      rt->DrawTextLayout({ tx, ty }, tl, Brush(WithA(Z::text, .4f + .6f * fade)), D2D1_DRAW_TEXT_OPTIONS_CLIP);
      ty += std::min(m.height, 54.f) + 6;
      tl->Release();
    }
    // etiqueta de plataforma + estado
    auto fs = Font(11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float pw = TextWidth(g.platform, fs) + 18;
    FillRound(rt, { tx, ty, tx + pw, ty + 20 }, 6, Z::elev);
    StrokeRound(rt, { tx + .5f, ty + .5f, tx + pw - .5f, ty + 19.5f }, 6, Z::border, 1);
    DrawText(rt, g.platform, fs, { tx, ty, tx + pw, ty + 20 }, Z::text2, AL_CENTER);
    rt->FillEllipse(D2D1::Ellipse({ tx + pw + 12, ty + 10 }, 3, 3), Brush(Z::ok));
    DrawText(rt, L"Instalado", Font(11.5f), { tx + pw + 19, ty, R, ty + 20 }, Z::text2);

    // C. accion principal
    float y = 292;
    RegisterPanelButtons(g, p);  // registra (y calcula) las zonas de los botones del panel
    DrawPlay(rt, g);
    y = playRect.bottom + 12;
    // D. acciones secundarias (compactas)
    DrawSecondary(rt, g, p, y);
    y += 44 + 20;
    // B'. datos en filas compactas (solo lo que se conoce de verdad)
    struct Row { const wchar_t* k; std::wstring v; };
    std::vector<Row> rows;
    rows.push_back({ L"Plataforma", g.platform });
    if (g.lastPlayed > 0) {
      FILETIME ft; LONGLONG ll = (g.lastPlayed + 11644473600LL) * 10000000LL;
      ft.dwLowDateTime = (DWORD)ll; ft.dwHighDateTime = (DWORD)(ll >> 32);
      FILETIME lft; SYSTEMTIME stt; FileTimeToLocalFileTime(&ft, &lft); FileTimeToSystemTime(&lft, &stt);
      rows.push_back({ L"Última ejecución", Ago(g.lastPlayed) + L" · " + FormatDate(L"d MMM yyyy", stt) });
    }
    if (g.size) { wchar_t b[32]; StrFormatByteSizeW((LONGLONG)g.size, b, 32); rows.push_back({ L"Tamaño", b }); }
    if (!Drive(g.folder).empty()) rows.push_back({ L"Ubicación", L"Disco " + Drive(g.folder) });
    for (auto& rw : rows) {
      rt->FillRectangle({ L, y, R, y + 1 }, Brush(Z::border));
      DrawText(rt, rw.k, Font(12), { L, y + 1, L + 130, y + 31 }, Z::text2);
      DrawText(rt, rw.v, Font(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), { L + 130, y + 1, R, y + 31 }, Z::text, AL_RIGHT);
      y += 31;
    }
    rt->FillRectangle({ L, y, R, y + 1 }, Brush(Z::border));
    y += 18;
    // E. resumen breve
    if (y + 40 < Hd()) {
      DrawText(rt, L"ACERCA DEL JUEGO", Font(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), { L, y, R, y + 16 }, Z::text3);
      y += 22;
      std::wstring d = g.desc.empty() ? L"Sin descripción disponible." : g.desc;
      std::wstring key = g.key + L"|" + std::to_wstring((int)(Hd() - y)) + L"|" + std::to_wstring(d.size());
      if (key != descKey) {
        DropDescLayout();
        g_dw->CreateTextLayout(d.c_str(), (UINT32)d.size(), Font(12.5f), R - L, std::min(Hd() - y - 16, 5 * 19.f), &descLayout);
        if (descLayout) {
          descLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
          descLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
          descLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
          DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0 };
          IDWriteInlineObject* ell = nullptr;
          g_dw->CreateEllipsisTrimmingSign(descLayout, &ell);
          descLayout->SetTrimming(&tr, ell);
          if (ell) ell->Release();
          descKey = key;
        }
      }
      if (descLayout) rt->DrawTextLayout({ L, y + dy }, descLayout, Brush(WithA(g.desc.empty() ? Z::text3 : Z::text2, .3f + .7f * fade)), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  // geometria del panel (boton Jugar y acciones): se registra aunque no se repinte esa zona
  struct SecAct { const wchar_t* glyph; const wchar_t* label; std::function<void()> f; };
  std::vector<SecAct> SecActions(const Game& g) {
    Game copy = g;
    std::vector<SecAct> v;
    if (!g.folder.empty()) v.push_back({ L"", L"Carpeta", [copy] { RunShell(copy.folder); } });
    if (!g.store.empty()) v.push_back({ L"", L"Tienda", [copy] { RunShell(copy.store); } });
    v.push_back({ L"", L"Más", [this, copy] { MoreMenu(copy); } });
    return v;
  }

  void RegisterPanelButtons(const Game& g, D2D1_RECT_F p) {
    float L = p.left + 24, R = p.right - 24;
    playRect = { L, 292, R, 344 };
    Game copy = g;
    AddBtn(playRect, [this, copy] { if (!(ls == LS_START && lsKey == copy.key)) Launch(copy); });
    auto acts = SecActions(g);
    float y = playRect.bottom + 12, n = (float)acts.size(), bw = (R - L - 8 * (n - 1)) / n;
    for (size_t i = 0; i < acts.size(); i++) AddBtn({ L + i * (bw + 8), y, L + i * (bw + 8) + bw, y + 44 }, acts[i].f);
  }

  void DrawPlay(ID2D1RenderTarget* rt, const Game& g) {
    int idx = (int)btns.size() - (int)SecActions(g).size() - 1;  // el boton Jugar se registro justo antes que las acciones
    float hv = BtnAmt(idx);
    bool pressed = pressBtn == idx;
    D2D1_RECT_F r = playRect;
    LaunchState st = lsKey == g.key ? ls : LS_NONE;
    D2D1_COLOR_F top, bot, fg = RGBA(255, 255, 255);
    const wchar_t* glyph = L"";
    std::wstring label = L"Jugar";
    switch (st) {
    case LS_START: {
      top = Z::brandLo; bot = Hex(0xC9461A);
      int dots = (int)((GetTickCount() - lsT0) / 450) % 4;
      label = L"Iniciando" + std::wstring(dots, L'.');
      glyph = L"";
      break;
    }
    case LS_RUN: top = WithA(Z::ok, .16f); bot = WithA(Z::ok, .16f); fg = Z::ok; label = L"En ejecución"; glyph = L""; break;
    case LS_ERR: top = WithA(Z::err, .16f); bot = WithA(Z::err, .16f); fg = Z::err; label = L"No se pudo iniciar"; glyph = L""; break;
    default:
      top = MixC(Z::brand, Z::brandHi, hv); bot = MixC(Z::brandLo, Z::brand, hv);
      if (pressed) { top = Z::brandLo; bot = Hex(0xC9461A); }
      break;
    }
    if (st == LS_NONE && hv > .01f)  // resplandor contenido, solo en hover
      FillRound(rt, { r.left - 3, r.top - 1, r.right + 3, r.bottom + 5 }, 14, WithA(Z::brand, .18f * hv));
    ID2D1GradientStopCollection* gsc = nullptr;
    D2D1_GRADIENT_STOP gs[2] = { { 0, top }, { 1, bot } };
    rt->CreateGradientStopCollection(gs, 2, &gsc);
    ID2D1LinearGradientBrush* lg = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gsc, &lg);
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), lg);
    lg->Release(); gsc->Release();
    if (st == LS_RUN || st == LS_ERR) StrokeRound(rt, { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, 11.5f, WithA(fg, .6f), 1);
    else StrokeRound(rt, { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, 11.5f, RGBA(255, 255, 255, .14f), 1);
    auto f = Font(16, DWRITE_FONT_WEIGHT_BOLD);
    float tw = TextWidth(label, f), cx = (r.left + r.right) / 2;
    DrawSymbol(rt, glyph, { cx - tw / 2 - 30, r.top, cx - tw / 2 - 6, r.bottom }, 15, fg);
    DrawText(rt, label, f, { cx - tw / 2, r.top, cx + tw / 2 + 4, r.bottom }, fg);
  }

  void DrawSecondary(ID2D1RenderTarget* rt, const Game& g, D2D1_RECT_F p, float y) {
    auto acts = SecActions(g);
    int first = (int)btns.size() - (int)acts.size();
    for (size_t i = 0; i < acts.size(); i++) {
      D2D1_RECT_F r = btns[first + i].r;
      float hv = BtnAmt(first + (int)i);
      FillRound(rt, r, 10, MixC(Z::elev, Hex(0x2A2E35), hv));
      StrokeRound(rt, { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f }, 9.5f, MixC(Z::border, Z::borderHi, hv), 1);
      auto f = Font(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
      float tw = TextWidth(acts[i].label, f), cx = (r.left + r.right) / 2;
      DrawSymbol(rt, acts[i].glyph, { cx - tw / 2 - 22, r.top, cx - tw / 2 - 4, r.bottom }, 12, MixC(Z::text2, Z::text, hv));
      DrawText(rt, acts[i].label, f, { cx - tw / 2, r.top, cx + tw / 2 + 4, r.bottom }, MixC(Z::text2, Z::text, hv));
    }
    (void)p; (void)y;
  }

  // acciones de gestion: desinstalar (en la tienda) es distinto de quitarlo de la biblioteca
  void MoreMenu(const Game& g) {
    std::vector<MenuItem> v;
    auto add = [&](const std::wstring& t, std::function<void()> f) { MenuItem m; m.text = t; m.action = f; v.push_back(m); };
    Game copy = g;
    if (Exists(CacheDir() + L"\\" + g.key + L"_custom.jpg")) add(L"Restablecer carátula original", [this, copy] { ResetCover(copy); });
    add(L"Cambiar carátula…", [this, copy] { ChooseCover(copy); });
    add(Exists(TrailerFile(g.key)) ? L"Cambiar video…" : L"Elegir video…", [this, copy] { ChooseVideo(copy); });
    add(L"Ocultar de la biblioteca (no borra el juego)", [this, copy] { Hide(copy); });
    if (g.platform == L"Otros") add(L"Editar nombre…", [this, copy] {  // el nombre es lo que se busca en internet
      std::wstring n = InputBox(L"Zyntra", L"Nombre del juego (se usa para buscar carátula, información y video):", copy.name);
      while (!n.empty() && iswspace(n.back())) n.pop_back();
      if (n.empty() || n == copy.name) return;
      WritePrivateProfileStringW(L"Games", (L"customname_" + copy.key).c_str(), n.c_str(), IniPath().c_str());
      Refresh();
    });
    if (g.platform == L"Otros") add(L"Quitar de la biblioteca", [this, copy] {
      auto c = IniList(L"custom");
      c.erase(std::remove_if(c.begin(), c.end(), [&](const std::wstring& p) { return L"custom_" + SafeKey(Lower(p)) == copy.key; }), c.end());
      IniSaveList(L"custom", c);
      Rescan();
    });
    if (!g.uninstall.empty()) {
      v.push_back(MenuItem::Sep());
      add(L"Desinstalar de " + g.platform + L"…", [copy] {
        std::wstring msg = L"Se abrirá el desinstalador de " + copy.platform + L" para «" + copy.name + L"».\nEsto elimina el juego del disco. ¿Continuar?";
        if (ConfirmBox(msg.c_str())) RunShell(copy.uninstall);
      });
    }
    POINT pt; GetCursorPos(&pt);
    ShowMenu(v, pt.x, pt.y);
  }

  // ------------------------------------------------------------ menu contextual de la tarjeta
  void CardMenu(int i, POINT pt) {
    Game g = view[i];
    std::vector<MenuItem> v;
    auto add = [&](const std::wstring& t, std::function<void()> f) { MenuItem m; m.text = t; m.action = f; v.push_back(m); };
    add(L"Jugar", [this, g] { Launch(g); });
    if (!g.folder.empty()) add(L"Abrir carpeta del juego", [g] { RunShell(g.folder); });
    if (!g.store.empty()) add(L"Ver en la tienda", [g] { RunShell(g.store); });
    add(L"Cambiar carátula…", [this, g] { ChooseCover(g); });
    v.push_back(MenuItem::Sep());
    add(L"Ocultar de la biblioteca", [this, g] { Hide(g); });
    if (!g.uninstall.empty()) add(L"Desinstalar de " + g.platform + L"…", [g] {
      std::wstring msg = L"Se abrirá el desinstalador de " + g.platform + L" para «" + g.name + L"».\nEsto elimina el juego del disco. ¿Continuar?";
      if (ConfirmBox(msg.c_str())) RunShell(g.uninstall);
    });
    ShowMenu(v, pt.x, pt.y);
  }

  // ------------------------------------------------------------ mensajes
  void SetSearchActive(bool on) {
    if (on == searchActive) return;
    searchActive = on;
    caret = true;
    if (on) SetTimer(hwnd, 3, 530, nullptr); else if (search.empty()) KillTimer(hwnd, 3);
    InvalidateDip(searchRect, 4);
  }

  LRESULT Proc(UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_NCCALCSIZE:
      if (wp) {
        if (IsZoomed(hwnd)) {
          auto p = (NCCALCSIZE_PARAMS*)lp;
          int f = GetSystemMetricsForDpi(SM_CXFRAME, GetDpiForWindow(hwnd)) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(hwnd));
          p->rgrc[0].left += f; p->rgrc[0].top += f; p->rgrc[0].right -= f; p->rgrc[0].bottom -= f;
        }
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
      ScreenToClient(hwnd, &pt);
      float x = Dip(pt.x), y = Dip(pt.y), b = 6;
      if (!IsZoomed(hwnd)) {
        bool l = x < b, r = x > Wd() - b, t = y < b, bo = y > Hd() - b;
        if (t && l) return HTTOPLEFT; if (t && r) return HTTOPRIGHT; if (bo && l) return HTBOTTOMLEFT; if (bo && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT; if (r) return HTRIGHT; if (t) return HTTOP; if (bo) return HTBOTTOM;
      }
      if (y < TB) {
        for (int k = 0; k < 3; k++) if (In(TL(k), x, y)) return HTCLIENT;
        for (auto& bt : btns) if (In(bt.r, x, y)) return HTCLIENT;
        if (In(searchRect, x, y)) return HTCLIENT;
        if (x < PanelX()) return HTCAPTION;
      }
      return HTCLIENT;
    }
    case WM_SIZE:
      if (wp == SIZE_MINIMIZED) {  // minimizada: se sueltan video e imagenes (se vuelven a cargar al restaurar)
        player.reset();
        if (videoBmp) { videoBmp->Release(); videoBmp = nullptr; }
        ReleaseImages();
        TrimMemory();
        return 0;
      }
      w = LOWORD(lp); h = HIWORD(lp); ClampScroll(); DropDescLayout(); UpdateVideo(); Invalidate(); return 0;
    case WM_GETMINMAXINFO: { auto mm = (MINMAXINFO*)lp; mm->ptMinTrackSize = { Px(820), Px(640) }; return 0; }
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GAMES_LIST: {
      auto list = (std::vector<Game>*)lp;
      SetGames(std::move(*list), wp == 1);
      delete list;
      if (wp == 1) {
        if (pendingRescan) { pendingRescan = false; Rescan(); }
        else if (refreshing) { refreshing = false; InvalidateDip({ 0, 0, Wd(), TB }, 0); }
      }
      return 0;
    }
    case WM_TRAILER_READY: {
      auto key = (std::wstring*)lp;
      if (const Game* g = SlideGame(slide); (g && g->key == *key) || ficha == *key) { UpdateVideo(); InvalidateDip(videoRect, 2); }
      delete key;
      return 0;
    }
    case WM_VIDEO_FRAME: OnVideoFrame(); return 0;
    case WM_FICHA_DATA: {
      auto key = (std::wstring*)lp;
      if (*key == ficha) { LoadFichaData(); Invalidate(); }
      delete key;
      return 0;
    }
    case WM_TIMER:
      switch (wp) {
      case 1: {  // desplazamiento suave
        float d = scrollTarget - scroll;
        if (fabsf(d) < .5f) { scroll = scrollTarget; KillTimer(hwnd, 1); UpdateVideo(); } else scroll += d * .3f;
        InvalidateGrid();
        break;
      }
      case 8:  // siguiente diapositiva: solo con la ventana al frente y sin el raton encima
        if (!bannerHover && !IsIconic(hwnd) && GetForegroundWindow() == hwnd && CarouselOn()) GoSlide(slide + 1);
        break;
      case 2: if (GetForegroundWindow() == hwnd) Rescan(); break;  // instalaciones/desinstalaciones (al volver al frente tambien se revisa)
      case 3:
        if (!searchActive && search.empty()) { KillTimer(hwnd, 3); break; }
        caret = !caret; InvalidateDip(searchRect, 2);
        break;
      case 4: KillTimer(hwnd, 4); Invalidate(); break;  // seguir decodificando caratulas
      case 5: LaunchTick(); break;
      case 6: AnimTick(); break;
      case 10:  // animacion entre las secciones de la ficha
        Invalidate();
        if (!fichaPage.Active()) KillTimer(hwnd, 10);
        break;
      case 7: if (ls == LS_START) InvalidateDip(playRect, 6); else KillTimer(hwnd, 7); break;
      }
      return 0;
    case WM_MOUSEWHEEL:
      if (!ficha.empty()) { if (fichaBig.empty()) FichaPage(GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1 : 0); return 0; }
      scrollTarget -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 120;
      ClampScroll();
      SetTimer(hwnd, 1, 15, nullptr);
      return 0;
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, hwnd };
      TrackMouseEvent(&t);
      float x = Dip(GET_X_LPARAM(lp)), y = Dip(GET_Y_LPARAM(lp));
      int hb = -1;
      for (size_t i = 0; i < btns.size(); i++) if (btns[i].act && In(btns[i].r, x, y)) hb = (int)i;
      SetHover(hb >= 0 || !ficha.empty() ? -1 : CardAt(x, y));
      SetHotBtn(hb);
      if (!ficha.empty()) return 0;
      bool tl = y < 42 && x < 72;
      if (tl != hoverTL) { hoverTL = tl; InvalidateDip({ 0, 0, 80, 44 }, 0); }
      bool bh = CarouselOn() && In(BannerRect(), x, y) && y > TB;
      if (bh != bannerHover) { bannerHover = bh; InvalidateDip(BannerRect(), 4); }  // flechas
      return 0;
    }
    case WM_MOUSELEAVE:
      SetHover(-1); SetHotBtn(-1);
      if (hoverTL) { hoverTL = false; InvalidateDip({ 0, 0, 80, 44 }, 0); }
      if (bannerHover) { bannerHover = false; InvalidateDip(BannerRect(), 4); }
      return 0;
    case WM_LBUTTONDOWN: {
      CloseMenu();
      SetFocus(hwnd);
      float x = Dip(GET_X_LPARAM(lp)), y = Dip(GET_Y_LPARAM(lp));
      SetSearchActive(In(searchRect, x, y));
      for (size_t i = 0; i < btns.size(); i++)
        if (btns[i].act && In(btns[i].r, x, y)) { pressBtn = (int)i; pressRect = btns[i].r; SetCapture(hwnd); InvalidateBtn(pressBtn); return 0; }
      if (!ficha.empty()) return 0;
      int i = CardAt(x, y);
      if (i >= 0) Select(i);
      return 0;
    }
    case WM_LBUTTONUP: {
      float x = Dip(GET_X_LPARAM(lp)), y = Dip(GET_Y_LPARAM(lp));
      if (pressBtn >= 0) {  // el boton actua al soltar, como los botones nativos
        int b = pressBtn;
        pressBtn = -1;
        ReleaseCapture();
        InvalidateBtn(b);
        // la lista de botones se rehace en cada repintado (el video repinta 30 veces por segundo): se busca
        // el mismo boton por su rectangulo, no por su posicion en la lista
        if (In(pressRect, x, y))
          for (auto& bt : btns)
            if (bt.act && bt.r.left == pressRect.left && bt.r.top == pressRect.top && bt.r.right == pressRect.right && bt.r.bottom == pressRect.bottom) { auto f = bt.act; f(); break; }
        return 0;
      }
      if (!ficha.empty()) return 0;
      if (In(TL(0), x, y)) DestroyWindow(hwnd);
      else if (In(TL(1), x, y)) ShowWindow(hwnd, SW_MINIMIZE);
      else if (In(TL(2), x, y)) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
      return 0;
    }
    case WM_LBUTTONDBLCLK: {  // doble clic en la caratula: jugar
      if (!ficha.empty()) return 0;
      int i = CardAt(Dip(GET_X_LPARAM(lp)), Dip(GET_Y_LPARAM(lp)));
      if (i >= 0) { Select(i); Launch(view[i]); }
      return 0;
    }
    case WM_RBUTTONUP: {
      if (!ficha.empty()) return 0;
      int i = CardAt(Dip(GET_X_LPARAM(lp)), Dip(GET_Y_LPARAM(lp)));
      if (i >= 0) { Select(i); POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; ClientToScreen(hwnd, &pt); CardMenu(i, pt); }
      return 0;
    }
    case WM_KEYDOWN: {
      int c = Cols(), n = (int)view.size();
      if (!ficha.empty()) {  // ficha abierta: Esc cierra, Enter juega
        if (wp == VK_ESCAPE) { if (!fichaBig.empty()) { fichaBig.clear(); Invalidate(); } else CloseFicha(); }
        else if (wp == VK_DOWN || wp == VK_NEXT) FichaPage(1);
        else if (wp == VK_UP || wp == VK_PRIOR) FichaPage(0);
        else if (wp == VK_RETURN) { if (const Game* fg = FindGame(ficha)) { Game c = *fg; Launch(c); } }
        return 0;
      }
      if (wp == VK_ESCAPE) { search.clear(); SetSearchActive(false); Filter(); Invalidate(); return 0; }
      if (!n) break;
      int s = sel < 0 ? 0 : sel;
      switch (wp) {
      case VK_RIGHT: s = std::min(n - 1, s + 1); break;
      case VK_LEFT: s = std::max(0, s - 1); break;
      case VK_DOWN: s = std::min(n - 1, s + c); break;
      case VK_UP: s = std::max(0, s - c); break;
      case VK_RETURN: if (sel >= 0) Launch(view[sel]); return 0;
      default: return 0;
      }
      Select(s);
      // mantener la seleccion a la vista
      D2D1_RECT_F r = Card(sel);
      if (r.top < TB + 10) scrollTarget -= TB + 10 - r.top;
      else if (r.bottom + 46 > Hd()) scrollTarget += r.bottom + 46 - Hd();
      ClampScroll();
      SetTimer(hwnd, 1, 15, nullptr);
      return 0;
    }
    case WM_CHAR:  // escribir filtra por nombre
      if (wp == 8) { if (!search.empty()) search.pop_back(); }
      else if (wp >= 32) search.push_back((wchar_t)wp);
      else return 0;
      SetSearchActive(true);
      scroll = scrollTarget = 0;
      Filter(); Invalidate();
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wp) != WA_INACTIVE) Rescan();
      else { SetSearchActive(false); KillTimer(hwnd, 3); }
      PostMessageW(hwnd, WM_APP + 83, 0, 0);  // el foco ya cambio cuando llega: reanudar/pausar video
      break;
    case WM_APP + 83: UpdateVideo(); return 0;
    case WM_DESTROY:
      KillTimer(hwnd, 8);
      player.reset();  // detiene y espera al hilo de video
      if (videoBmp) { videoBmp->Release(); videoBmp = nullptr; }
      ReleaseImages();
      DropDescLayout();
      ReleaseIcons(Px(96));
      TrimMemory();
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      delete this;
      extern GameMode* g_gameMode;
      g_gameMode = nullptr;
      PostQuitMessage(0);  // proceso propio: al cerrar la ventana termina y libera toda su memoria
      return 0;
    }
    return DefWindowProcW(hwnd, m, wp, lp);
  }

  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) {
      auto f = (GameMode*)((CREATESTRUCTW*)lp)->lpCreateParams;
      f->hwnd = h;
      SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)f);
    }
    auto f = (GameMode*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return f ? f->Proc(m, wp, lp) : DefWindowProcW(h, m, wp, lp);
  }

  void Create() {
    static bool reg = false;
    if (!reg) {
      WNDCLASSW wc{};
      wc.lpfnWndProc = WndProc; wc.hInstance = g_inst; wc.lpszClassName = kGameClass;
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.style = CS_DBLCLKS;
      RegisterClassW(&wc);
      reg = true;
    }
    panelOpen = GetPrivateProfileIntW(L"Games", L"panel", 1, IniPath().c_str()) != 0;
    trailers = GetPrivateProfileIntW(L"Games", L"trailers", 1, IniPath().c_str()) != 0;
    panelAnim.Set(panelOpen ? 1.f : 0.f);
    selAnim.Set(1);
    slideFade.Set(1);  // la primera diapositiva se ve completa desde el inicio (sin fundido)
    RECT mon = MonitorRect();
    int ww = std::min(Px(1460), (int)(mon.right - mon.left) - Px(80)), hh = std::min(Px(880), (int)(mon.bottom - mon.top) - Px(120));
    hwnd = CreateWindowExW(0, kGameClass, L"Zyntra", WS_OVERLAPPEDWINDOW, (mon.left + mon.right - ww) / 2, (mon.top + mon.bottom - hh) / 2,
                           ww, hh, nullptr, nullptr, g_inst, this);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);
    MARGINS mg{ -1 };
    DwmExtendFrameIntoClientArea(hwnd, &mg);
    ApplyGlass(hwnd, true);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    // mismo AppUserModelID que el acceso del menu Inicio: el dock la agrupa con su icono
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&ps)))) {
      PROPVARIANT pv;
      if (SUCCEEDED(InitPropVariantFromString(kAumid, &pv))) { ps->SetValue(PKEY_AppUserModel_ID, pv); PropVariantClear(&pv); }
      ps->Release();
    }
    // icono incrustado en el exe (resource.rc)
    if (HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0))
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (HICON sm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0))
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetTimer(hwnd, 2, 20000, nullptr);
    Rescan();
  }
};

GameMode* g_gameMode = nullptr;

void GameMode_RunStandalone() {
  SetCurrentProcessExplicitAppUserModelID(kAumid);
  g_gameMode = new GameMode;
  g_gameMode->Create();
}

void GameMode_Invalidate() { if (g_gameMode) g_gameMode->Invalidate(); }

// ¿el programa pertenece a un juego de la biblioteca? (sus carpetas, en minusculas)
bool GameMode_IsGameExe(const std::wstring& exe) {
  if (exe.empty()) return false;
  std::wstring e = Lower(exe);
  std::lock_guard<std::mutex> lk(s_trackMx);
  for (auto& t : s_track)
    for (auto& p : t.second)
      if (p.size() > 3 && e.rfind(p + L"\\", 0) == 0) return true;
  return false;
}
