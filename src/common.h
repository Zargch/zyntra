// Zyntra - launcher de juegos para Windows. Creado por Zargch (https://github.com/Zargch). Licencia MIT.
// Nacio como el modo de juegos de GlassShell.
// Todo el render es Direct2D por software sobre un unico render target compartido,
// para no cargar el driver de la GPU y mantener la RAM al minimo.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <algorithm>
#include <memory>
#include <mutex>
#include <cmath>

// ---------------------------------------------------------------- mensajes propios
enum : UINT {
  WM_APP_RENDER = WM_APP + 1,   // render diferido de una superficie
  WM_APP_ICONREADY,             // un icono termino de cargarse en segundo plano
  WM_APP_CATALOG,               // catalogo de apps listo
  WM_APP_TRAY,                  // icono de bandeja
  WM_APP_WEATHER,               // datos del clima listos
  WM_APP_MEDIA,                 // cambio en la musica que suena
  WM_APP_VOLUME,                // cambio de volumen (wParam = 0..100, lParam = silencio)
};

enum HotkeyId { HK_SPOTLIGHT = 1, HK_LAUNCHPAD, HK_QUIT };

// ---------------------------------------------------------------- globales
extern HINSTANCE g_inst;
extern ID2D1Factory* g_d2d;
extern IDWriteFactory* g_dw;
extern IWICImagingFactory* g_wic;
extern ID2D1DCRenderTarget* g_rt;
extern ID2D1DeviceContext* g_dc;  // misma superficie que g_rt; da filtros de alta calidad y capas
extern float g_scale;           // px por DIP
extern HWND g_msgWnd;           // ventana oculta para mensajes globales
extern std::wstring g_dir;      // carpeta de datos: %LOCALAPPDATA%\Zyntra (config.ini, cache)

struct Theme {
  bool dark;
  D2D1_COLOR_F text, textDim, tint, rim, accent, hover, sep, pill;
};
extern Theme g_th;
void LoadTheme();

struct Config {
  std::wstring font = L"Segoe UI Variable Text";
  std::wstring fontDisplay = L"Segoe UI Variable Display";
  int glassOpacity = 35;        // 0 = vidrio transparente, 100 = teñido (como el slider de Golden Gate)
  bool menubar = true;
  bool menubarAutohide = true;   // barra superior e isla se esconden mientras se usa una app (aparecen con el raton arriba)
  bool dock = true;
  int dockIcon = 52;
  float dockMagnify = 1.7f;     // 1.0 = sin magnificacion
  bool dockAutohide = false;     // ocultar siempre
  bool dockHideWithApps = true;  // ocultar mientras se usa una app (aparece con el raton abajo)
  bool dockBackground = false;   // panel de vidrio detras de los iconos
  bool hideTaskbar = true;
  bool squircle = true;         // encerrar iconos de Windows en un squircle tipo macOS
  bool syncTaskbar = true;      // el dock copia los anclados de la barra de tareas
  bool finder = true;           // el Explorador del dock abre el Finder propio
  bool hideOnFullscreen = true;
  std::vector<std::wstring> pinned;
  std::wstring hkSpotlight = L"Alt+Space";
  std::wstring hkLaunchpad = L"Alt+F1";
  bool wClock = false, wCalendar = false, wWeather = false, wSystem = false;  // reemplazados por la isla
  bool island = true;
  std::wstring weatherCity;
  double lat = 0, lon = 0; bool hasLoc = false;
  std::map<std::wstring, POINT> widgetPos;
};
extern Config g_cfg;
void LoadConfig();
void SaveConfig();
void SaveDockPins();
void SaveWidgetPos(const std::wstring& name, POINT p);

