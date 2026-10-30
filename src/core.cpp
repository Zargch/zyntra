#include "common.h"

HINSTANCE g_inst;
ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
IWICImagingFactory* g_wic;
ID2D1DCRenderTarget* g_rt;
ID2D1DeviceContext* g_dc;
float g_scale = 1.f;
HWND g_msgWnd;
std::wstring g_dir;
Theme g_th;
Config g_cfg;

// ================================================================ tema
void LoadTheme() {
  DWORD v = 1, sz = sizeof v;
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
  float o = Clamp(g_cfg.glassOpacity / 100.f, 0.f, 1.f);
  Theme& t = g_th;
  t.dark = v == 0;
  if (t.dark) {
    t.text = RGBA(255, 255, 255, .92f); t.textDim = RGBA(255, 255, 255, .55f);
    t.tint = RGBA(28, 28, 30, .18f + .62f * o); t.rim = RGBA(255, 255, 255, .22f);
    t.accent = RGBA(10, 132, 255); t.hover = RGBA(255, 255, 255, .12f);
    t.sep = RGBA(255, 255, 255, .14f); t.pill = RGBA(44, 44, 46, .92f);
  } else {
    t.text = RGBA(0, 0, 0, .85f); t.textDim = RGBA(0, 0, 0, .5f);
    t.tint = RGBA(255, 255, 255, .12f + .6f * o); t.rim = RGBA(255, 255, 255, .65f);
    t.accent = RGBA(0, 122, 255); t.hover = RGBA(0, 0, 0, .07f);
    t.sep = RGBA(0, 0, 0, .11f); t.pill = RGBA(246, 246, 246, .94f);
  }
}

// ================================================================ configuracion (INI junto al exe)
static std::wstring IniPath() { return g_dir + L"\\config.ini"; }
static std::wstring IniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(sec, key, def, buf, 2048, IniPath().c_str());
  return buf;
}
static int IniInt(const wchar_t* sec, const wchar_t* key, int def) {
  return GetPrivateProfileIntW(sec, key, def, IniPath().c_str());
}
static void IniSet(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
  WritePrivateProfileStringW(sec, key, v.c_str(), IniPath().c_str());
}

void LoadConfig() {
  bool fresh = GetFileAttributesW(IniPath().c_str()) == INVALID_FILE_ATTRIBUTES;
  Config& c = g_cfg;
  c.font = IniStr(L"General", L"font", c.font.c_str());
  c.fontDisplay = IniStr(L"General", L"font_display", c.fontDisplay.c_str());
  c.glassOpacity = IniInt(L"General", L"glass_opacity", c.glassOpacity);
  c.hideTaskbar = IniInt(L"General", L"hide_taskbar", c.hideTaskbar);
  c.hideOnFullscreen = IniInt(L"General", L"hide_on_fullscreen", c.hideOnFullscreen);
  c.hkSpotlight = IniStr(L"General", L"hotkey_spotlight", c.hkSpotlight.c_str());
  c.hkLaunchpad = IniStr(L"General", L"hotkey_launchpad", c.hkLaunchpad.c_str());
  c.menubar = IniInt(L"MenuBar", L"enabled", c.menubar);
  c.menubarAutohide = IniInt(L"MenuBar", L"autohide", c.menubarAutohide);
  c.dock = IniInt(L"Dock", L"enabled", c.dock);
  c.dockIcon = std::max(32, std::min(96, IniInt(L"Dock", L"icon_size", c.dockIcon)));
  c.dockMagnify = std::max(100, std::min(250, IniInt(L"Dock", L"magnification", (int)(c.dockMagnify * 100)))) / 100.f;
  c.dockAutohide = IniInt(L"Dock", L"autohide", c.dockAutohide);
  c.dockHideWithApps = IniInt(L"Dock", L"hide_with_apps", c.dockHideWithApps);
  c.dockBackground = IniInt(L"Dock", L"background", c.dockBackground);
  c.squircle = IniInt(L"Dock", L"squircle_icons", c.squircle);
  c.syncTaskbar = IniInt(L"Dock", L"sync_taskbar", c.syncTaskbar);
  c.finder = IniInt(L"General", L"finder", c.finder);
  int n = IniInt(L"Dock", L"count", -1);
  c.pinned.clear();
  for (int i = 0; i < n; i++) {
    std::wstring v = IniStr(L"Dock", (L"pin" + std::to_wstring(i)).c_str(), L"");
    if (!v.empty()) c.pinned.push_back(v);
  }
  if (n < 0) c.pinned.push_back(L"*default*");
  c.island = IniInt(L"Island", L"enabled", c.island);
  c.wClock = IniInt(L"Widgets", L"clock", c.wClock);
  c.wCalendar = IniInt(L"Widgets", L"calendar", c.wCalendar);
  c.wWeather = IniInt(L"Widgets", L"weather", c.wWeather);
  c.wSystem = IniInt(L"Widgets", L"system", c.wSystem);
  c.weatherCity = IniStr(L"Widgets", L"weather_city", L"");
  std::wstring la = IniStr(L"Widgets", L"latitude", L""), lo = IniStr(L"Widgets", L"longitude", L"");
  c.hasLoc = !la.empty() && !lo.empty();
  if (c.hasLoc) { c.lat = _wtof(la.c_str()); c.lon = _wtof(lo.c_str()); }
  for (const wchar_t* w : { L"clock", L"calendar", L"weather", L"system" }) {
    std::wstring v = IniStr(L"WidgetPos", w, L"");
    int px, py;
    if (swscanf_s(v.c_str(), L"%d,%d", &px, &py) == 2) c.widgetPos[w] = { px, py };
  }
  if (fresh) SaveConfig();
}

