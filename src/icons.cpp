#include "common.h"
#include <thread>
#include <condition_variable>
#include <deque>

// Iconos: se cargan en un hilo aparte, se normaliza su alfa, se analizan (forma y color) y se
// dibujan con estilo macOS (squircle) usando filtrado cubico de alta calidad.

static bool EndsWith(const std::wstring& s, const wchar_t* suf) {
  size_t n = wcslen(suf);
  return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

struct IconEntry {
  IWICBitmapSource* wic = nullptr;
  ID2D1Bitmap* bmp = nullptr;
  bool ready = false, custom = false, fullBleed = false;
  D2D1_COLOR_F dom{ .5f, .5f, .5f, 1 };
};
struct IconJob { std::wstring key, path, name; int px; };

static std::map<std::wstring, IconEntry> s_icons;
static std::deque<IconJob> s_jobs;
static std::mutex s_mx;
static std::condition_variable s_cv;
static bool s_workerStarted = false;

static IWICBitmapSource* ToMemory(IWICBitmapSource* src) {
  IWICFormatConverter* conv = nullptr;
  g_wic->CreateFormatConverter(&conv);
  conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
  IWICBitmap* mem = nullptr;
  g_wic->CreateBitmapFromSource(conv, WICBitmapCacheOnLoad, &mem);
  conv->Release();
  return mem;
}

static IWICBitmapSource* Scale(IWICBitmapSource* src, int px) {
  IWICBitmapScaler* sc = nullptr;
  g_wic->CreateBitmapScaler(&sc);
  sc->Initialize(src, px, px, WICBitmapInterpolationModeHighQualityCubic);
  IWICBitmapSource* r = ToMemory(sc);
  sc->Release();
  return r;
}

static IWICBitmapSource* LoadPng(const std::wstring& file, int px) {
  IWICBitmapDecoder* dec = nullptr;
  if (FAILED(g_wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return nullptr;
  IWICBitmapFrameDecode* fr = nullptr;
  IWICBitmapSource* r = nullptr;
  if (SUCCEEDED(dec->GetFrame(0, &fr))) {
    IWICBitmapSource* m = ToMemory(fr);
    r = Scale(m, px);
    m->Release(); fr->Release();
  }
  dec->Release();
  return r;
}

// Muchas apps Win32 solo traen iconos de 32/48 px: el shell los devuelve pequeños en un lienzo
// grande y transparente. Recortamos al contenido y reescalamos para que llenen el espacio.
static IWICBitmapSource* FitContent(IWICBitmapSource* src, int px) {
  UINT w, h;
  src->GetSize(&w, &h);
  std::vector<BYTE> buf(w * h * 4);
  src->CopyPixels(nullptr, w * 4, (UINT)buf.size(), buf.data());
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (UINT y = 0; y < h; y++)
    for (UINT x = 0; x < w; x++)
      if (buf[(y * w + x) * 4 + 3] > 8) { x0 = std::min(x0, (int)x); y0 = std::min(y0, (int)y); x1 = std::max(x1, (int)x); y1 = std::max(y1, (int)y); }
  if (x1 < 0) return nullptr;
  int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
  if (std::max(cw, ch) >= (int)w * 0.7) { src->AddRef(); return src; }
  int side = std::max(cw, ch);
  int cx = x0 + cw / 2 - side / 2, cy = y0 + ch / 2 - side / 2;
  cx = std::max(0, std::min(cx, (int)w - side)); cy = std::max(0, std::min(cy, (int)h - side));
  IWICBitmapClipper* clip = nullptr;
  g_wic->CreateBitmapClipper(&clip);
  WICRect rc{ cx, cy, side, side };
  clip->Initialize(src, &rc);
  IWICBitmapSource* r = Scale(clip, px);
  clip->Release();
  return r;
}

// Algunos iconos del shell vienen con alfa "directo" y otros premultiplicado. Tratar uno como el
// otro deja un halo blanco en el borde: lo detectamos pixel a pixel y lo normalizamos.
static IWICBitmapSource* FromHBitmap(HBITMAP hb) {
  BITMAP bm;
  if (!GetObjectW(hb, sizeof bm, &bm)) return nullptr;
  int w = bm.bmWidth, h = bm.bmHeight;
  std::vector<BYTE> px((size_t)w * h * 4);
  BITMAPINFO bi{};
  bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
  HDC dc = GetDC(nullptr);
  int ok = GetDIBits(dc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS);
  ReleaseDC(nullptr, dc);
  if (!ok) return nullptr;
  bool anyAlpha = false, straight = false;
  for (size_t i = 0; i < px.size(); i += 4) {
    BYTE a = px[i + 3];
    if (a) anyAlpha = true;
    if (px[i] > a || px[i + 1] > a || px[i + 2] > a) straight = true;
  }
  for (size_t i = 0; i < px.size(); i += 4) {
    if (!anyAlpha) { px[i + 3] = 255; continue; }  // icono antiguo sin canal alfa
    if (straight) {
      BYTE a = px[i + 3];
      px[i] = (BYTE)(px[i] * a / 255); px[i + 1] = (BYTE)(px[i + 1] * a / 255); px[i + 2] = (BYTE)(px[i + 2] * a / 255);
    }
  }
  IWICBitmap* wb = nullptr;
  g_wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, (UINT)px.size(), px.data(), &wb);
  return wb;
}

static IWICBitmapSource* LoadShellIcon(const std::wstring& path, int px, bool thumb) {
  IShellItemImageFactory* f = nullptr;
  if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return nullptr;
  HBITMAP hb = nullptr;
  IWICBitmapSource* r = nullptr;
  SIIGBF fl = thumb ? (SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK) : SIIGBF_ICONONLY;
  if (SUCCEEDED(f->GetImage({ px, px }, fl, &hb)) && hb) {
    if (IWICBitmapSource* mem = FromHBitmap(hb)) {
      if (thumb) { r = mem; mem = nullptr; }
      else r = FitContent(mem, px);
      if (mem) mem->Release();
    }
    DeleteObject(hb);
  }
  f->Release();
  return r;
}

// Analiza el icono: ¿ya es un cuadrado lleno (se recorta como squircle)? ¿cual es su color?
static void Analyze(IWICBitmapSource* src, bool& fullBleed, D2D1_COLOR_F& dom) {
  UINT w, h;
  src->GetSize(&w, &h);
  std::vector<BYTE> p((size_t)w * h * 4);
  src->CopyPixels(nullptr, w * 4, (UINT)p.size(), p.data());
  auto A = [&](float fx, float fy) { return p[((size_t)(fy * (h - 1)) * w + (size_t)(fx * (w - 1))) * 4 + 3]; };
  bool corners = A(.1f, .1f) > 220 && A(.9f, .1f) > 220 && A(.1f, .9f) > 220 && A(.9f, .9f) > 220;
  size_t opaque = 0;
  double r = 0, g = 0, b = 0, wsum = 0;
  for (size_t i = 0; i < p.size(); i += 4) {
    BYTE a = p[i + 3];
    if (a < 200) continue;
    opaque++;
    double B = p[i] * 255.0 / a, G = p[i + 1] * 255.0 / a, R = p[i + 2] * 255.0 / a;
    double mx = std::max({ R, G, B }), mn = std::min({ R, G, B });
    double sat = mx > 0 ? (mx - mn) / mx : 0;
    double wt = sat * sat * (mx / 255.0) + .01;
    r += R * wt; g += G * wt; b += B * wt; wsum += wt;
  }
  fullBleed = corners && opaque > p.size() / 4 * .86;
  if (wsum > 0) dom = D2D1::ColorF((float)(r / wsum / 255), (float)(g / wsum / 255), (float)(b / wsum / 255));
}

static std::wstring SafeFileName(std::wstring s) {
  for (auto& c : s) if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
  return s;
}

static void IconWorker() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  for (;;) {
    IconJob job;
    {
      std::unique_lock<std::mutex> lk(s_mx);
      s_cv.wait(lk, [] { return !s_jobs.empty(); });
      job = s_jobs.front();
      s_jobs.pop_front();
    }
    bool custom = false, thumb = job.path.rfind(L"thumb:", 0) == 0;
    std::wstring path = thumb ? job.path.substr(6) : job.path;
    IWICBitmapSource* bmp = nullptr;
    if (!thumb) {
      // icono propio: icons\<Nombre de la app>.png o icons\<exe>.png
      std::vector<std::wstring> cands = { g_dir + L"\\icons\\" + SafeFileName(job.name) + L".png" };
      size_t slash = path.find_last_of(L"\\/");
      if (slash != std::wstring::npos) {
        std::wstring base = path.substr(slash + 1);
        if (EndsWith(base, L".exe")) cands.push_back(g_dir + L"\\icons\\" + base.substr(0, base.size() - 4) + L".png");
        else cands.push_back(g_dir + L"\\icons\\" + SafeFileName(base) + L".png");
      }
      for (auto& c : cands)
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES && (bmp = LoadPng(c, job.px))) { custom = true; break; }
    }
    if (!bmp) bmp = LoadShellIcon(path, job.px, thumb);
    bool full = false;
    D2D1_COLOR_F dom{ .5f, .5f, .5f, 1 };
    if (bmp && !custom && !thumb) Analyze(bmp, full, dom);
    {
      std::lock_guard<std::mutex> lk(s_mx);
      auto it = s_icons.find(job.key);
      if (it == s_icons.end()) { if (bmp) bmp->Release(); continue; }
      it->second.wic = bmp;
      it->second.custom = custom || thumb;
      it->second.fullBleed = full;
      it->second.dom = dom;
      it->second.ready = true;
    }
    PostMessageW(g_msgWnd, WM_APP_ICONREADY, 0, 0);
  }
}