// ---------------------------------------------------------------- utilidades
inline D2D1_COLOR_F RGBA(float r, float g, float b, float a = 1.f) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
inline D2D1_COLOR_F WithA(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
inline int Px(float dip) { return (int)lroundf(dip * g_scale); }
inline float Dip(int px) { return px / g_scale; }
inline float Clamp(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
std::wstring Lower(std::wstring s);
bool IContains(const std::wstring& hay, const std::wstring& needle);
RECT MonitorRect();             // monitor principal, px
void RunShell(const std::wstring& target, const std::wstring& args = L"", const wchar_t* verb = L"open");
void SendKeys(std::initializer_list<WORD> keys);  // pulsa en orden y suelta en orden inverso
void TrimMemory();
bool IsOwnWindow(HWND h);
void ApplyGlass(HWND h, bool on);
std::wstring FormatDate(const wchar_t* fmt, const SYSTEMTIME& st);

// ---------------------------------------------------------------- dibujo
IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, bool display = false);
ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
float TextWidth(const std::wstring& s, IDWriteTextFormat* f);
enum Align { AL_LEFT, AL_CENTER, AL_RIGHT };
void DrawText(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c,
              Align a = AL_LEFT, bool vcenter = true);
void FillRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, D2D1_COLOR_F c);
void StrokeRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1.f);
void DrawGlass(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float tintBoost = 0.f);  // tinte + borde de luz
void DrawShadow(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float size, float alpha);
ID2D1PathGeometry* MakePath(std::function<void(ID2D1GeometrySink*)> build, bool filled = true);
void DrawSymbol(ID2D1RenderTarget* rt, const wchar_t* glyph, D2D1_RECT_F r, float size, D2D1_COLOR_F c);
void DrawBitmapHQ(ID2D1RenderTarget* rt, ID2D1Bitmap* b, D2D1_RECT_F dst, float alpha = 1.f, const D2D1_RECT_F* src = nullptr);
void PushRoundClip(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad);
void PopClip(ID2D1RenderTarget* rt);
void DrawFolderIcon(ID2D1RenderTarget* rt, D2D1_RECT_F r, float alpha = 1.f, const wchar_t* glyph = nullptr);
// fondo de pantalla desenfocado (para widgets: vidrio con esquinas redondas perfectas)
ID2D1Bitmap* WallpaperBlur(float* scaleOut);
void WallpaperChanged();

// ---------------------------------------------------------------- superficies
// Una superficie es una ventana popup. Con "glass" usa el desenfoque de DWM detras del
// contenido; sin glass es una ventana layered con alfa por pixel (para el dock).
class Surface {
public:
  HWND hwnd = nullptr;
  int x = 0, y = 0, w = 0, h = 0;   // px
  bool layered = false;
  float radius = 0;                 // DIPs; >0 recorta la ventana con esquinas redondeadas
  bool dwmRound = false;            // usa las esquinas redondeadas (con antialias y sombra) de Windows 11
  virtual ~Surface();
  void Create(DWORD exStyle, bool layered, bool glass);
  void SetBounds(int x, int y, int w, int h, HWND after = nullptr);
  void Invalidate();
  void Show(bool show, bool activate = false);
  bool Visible() const { return hwnd && IsWindowVisible(hwnd); }
  float Wd() const { return Dip(w); }
  float Hd() const { return Dip(h); }
  virtual void Paint(ID2D1RenderTarget* rt) = 0;
  virtual LRESULT Handle(UINT m, WPARAM wp, LPARAM lp);
  void RenderNow();
  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
protected:
  bool pending = false;
  HDC memDC = nullptr; HBITMAP dib = nullptr; HGDIOBJ oldBmp = nullptr; int dibW = 0, dibH = 0;
  void ApplyShape();
};

// ---------------------------------------------------------------- apps
struct AppInfo {
  std::wstring name;     // nombre visible
  std::wstring id;       // nombre de analisis dentro de shell:AppsFolder
  std::wstring exe;      // ruta del exe en minusculas (si se conoce)
  std::wstring aumid;    // AppUserModelID en minusculas (si se conoce)
};
extern std::vector<AppInfo> g_apps;
extern bool g_catalogReady;
void StartCatalog();
const AppInfo* FindApp(const std::wstring& id);
const AppInfo* MatchApp(const std::wstring& aumid, const std::wstring& exe);
void LaunchApp(const AppInfo& a, bool admin = false);
std::vector<const AppInfo*> SearchApps(const std::wstring& q);

struct RunningWin { HWND hwnd; std::wstring key, exe, aumid, title; };
std::vector<RunningWin> EnumRunning();
std::wstring ExeOfWindow(HWND h);
std::wstring AumidOfWindow(HWND h);
std::wstring AppNameForWindow(HWND h);
void ActivateWindow(HWND h);