void SaveConfig() {
  Config& c = g_cfg;
  IniSet(L"General", L"font", c.font);
  IniSet(L"General", L"font_display", c.fontDisplay);
  IniSet(L"General", L"glass_opacity", std::to_wstring(c.glassOpacity));
  IniSet(L"General", L"hide_taskbar", std::to_wstring(c.hideTaskbar));
  IniSet(L"General", L"hide_on_fullscreen", std::to_wstring(c.hideOnFullscreen));
  IniSet(L"General", L"hotkey_spotlight", c.hkSpotlight);
  IniSet(L"General", L"hotkey_launchpad", c.hkLaunchpad);
  IniSet(L"General", L"finder", std::to_wstring(c.finder));
  IniSet(L"MenuBar", L"enabled", std::to_wstring(c.menubar));
  IniSet(L"MenuBar", L"autohide", std::to_wstring(c.menubarAutohide));
  IniSet(L"Dock", L"enabled", std::to_wstring(c.dock));
  IniSet(L"Dock", L"icon_size", std::to_wstring(c.dockIcon));
  IniSet(L"Dock", L"magnification", std::to_wstring((int)lroundf(c.dockMagnify * 100)));
  IniSet(L"Dock", L"autohide", std::to_wstring(c.dockAutohide));
  IniSet(L"Dock", L"squircle_icons", std::to_wstring(c.squircle));
  IniSet(L"Island", L"enabled", std::to_wstring(c.island));
  IniSet(L"Widgets", L"clock", std::to_wstring(c.wClock));
  IniSet(L"Widgets", L"calendar", std::to_wstring(c.wCalendar));
  IniSet(L"Widgets", L"weather", std::to_wstring(c.wWeather));
  IniSet(L"Widgets", L"system", std::to_wstring(c.wSystem));
  IniSet(L"Widgets", L"weather_city", c.weatherCity);
}