static IconEntry* GetEntry(const std::wstring& path, const std::wstring& name, int px) {
  std::wstring key = path + L"|" + std::to_wstring(px);
  std::lock_guard<std::mutex> lk(s_mx);
  auto it = s_icons.find(key);
  if (it == s_icons.end()) {
    s_icons[key] = IconEntry{};
    s_jobs.push_back({ key, path, name, px });
    if (!s_workerStarted) { s_workerStarted = true; std::thread(IconWorker).detach(); }
    s_cv.notify_one();
    return nullptr;
  }
  IconEntry& e = it->second;
  if (!e.ready) return nullptr;
  if (!e.bmp && e.wic) {
    g_rt->CreateBitmapFromWicBitmap(e.wic, nullptr, &e.bmp);
    e.wic->Release();  // solo guardamos una copia de los pixeles
    e.wic = nullptr;
  }
  return e.bmp ? &e : nullptr;
}

ID2D1Bitmap* GetIcon(const std::wstring& path, const std::wstring& name, int px, bool* custom) {
  IconEntry* e = GetEntry(path, name, px);
  if (!e) return nullptr;
  if (custom) *custom = e->custom;
  return e->bmp;
}

void ReleaseIcons(int px) {
  std::lock_guard<std::mutex> lk(s_mx);
  std::wstring suf = L"|" + std::to_wstring(px);
  for (auto it = s_icons.begin(); it != s_icons.end();) {
    if (EndsWith(it->first, suf.c_str())) {
      if (it->second.bmp) it->second.bmp->Release();
      if (it->second.wic) it->second.wic->Release();
      it = s_icons.erase(it);
    } else ++it;
  }
  s_jobs.erase(std::remove_if(s_jobs.begin(), s_jobs.end(), [px](const IconJob& j) { return j.px == px; }), s_jobs.end());
}