// iconos: se cargan en un hilo aparte; devuelve nullptr mientras no estan listos
ID2D1Bitmap* GetIcon(const std::wstring& shellPath, const std::wstring& name, int px, bool* custom = nullptr);
void ReleaseIcons(int px);
void DrawAppIcon(ID2D1RenderTarget* rt, const std::wstring& shellPath, const std::wstring& name, D2D1_RECT_F r, int loadPx, float alpha = 1.f, bool pressed = false);
int SystemIconKind(const std::wstring& path, const std::wstring& name);
bool DrawSystemIcon(ID2D1RenderTarget* rt, int kind, D2D1_RECT_F r, float alpha, bool pressed);
bool WriteSystemIco(int kind, const std::wstring& file);
enum { SI_KIND_CALENDAR = 8, SI_KIND_ZARGCH = 20, SI_KIND_GAMES = 21 };
DWORD DragFilesOut(const std::vector<std::wstring>& paths, DWORD allowed);  // arrastrar y soltar hacia otras ventanas
std::vector<std::wstring> DroppedFiles(IDataObject* d);                     // rutas de un CF_HDROP
void GameMode_Open();            // en el escritorio: lanza el proceso de Zargch Game
void GameMode_RunStandalone();   // dentro del proceso de Zargch Game: crea la ventana
void GameMode_Invalidate();
bool IsGameModeWindow(HWND h);
bool GameMode_IsOpen();
void GameMode_MigratePin();
void GameMode_StartTracking();  // escaneo inicial para saber que juegos estan abiertos
void GameMode_Track();          // anota la hora si hay un juego abierto
bool GameMode_IsGameExe(const std::wstring& exe);  // el exe esta en la carpeta de un juego conocido
void GameMode_EnsureShortcut();
std::string HttpGet(const wchar_t* host, const std::wstring& path);
std::wstring Utf8(const std::string& s);
inline void DrawZargchLogo(ID2D1RenderTarget* rt, D2D1_RECT_F r, float alpha = 1.f, bool pressed = false) { DrawSystemIcon(rt, 20, r, alpha, pressed); }
std::vector<std::wstring> TaskbarPins();  // ids del catalogo anclados en la barra de tareas
void Finder_Open(const std::wstring& path);  // ruta vacia: trae al frente un Finder o abre la carpeta personal
int FinderWindows();
void Finder_Invalidate();
bool IsFinderWindow(HWND h);
std::wstring KnownPath(REFKNOWNFOLDERID id);
std::wstring DownloadsPath();  // Descargas de GlassShell ([Finder] downloads o la de Windows)
std::wstring InputBox(const wchar_t* title, const wchar_t* prompt, const std::wstring& def);
void DrawFinderIcon(ID2D1RenderTarget* rt, D2D1_RECT_F r, float alpha = 1.f);
std::wstring AppShellPath(const AppInfo& a);

// ---------------------------------------------------------------- menus emergentes
struct MenuItem {
  std::wstring text, shortcut;
  std::function<void()> action;
  bool sep = false, disabled = false, checked = false, header = false;
  static MenuItem Sep() { MenuItem m; m.sep = true; return m; }
};
void ShowMenu(std::vector<MenuItem> items, int x, int y, bool above = false, std::function<void()> onClose = nullptr);
void CloseMenu();
bool MenuOpen();
HWND MenuHwnd();

// ---------------------------------------------------------------- componentes
void MenuBar_Create();  void MenuBar_Update(); void MenuBar_Show(bool); int MenuBar_Height();
void Dock_Create();     void Dock_Update(bool relayout); void Dock_Show(bool);
void Dock_SetAppActive(bool active);
void Dock_Pin(const std::wstring& id, bool pin); bool Dock_IsPinned(const std::wstring& id);
void Launchpad_Toggle(); bool Launchpad_Visible();
void Spotlight_Toggle();
void Widgets_Create();  void Widgets_Tick(); void Widgets_Show(bool); void Widgets_Rebuild();
void Widgets_WeatherReady();
void ControlCenter_Toggle(int anchorX);

// ---------------------------------------------------------------- musica, clima e isla dinamica
struct MediaSnap {
  bool has = false, playing = false, trackChanged = false;
  std::wstring title, artist, app, aumid;  // aumid: app de origen (Spotify.exe, Chrome, una PWA...)
  double pos = 0, dur = 0;
  DWORD posTick = 0;
  std::vector<BYTE> art;  // caratula PBGRA artPx x artPx
  int artPx = 0, version = 0;
  D2D1_COLOR_F color{ .5f, .5f, .5f, 1 };
};
void Media_Start();
MediaSnap Media_Get();
void Media_FocusSource();  // trae al frente la app o pestaña del navegador que esta sonando

struct WeatherInfo { bool ok = false; std::wstring city, err; double temp = 0, tmax = 0, tmin = 0; int code = 0; bool day = true; };
bool Weather_Info(WeatherInfo& out);
void Weather_Fetch();
void Weather_AskCity();
void Weather_Icon(ID2D1RenderTarget* rt, int code, bool day, float cx, float cy, float s);
const wchar_t* Weather_Text(int code);

void Island_Create();
void Island_Show(bool show);
void Island_OnMedia();
void Island_OnVolume(int level, bool muted);
void Island_Invalidate();
bool Island_Busy();
void TopBars_Refresh();
void OpenCalendar();
void OnForegroundChanged(HWND h);
void InvalidateAll();
void Menu_OnMessage(WPARAM what);
extern HWND g_fg;   // ultima ventana de otra app que tuvo el foco
bool ConfirmBox(const wchar_t* text);
bool Intro_Play(std::function<void()> done);  // animacion de inicio; false si no se reproduce (done no se llama)
void ShowAbout(HWND owner);  // version, creditos a Zargch y licencia
void LogoGlyph(ID2D1RenderTarget* rt, float x, float y, float size, D2D1_COLOR_F c);