void SaveDockPins() {
  WritePrivateProfileStringW(L"Dock", nullptr, nullptr, IniPath().c_str());  // borra la seccion
  IniSet(L"Dock", L"enabled", std::to_wstring(g_cfg.dock));
  IniSet(L"Dock", L"icon_size", std::to_wstring(g_cfg.dockIcon));
  IniSet(L"Dock", L"magnification", std::to_wstring((int)lroundf(g_cfg.dockMagnify * 100)));
  IniSet(L"Dock", L"autohide", std::to_wstring(g_cfg.dockAutohide));
  IniSet(L"Dock", L"hide_with_apps", std::to_wstring(g_cfg.dockHideWithApps));
  IniSet(L"Dock", L"background", std::to_wstring(g_cfg.dockBackground));
  IniSet(L"Dock", L"squircle_icons", std::to_wstring(g_cfg.squircle));
  IniSet(L"Dock", L"sync_taskbar", std::to_wstring(g_cfg.syncTaskbar));
  IniSet(L"Dock", L"count", std::to_wstring(g_cfg.pinned.size()));
  for (size_t i = 0; i < g_cfg.pinned.size(); i++) IniSet(L"Dock", (L"pin" + std::to_wstring(i)).c_str(), g_cfg.pinned[i]);
}

void SaveWidgetPos(const std::wstring& name, POINT p) {
  g_cfg.widgetPos[name] = p;
  IniSet(L"WidgetPos", name.c_str(), std::to_wstring(p.x) + L"," + std::to_wstring(p.y));
}

// ================================================================ utilidades
std::wstring Lower(std::wstring s) {
  CharLowerBuffW(s.data(), (DWORD)s.size());
  return s;
}

bool IContains(const std::wstring& hay, const std::wstring& needle) {
  if (needle.empty()) return true;
  return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                         hay.c_str(), (int)hay.size(), needle.c_str(), (int)needle.size(), nullptr, nullptr, nullptr, 0) >= 0;
}

RECT MonitorRect() {
  MONITORINFO mi{ sizeof mi };
  GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
  return mi.rcMonitor;
}

void RunShell(const std::wstring& target, const std::wstring& args, const wchar_t* verb) {
  ShellExecuteW(nullptr, verb, target.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
}

void SendKeys(std::initializer_list<WORD> keys) {
  std::vector<INPUT> in;
  for (WORD k : keys) { INPUT i{ INPUT_KEYBOARD }; i.ki.wVk = k; in.push_back(i); }
  for (auto it = keys.end(); it != keys.begin();) {
    --it; INPUT i{ INPUT_KEYBOARD }; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
  }
  SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void TrimMemory() {
  // el render target compartido conserva un bufer del tamaño de la ultima superficie dibujada
  // (una pantalla completa son ~15 MB): lo encogemos atandolo a un bitmap de 1x1
  BITMAPINFO bi{};
  bi.bmiHeader = { sizeof(BITMAPINFOHEADER), 1, -1, 1, 32, BI_RGB };
  void* bits;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP b = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(dc, b);
  RECT rc{ 0, 0, 1, 1 };
  g_rt->BindDC(dc, &rc);
  g_rt->BeginDraw();
  g_rt->EndDraw();
  SelectObject(dc, old);
  DeleteObject(b);
  DeleteDC(dc);
  HeapCompact(GetProcessHeap(), 0);
  SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

bool IsOwnWindow(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  return pid == GetCurrentProcessId();
}

struct ACCENT_POLICY { int AccentState, AccentFlags; DWORD GradientColor; int AnimationId; };
struct WINCOMPATTRDATA { int Attribute; PVOID Data; SIZE_T SizeOfData; };

void ApplyGlass(HWND h, bool on) {
  static auto swca = (BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*))GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
  if (!swca) return;
  ACCENT_POLICY ap{ on ? 3 : 0, 0, 0, 0 };  // 3 = blur gaussiano detras de la ventana
  WINCOMPATTRDATA d{ 19, &ap, sizeof ap };
  swca(h, &d);
}

std::wstring FormatDate(const wchar_t* fmt, const SYSTEMTIME& st) {
  wchar_t buf[128];
  GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, fmt, buf, 128, nullptr);
  return buf;
}

// ================================================================ dibujo
static bool FontExists(const std::wstring& fam) {
  static std::map<std::wstring, bool> cache;
  auto it = cache.find(fam);
  if (it != cache.end()) return it->second;
  IDWriteFontCollection* col = nullptr;
  g_dw->GetSystemFontCollection(&col);
  UINT32 idx; BOOL exists = FALSE;
  if (col) { col->FindFamilyName(fam.c_str(), &idx, &exists); col->Release(); }
  return cache[fam] = !!exists;
}

IDWriteTextFormat* Font(float size, DWRITE_FONT_WEIGHT w, bool display) {
  static std::map<std::tuple<int, int, bool>, IDWriteTextFormat*> cache;
  auto key = std::make_tuple((int)(size * 10), (int)w, display);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  // San Francisco tiene prioridad si esta instalada; luego la fuente configurada, Inter y Segoe
  std::wstring fam = display ? g_cfg.fontDisplay : g_cfg.font;
  bool isDefault = fam.rfind(L"Segoe UI", 0) == 0;
  std::vector<std::wstring> cands;
  if (isDefault) {
    if (display) cands = { L"SF Pro Display", L"SF Pro", L"Inter Display", L"Inter", fam };
    else cands = { L"SF Pro Text", L"SF Pro", L"Inter", fam };
  }
  else cands = { fam };
  cands.push_back(L"Segoe UI Variable Text");
  cands.push_back(L"Segoe UI");
  for (auto& c : cands) if (FontExists(c)) { fam = c; break; }
  IDWriteTextFormat* f = nullptr;
  g_dw->CreateTextFormat(fam.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
  f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
  IDWriteInlineObject* ell = nullptr;
  g_dw->CreateEllipsisTrimmingSign(f, &ell);
  f->SetTrimming(&tr, ell);
  if (ell) ell->Release();
  return cache[key] = f;
}

ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c) {
  static ID2D1SolidColorBrush* b = nullptr;
  if (!b) g_rt->CreateSolidColorBrush(c, &b);
  b->SetColor(c);
  b->SetOpacity(1.f);
  return b;
}

float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
  IDWriteTextLayout* l = nullptr;
  g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 1000, &l);
  DWRITE_TEXT_METRICS m{};
  if (l) { l->GetMetrics(&m); l->Release(); }
  return m.widthIncludingTrailingWhitespace;
}