static D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t, float alpha) {
  return D2D1::ColorF(Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), alpha);
}

void DrawAppIcon(ID2D1RenderTarget* rt, const std::wstring& path, const std::wstring& name, D2D1_RECT_F r, int loadPx, float alpha, bool pressed) {
  // apps del sistema: icono vectorial estilo macOS (sin cargar bitmaps)
  static std::map<std::wstring, int> kinds;
  auto k = kinds.find(path);
  if (k == kinds.end()) k = kinds.emplace(path, SystemIconKind(path, name)).first;
  if (k->second >= 0 && DrawSystemIcon(rt, k->second, r, alpha, pressed)) return;
  IconEntry* e = GetEntry(path, name, loadPx);
  float sz = r.right - r.left, rad = sz * .225f;
  if (!e) {
    FillRound(rt, r, rad, WithA(g_th.hover, alpha));
    return;
  }
  D2D1_RECT_F shadow{ r.left + sz * .02f, r.top + sz * .04f, r.right - sz * .02f, r.bottom + sz * .03f };
  if (e->custom) {
    DrawBitmapHQ(rt, e->bmp, r, alpha);
  } else if (e->fullBleed) {
    // icono cuadrado "a sangre": se recorta con la forma squircle de macOS
    FillRound(rt, shadow, rad, RGBA(0, 0, 0, .14f * alpha));
    PushRoundClip(rt, r, rad);
    float o = sz * .03f;
    DrawBitmapHQ(rt, e->bmp, { r.left - o, r.top - o, r.right + o, r.bottom + o }, alpha);
    PopClip(rt);
    StrokeRound(rt, { r.left + .4f, r.top + .4f, r.right - .4f, r.bottom - .4f }, rad, RGBA(255, 255, 255, .22f * alpha), .8f);
  } else if (g_cfg.squircle) {
    // fondo squircle teñido con el color de la propia app
    D2D1_COLOR_F top, bot;
    if (g_th.dark) { top = Mix(e->dom, RGBA(58, 58, 64), .72f, alpha); bot = Mix(e->dom, RGBA(30, 30, 34), .80f, alpha); }
    else { top = Mix(e->dom, RGBA(255, 255, 255), .90f, alpha); bot = Mix(e->dom, RGBA(255, 255, 255), .74f, alpha); }
    FillRound(rt, shadow, rad, RGBA(0, 0, 0, .14f * alpha));
    ID2D1GradientStopCollection* st = nullptr;
    D2D1_GRADIENT_STOP gs[2] = { { 0, top }, { 1, bot } };
    rt->CreateGradientStopCollection(gs, 2, &st);
    ID2D1LinearGradientBrush* lg = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), st, &lg);
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg);
    lg->Release(); st->Release();
    StrokeRound(rt, { r.left + .4f, r.top + .4f, r.right - .4f, r.bottom - .4f }, rad, RGBA(255, 255, 255, (g_th.dark ? .14f : .6f) * alpha), .8f);
    float in = sz * .17f;
    DrawBitmapHQ(rt, e->bmp, { r.left + in, r.top + in, r.right - in, r.bottom - in }, alpha);
  } else {
    DrawBitmapHQ(rt, e->bmp, r, alpha);
  }
  if (pressed) FillRound(rt, r, rad, RGBA(0, 0, 0, .3f * alpha));
}