void DrawText(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c, Align a, bool vcenter) {
  f->SetTextAlignment(a == AL_LEFT ? DWRITE_TEXT_ALIGNMENT_LEADING : a == AL_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
  f->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void FillRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
  if (rad <= 0) rt->FillRectangle(r, Brush(c));
  else rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), Brush(c));
}

void StrokeRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w) {
  rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), Brush(c), w);
}

void DrawGlass(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float tintBoost) {
  D2D1_COLOR_F tint = g_th.tint;
  tint.a = Clamp(tint.a + tintBoost, 0.f, 1.f);
  FillRound(rt, r, rad, tint);
  // brillo superior tipo "liquid glass"
  ID2D1GradientStopCollection* stops = nullptr;
  D2D1_GRADIENT_STOP gs[2] = { { 0.f, RGBA(255, 255, 255, g_th.dark ? .07f : .22f) }, { 1.f, RGBA(255, 255, 255, 0) } };
  rt->CreateGradientStopCollection(gs, 2, &stops);
  ID2D1LinearGradientBrush* lg = nullptr;
  rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.top + (r.bottom - r.top) * .5f }), stops, &lg);
  rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg);
  lg->Release(); stops->Release();
  D2D1_RECT_F in = { r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f };
  StrokeRound(rt, in, std::max(0.f, rad - .5f), g_th.rim, 1.f);
}

void DrawShadow(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float size, float alpha) {
  int steps = (int)size;
  for (int i = steps; i >= 1; i--) {
    float t = i / (float)steps;
    D2D1_RECT_F s = { r.left - i, r.top - i * .6f, r.right + i, r.bottom + i * 1.2f };
    StrokeRound(rt, s, rad + i, RGBA(0, 0, 0, alpha * (1 - t) * (1 - t) / steps * 3.f), 1.6f);
  }
}

ID2D1PathGeometry* MakePath(std::function<void(ID2D1GeometrySink*)> build, bool filled) {
  ID2D1PathGeometry* p = nullptr;
  g_d2d->CreatePathGeometry(&p);
  ID2D1GeometrySink* s = nullptr;
  p->Open(&s);
  build(s);
  s->Close(); s->Release();
  return p;
}

void DrawSymbol(ID2D1RenderTarget* rt, const wchar_t* glyph, D2D1_RECT_F r, float size, D2D1_COLOR_F c) {
  static std::map<int, IDWriteTextFormat*> fmts;
  int k = (int)(size * 10);
  if (!fmts[k]) {
    const wchar_t* fam = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    g_dw->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmts[k]);
    fmts[k]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmts[k]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  rt->DrawTextW(glyph, (UINT32)wcslen(glyph), fmts[k], r, Brush(c));
}

void DrawBitmapHQ(ID2D1RenderTarget* rt, ID2D1Bitmap* b, D2D1_RECT_F dst, float alpha, const D2D1_RECT_F* src) {
  // la interpolacion cubica de alta calidad evita el "ruido" al reducir iconos grandes
  if (g_dc && rt == g_rt) g_dc->DrawBitmap(b, &dst, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, src, nullptr);
  else rt->DrawBitmap(b, dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
}

void PushRoundClip(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad) {
  ID2D1RoundedRectangleGeometry* g = nullptr;
  g_d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, rad, rad), &g);
  rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
  g->Release();
}

void PopClip(ID2D1RenderTarget* rt) { rt->PopLayer(); }

void DrawFolderIcon(ID2D1RenderTarget* rt, D2D1_RECT_F r, float a, const wchar_t* glyph) {
  // carpeta azul estilo macOS
  float s = r.right - r.left, l = r.left + s * .04f, rr = r.right - s * .04f, t = r.top + s * .14f, b = r.bottom - s * .1f;
  FillRound(rt, { l, t, l + s * .4f, t + s * .2f }, s * .05f, RGBA(78, 160, 236, a));
  FillRound(rt, { l, t + s * .07f, rr, b }, s * .07f, RGBA(78, 160, 236, a));
  ID2D1GradientStopCollection* st = nullptr;
  D2D1_GRADIENT_STOP gs[2] = { { 0, RGBA(132, 196, 255, a) }, { 1, RGBA(92, 172, 248, a) } };
  rt->CreateGradientStopCollection(gs, 2, &st);
  ID2D1LinearGradientBrush* lg = nullptr;
  rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, t + s * .16f }, { 0, b }), st, &lg);
  rt->FillRoundedRectangle(D2D1::RoundedRect({ l, t + s * .16f, rr, b }, s * .07f, s * .07f), lg);
  lg->Release(); st->Release();
  rt->FillRectangle({ l + s * .02f, t + s * .16f, rr - s * .02f, t + s * .16f + std::max(.6f, s * .012f) }, Brush(RGBA(255, 255, 255, .55f * a)));
  if (glyph) DrawSymbol(rt, glyph, { l, t + s * .2f, rr, b }, s * .34f, RGBA(48, 128, 214, a));
}

// ================================================================ superficies
static const wchar_t* kSurfaceClass = L"ZyntraSurface";

Surface::~Surface() {
  if (memDC) { SelectObject(memDC, oldBmp); DeleteDC(memDC); }
  if (dib) DeleteObject(dib);
  if (hwnd) DestroyWindow(hwnd);
}

void Surface::Create(DWORD exStyle, bool layered_, bool glass) {
  static bool reg = false;
  if (!reg) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc; wc.hInstance = g_inst; wc.lpszClassName = kSurfaceClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    reg = true;
  }
  layered = layered_;
  hwnd = CreateWindowExW(exStyle | WS_EX_TOOLWINDOW | (layered ? WS_EX_LAYERED : 0), kSurfaceClass, L"Zyntra", WS_POPUP,
                         0, 0, 10, 10, nullptr, nullptr, g_inst, this);
  int corner = dwmRound ? 2 : 1;  // 2 = redondeo de Windows 11 con antialias; 1 = sin redondeo
  DwmSetWindowAttribute(hwnd, 33, &corner, sizeof corner);
  COLORREF none = 0xFFFFFFFE;
  DwmSetWindowAttribute(hwnd, 34, &none, sizeof none);
  if (glass) {
    MARGINS m{ -1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);
    ApplyGlass(hwnd, true);
  }
}