// ================================================================ anclados de la barra de tareas
static std::wstring LinkAumid(const std::wstring& lnk) {
  IShellItem2* it = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHCreateItemFromParsingName(lnk.c_str(), nullptr, IID_PPV_ARGS(&it)))) {
    PWSTR v = nullptr;
    if (SUCCEEDED(it->GetString(PKEY_AppUserModel_ID, &v)) && v) { r = Lower(v); CoTaskMemFree(v); }
    it->Release();
  }
  return r;
}

static void LinkTarget(const std::wstring& lnk, std::wstring& exe, std::wstring& args) {
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
  IPersistFile* pf = nullptr;
  if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
    if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
      wchar_t p[MAX_PATH] = {}, a[1024] = {};
      sl->GetPath(p, MAX_PATH, nullptr, 0);
      sl->GetArguments(a, 1024);
      exe = Lower(p); args = a;
    }
    pf->Release();
  }
  sl->Release();
}

std::vector<std::wstring> TaskbarPins() {
  // orden real: el registro Taskband guarda los anclados (accesos .lnk y apps de la Store) en orden
  std::vector<BYTE> raw;
  DWORD sz = 0;
  const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
  if (RegGetValueW(HKEY_CURRENT_USER, key, L"Favorites", RRF_RT_REG_BINARY, nullptr, nullptr, &sz) == ERROR_SUCCESS && sz) {
    raw.resize(sz);
    RegGetValueW(HKEY_CURRENT_USER, key, L"Favorites", RRF_RT_REG_BINARY, nullptr, raw.data(), &sz);
  }
  auto decode = [&](size_t off) { std::wstring s; for (size_t i = off; i + 1 < raw.size(); i += 2) s += (wchar_t)(raw[i] | (raw[i + 1] << 8)); return s; };
  std::wstring even = decode(0), odd = raw.size() > 1 ? decode(1) : L"";
  auto posOf = [&](const std::wstring& needle) -> size_t {
    size_t a = even.find(needle), b = odd.find(needle);
    if (a != std::wstring::npos) return a * 2;
    if (b != std::wstring::npos) return b * 2 + 1;
    return (size_t)-1;
  };
  std::vector<std::pair<size_t, const AppInfo*>> found;
  auto add = [&](size_t pos, const AppInfo* a) {
    if (!a) return;
    for (auto& f : found) if (f.second == a) return;
    found.push_back({ pos, a });
  };
  // 1) accesos directos anclados
  PWSTR appdata = nullptr;
  SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
  std::wstring dir = std::wstring(appdata) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\";
  CoTaskMemFree(appdata);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*.lnk").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring file = dir + fd.cFileName, base = fd.cFileName;
      base = base.substr(0, base.size() - 4);
      std::wstring exe, args, aumid = LinkAumid(file);
      LinkTarget(file, exe, args);
      if (aumid.empty() && (base == L"File Explorer" || base == L"Explorador de archivos")) aumid = L"microsoft.windows.explorer";
      const AppInfo* a = aumid.empty() ? nullptr : MatchApp(aumid, L"");
      if (!a) for (auto& x : g_apps) if (_wcsicmp(x.name.c_str(), base.c_str()) == 0) { a = &x; break; }
      if (!a && args.empty()) a = MatchApp(L"", exe);
      add(posOf(fd.cFileName), a);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  // 2) apps de la Store: "Paquete_hash!App"
  for (int pass = 0; pass < 2; pass++) {
    const std::wstring& s = pass ? odd : even;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] != L'!') continue;
      auto ok = [](wchar_t c) { return iswalnum(c) || c == L'.' || c == L'-' || c == L'_'; };
      size_t a = i, b = i + 1;
      while (a > 0 && ok(s[a - 1])) a--;
      while (b < s.size() && ok(s[b])) b++;
      if (b == i + 1 || a == i) continue;
      std::wstring id = s.substr(a, b - a);
      if (id.find(L'_') == std::wstring::npos) continue;
      if (const AppInfo* ap = MatchApp(Lower(id), L"")) add(pass ? a * 2 + 1 : a * 2, ap);
    }
  }
  std::stable_sort(found.begin(), found.end(), [](auto& x, auto& y) { return x.first < y.first; });
  std::vector<std::wstring> out;
  for (auto& f : found) out.push_back(f.second->id);
  return out;
}