void Surface::ApplyShape() {
  if (layered || dwmRound) return;
  if (radius > 0) {
    int r = Px(radius * 2);
    SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, r, r), TRUE);
  } else SetWindowRgn(hwnd, nullptr, TRUE);
}

void Surface::SetBounds(int nx, int ny, int nw, int nh, HWND after) {
  bool resized = nw != w || nh != h;
  x = nx; y = ny; w = nw; h = nh;
  if (layered) { RenderNow(); return; }
  UINT fl = SWP_NOACTIVATE | (after ? 0 : SWP_NOZORDER);
  SetWindowPos(hwnd, after, x, y, w, h, fl);
  if (resized) ApplyShape();
  Invalidate();
}

void Surface::Invalidate() {
  if (!hwnd) return;
  if (!layered) { InvalidateRect(hwnd, nullptr, FALSE); return; }
  if (!pending) { pending = true; PostMessageW(hwnd, WM_APP_RENDER, 0, 0); }
}

void Surface::Show(bool show, bool activate) {
  if (!hwnd) return;
  if (show) {
    if (layered) RenderNow();
    ShowWindow(hwnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(hwnd);
  } else ShowWindow(hwnd, SW_HIDE);
}

void Surface::RenderNow() {
  pending = false;
  if (w <= 0 || h <= 0) return;
  if (!layered) { InvalidateRect(hwnd, nullptr, FALSE); UpdateWindow(hwnd); return; }
  if (!memDC) memDC = CreateCompatibleDC(nullptr);
  if (dibW != w || dibH != h) {
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits;
    HBITMAP nb = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ prev = SelectObject(memDC, nb);
    if (!oldBmp) oldBmp = prev;
    if (dib) DeleteObject(dib);
    dib = nb; dibW = w; dibH = h;
  }
  RECT rc{ 0, 0, w, h };
  g_rt->BindDC(memDC, &rc);
  g_rt->BeginDraw();
  g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
  Paint(g_rt);
  g_rt->EndDraw();
  POINT pos{ x, y }, src{ 0, 0 };
  SIZE sz{ w, h };
  BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  UpdateLayeredWindow(hwnd, nullptr, &pos, &sz, memDC, &src, 0, &bf, ULW_ALPHA);
}

LRESULT Surface::Handle(UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcW(hwnd, m, wp, lp); }

LRESULT CALLBACK Surface::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_NCCREATE) {
    auto cs = (CREATESTRUCTW*)lp;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    ((Surface*)cs->lpCreateParams)->hwnd = h;
  }
  auto s = (Surface*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (!s) return DefWindowProcW(h, m, wp, lp);
  switch (m) {
  case WM_ERASEBKGND: return 1;
  case WM_APP_RENDER: s->RenderNow(); return 0;
  case WM_WINDOWPOSCHANGED:
    if (s->layered) {  // al arrastrar una ventana layered, mantenemos su posicion al dia
      auto p = (WINDOWPOS*)lp;
      if (!(p->flags & SWP_NOMOVE)) { s->x = p->x; s->y = p->y; }
    }
    break;
  case WM_PAINT:
    if (!s->layered) {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc{ 0, 0, s->w, s->h };
      g_rt->BindDC(dc, &rc);
      g_rt->BeginDraw();
      g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
      s->Paint(g_rt);
      g_rt->EndDraw();
      EndPaint(h, &ps);
      return 0;
    }
    break;
  }
  return s->Handle(m, wp, lp);
}

void InvalidateAll() { GameMode_Invalidate(); }
