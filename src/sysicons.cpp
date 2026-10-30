#include "common.h"
#include <shlwapi.h>

// Iconos vectoriales estilo macOS para las apps del sistema de Windows (y el logo Zargch PC).
// Se dibujan nitidos a cualquier tamaño y no ocupan memoria de bitmaps.
enum SysIcon {
  SI_NONE = -1, SI_SETTINGS, SI_STORE, SI_CALC, SI_NOTES, SI_TERMINAL, SI_ACTIVITY, SI_PHOTOS, SI_CAMERA,
  SI_CALENDAR, SI_MAIL, SI_CLOCK, SI_PAINT, SI_SCREENSHOT, SI_CONTROL, SI_MAPS, SI_WEATHER, SI_MUSIC, SI_TV,
  SI_VOICE, SI_STICKY, SI_ZARGCH, SI_GAMES
};

int SystemIconKind(const std::wstring& path, const std::wstring& name) {
  std::wstring p = Lower(path), n = Lower(name);
  struct { const wchar_t* key; int kind; } ids[] = {
    { L"windows.immersivecontrolpanel", SI_SETTINGS }, { L"microsoft.windowsstore", SI_STORE },
    { L"microsoft.windowscalculator", SI_CALC }, { L"microsoft.windowsnotepad", SI_NOTES },
    { L"microsoft.windowsterminal", SI_TERMINAL }, { L"microsoft.windows.photos", SI_PHOTOS },
    { L"microsoft.windowscamera", SI_CAMERA }, { L"microsoft.windowsalarms", SI_CLOCK },
    { L"microsoft.paint", SI_PAINT }, { L"microsoft.screensketch", SI_SCREENSHOT },
    { L"microsoft.bingweather", SI_WEATHER }, { L"microsoft.zunemusic", SI_MUSIC },
    { L"microsoft.zunevideo", SI_TV }, { L"microsoft.windowssoundrecorder", SI_VOICE },
    { L"microsoft.microsoftstickynotes", SI_STICKY }, { L"microsoft.windowsmaps", SI_MAPS },
    { L"taskmgr.exe", SI_ACTIVITY }, { L"\\control.exe", SI_CONTROL }, { L"\\cmd.exe", SI_TERMINAL },
    { L"powershell.exe", SI_TERMINAL }, { L"pwsh.exe", SI_TERMINAL }, { L"mspaint.exe", SI_PAINT },
    { L"snippingtool.exe", SI_SCREENSHOT }, { L"notepad.exe", SI_NOTES }, { L"calc.exe", SI_CALC },
  };
  for (auto& i : ids) if (p.find(i.key) != std::wstring::npos) return i.kind;
  struct { const wchar_t* name; int kind; } names[] = {
    { L"configuración", SI_SETTINGS }, { L"settings", SI_SETTINGS }, { L"microsoft store", SI_STORE },
    { L"calculadora", SI_CALC }, { L"calculator", SI_CALC }, { L"bloc de notas", SI_NOTES }, { L"notepad", SI_NOTES },
    { L"terminal", SI_TERMINAL }, { L"windows powershell", SI_TERMINAL }, { L"símbolo del sistema", SI_TERMINAL },
    { L"command prompt", SI_TERMINAL }, { L"administrador de tareas", SI_ACTIVITY }, { L"task manager", SI_ACTIVITY },
    { L"fotos", SI_PHOTOS }, { L"photos", SI_PHOTOS }, { L"cámara", SI_CAMERA }, { L"camera", SI_CAMERA },
    { L"calendario", SI_CALENDAR }, { L"calendar", SI_CALENDAR }, { L"google calendar", SI_CALENDAR }, { L"correo", SI_MAIL }, { L"mail", SI_MAIL },
    { L"reloj", SI_CLOCK }, { L"clock", SI_CLOCK }, { L"alarmas y reloj", SI_CLOCK }, { L"paint", SI_PAINT },
    { L"recortes", SI_SCREENSHOT }, { L"herramienta recortes", SI_SCREENSHOT }, { L"snipping tool", SI_SCREENSHOT },
    { L"panel de control", SI_CONTROL }, { L"control panel", SI_CONTROL }, { L"mapas", SI_MAPS }, { L"maps", SI_MAPS },
    { L"el tiempo", SI_WEATHER }, { L"tiempo", SI_WEATHER }, { L"weather", SI_WEATHER },
    { L"reproductor multimedia", SI_MUSIC }, { L"media player", SI_MUSIC }, { L"películas y tv", SI_TV },
    { L"movies & tv", SI_TV }, { L"grabadora de sonido", SI_VOICE }, { L"sound recorder", SI_VOICE },
    { L"notas rápidas", SI_STICKY }, { L"sticky notes", SI_STICKY }, { L"modo game", SI_GAMES }, { L"zargch game", SI_GAMES },
  };
  for (auto& i : names) if (n == i.name) return i.kind;
  return SI_NONE;
}

static ID2D1StrokeStyle* Round() {
  static ID2D1StrokeStyle* s = nullptr;
  if (!s) {
    auto p = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    g_d2d->CreateStrokeStyle(p, nullptr, 0, &s);
  }
  return s;
}

static void Grad(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, std::initializer_list<D2D1_COLOR_F> cols, bool diagonal = false) {
  std::vector<D2D1_GRADIENT_STOP> gs;
  int i = 0, n = (int)cols.size();
  for (auto c : cols) gs.push_back({ n > 1 ? i++ / (float)(n - 1) : 0.f, c });
  ID2D1GradientStopCollection* st = nullptr;
  rt->CreateGradientStopCollection(gs.data(), (UINT)gs.size(), &st);
  ID2D1LinearGradientBrush* lg = nullptr;
  D2D1_POINT_2F a = diagonal ? D2D1::Point2F(r.left, r.top) : D2D1::Point2F(0, r.top);
  D2D1_POINT_2F b = diagonal ? D2D1::Point2F(r.right, r.bottom) : D2D1::Point2F(0, r.bottom);
  rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), st, &lg);
  rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg);
  lg->Release(); st->Release();
}

static void Gear(ID2D1RenderTarget* rt, float cx, float cy, float R, D2D1_COLOR_F body, D2D1_COLOR_F inner, D2D1_COLOR_F hole) {
  D2D1_MATRIX_3X2_F old;
  rt->GetTransform(&old);
  for (int i = 0; i < 12; i++) {
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(i * 30.f, { cx, cy }) * old);
    FillRound(rt, { cx - R * .14f, cy - R * 1.12f, cx + R * .14f, cy - R * .7f }, R * .06f, body);
  }
  rt->SetTransform(old);
  rt->FillEllipse(D2D1::Ellipse({ cx, cy }, R * .86f, R * .86f), Brush(body));
  rt->FillEllipse(D2D1::Ellipse({ cx, cy }, R * .62f, R * .62f), Brush(inner));
  rt->FillEllipse(D2D1::Ellipse({ cx, cy }, R * .26f, R * .26f), Brush(hole));
}

// logo de Zyntra (PNG incrustado en el exe, recurso 2), decodificado una vez a 256 px por render target
static ID2D1Bitmap* GamesLogo(ID2D1RenderTarget* rt) {
  static std::map<ID2D1RenderTarget*, ID2D1Bitmap*> cache;
  auto it = cache.find(rt);
  if (it != cache.end()) return it->second;
  ID2D1Bitmap* bmp = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* fr = nullptr;
  IStream* st = nullptr;
  if (HRSRC res = FindResourceW(g_inst, MAKEINTRESOURCEW(2), RT_RCDATA))
    if (HGLOBAL mem = LoadResource(g_inst, res))
      st = SHCreateMemStream((const BYTE*)LockResource(mem), SizeofResource(g_inst, res));
  if (st && SUCCEEDED(g_wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &fr))) {
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    g_wic->CreateBitmapScaler(&sc);
    g_wic->CreateFormatConverter(&cv);
    if (sc && cv && SUCCEEDED(sc->Initialize(fr, 256, 256, WICBitmapInterpolationModeHighQualityCubic)) &&
        SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
      rt->CreateBitmapFromWicBitmap(cv, nullptr, &bmp);
    if (cv) cv->Release();
    if (sc) sc->Release();
  }
  if (fr) fr->Release();
  if (dec) dec->Release();
  if (st) st->Release();
  cache[rt] = bmp;
  return bmp;
}

bool DrawSystemIcon(ID2D1RenderTarget* rt, int kind, D2D1_RECT_F r, float a, bool pressed) {
  if (kind < 0) return false;
  float s = r.right - r.left, rad = s * .225f, L = r.left, T = r.top;
  if (kind == SI_GAMES)
    if (ID2D1Bitmap* logo = GamesLogo(rt)) {
      FillRound(rt, { r.left + s * .02f, r.top + s * .04f, r.right - s * .02f, r.bottom + s * .03f }, s * .27f, RGBA(0, 0, 0, .14f * a));
      DrawBitmapHQ(rt, logo, r, a);
      if (pressed) FillRound(rt, r, s * .27f, RGBA(0, 0, 0, .3f * a));
      return true;
    }
  auto P = [&](float fx, float fy) { return D2D1::Point2F(L + fx * s, T + fy * s); };
  auto R = [&](float x0, float y0, float x1, float y1) { return D2D1::RectF(L + x0 * s, T + y0 * s, L + x1 * s, T + y1 * s); };
  auto line = [&](float x0, float y0, float x1, float y1, float w, D2D1_COLOR_F c) { rt->DrawLine(P(x0, y0), P(x1, y1), Brush(c), w * s, Round()); };
  auto C = [&](int rr, int gg, int bb, float aa = 1.f) { return RGBA((float)rr, (float)gg, (float)bb, aa * a); };
  // sombra suave como los iconos de macOS
  FillRound(rt, { r.left + s * .02f, r.top + s * .04f, r.right - s * .02f, r.bottom + s * .03f }, rad, RGBA(0, 0, 0, .14f * a));
  PushRoundClip(rt, r, rad);
  switch (kind) {
  case SI_SETTINGS:
  case SI_CONTROL: {
    bool ctl = kind == SI_CONTROL;
    Grad(rt, r, rad, { ctl ? C(150, 170, 200) : C(158, 158, 164), ctl ? C(80, 100, 140) : C(96, 96, 102) });
    Gear(rt, L + s * .5f, T + s * .5f, s * .33f, C(64, 64, 70), ctl ? C(180, 200, 230) : C(206, 206, 212), C(64, 64, 70));
    break;
  }
  case SI_STORE: {
    Grad(rt, r, rad, { C(38, 180, 255), C(0, 108, 236) });
    D2D1_COLOR_F w = C(255, 255, 255);
    line(.36f, .74f, .58f, .28f, .075f, w);
    line(.64f, .74f, .42f, .28f, .075f, w);
    line(.26f, .6f, .74f, .6f, .075f, w);
    break;
  }
  case SI_CALC: {
    Grad(rt, r, rad, { C(70, 70, 74), C(30, 30, 32) });
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 4; x++) {
        D2D1_COLOR_F c = x == 3 ? C(255, 159, 10) : y == 0 ? C(170, 170, 176) : C(96, 96, 102);
        float cx = .23f + x * .18f, cy = .23f + y * .18f;
        rt->FillEllipse(D2D1::Ellipse(P(cx, cy), s * .075f, s * .075f), Brush(c));
      }
    break;
  }
  case SI_NOTES: {
    Grad(rt, r, rad, { C(255, 255, 255), C(240, 240, 240) });
    Grad(rt, R(0, 0, 1, .3f), 0, { C(255, 222, 70), C(255, 196, 0) });
    for (int i = 0; i < 9; i++) rt->FillEllipse(D2D1::Ellipse(P(.12f + i * .095f, .3f), s * .012f, s * .012f), Brush(C(190, 150, 0)));
    for (int i = 0; i < 4; i++) line(.14f, .44f + i * .13f, .86f, .44f + i * .13f, .012f, C(200, 200, 205));
    break;
  }
  case SI_TERMINAL: {
    Grad(rt, r, rad, { C(48, 48, 50), C(16, 16, 18) });
    D2D1_COLOR_F w = C(235, 235, 235);
    line(.22f, .3f, .36f, .4f, .055f, w);
    line(.36f, .4f, .22f, .5f, .055f, w);
    line(.42f, .52f, .6f, .52f, .055f, w);
    StrokeRound(rt, { r.left + s * .04f, r.top + s * .04f, r.right - s * .04f, r.bottom - s * .04f }, rad * .8f, C(255, 255, 255, .12f), s * .015f);
    break;
  }
  case SI_ACTIVITY: {
    Grad(rt, r, rad, { C(44, 44, 48), C(18, 18, 20) });
    for (int i = 1; i < 5; i++) line(.1f, i * .2f, .9f, i * .2f, .006f, C(255, 255, 255, .1f));
    auto pth = MakePath([&](ID2D1GeometrySink* g) {
      g->BeginFigure(P(.1f, .66f), D2D1_FIGURE_BEGIN_HOLLOW);
      float pts[][2] = { { .22f, .6f }, { .3f, .7f }, { .4f, .34f }, { .5f, .58f }, { .6f, .44f }, { .7f, .52f }, { .78f, .26f }, { .9f, .4f } };
      for (auto& q : pts) g->AddLine(P(q[0], q[1]));
      g->EndFigure(D2D1_FIGURE_END_OPEN);
    }, false);
    rt->DrawGeometry(pth, Brush(C(50, 215, 75)), s * .04f, Round());
    pth->Release();
    break;
  }
  case SI_PHOTOS: {
    Grad(rt, r, rad, { C(255, 255, 255), C(244, 244, 246) });
    D2D1_COLOR_F pet[8] = { C(255, 149, 0, .85f), C(255, 204, 0, .85f), C(140, 210, 60, .85f), C(50, 200, 160, .85f),
                            C(60, 150, 255, .85f), C(130, 100, 230, .85f), C(220, 90, 200, .85f), C(255, 70, 80, .85f) };
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    D2D1_POINT_2F c = P(.5f, .5f);
    for (int i = 0; i < 8; i++) {
      rt->SetTransform(D2D1::Matrix3x2F::Rotation(i * 45.f, c) * old);
      rt->FillEllipse(D2D1::Ellipse({ c.x, c.y - s * .19f }, s * .1f, s * .19f), Brush(pet[i]));
    }
    rt->SetTransform(old);
    break;
  }
  case SI_CAMERA: {
    Grad(rt, r, rad, { C(214, 214, 220), C(150, 150, 158) });
    FillRound(rt, R(.16f, .32f, .84f, .74f), s * .07f, C(48, 48, 52));
    FillRound(rt, R(.34f, .25f, .56f, .34f), s * .03f, C(48, 48, 52));
    rt->FillEllipse(D2D1::Ellipse(P(.5f, .53f), s * .15f, s * .15f), Brush(C(150, 150, 158)));
    rt->FillEllipse(D2D1::Ellipse(P(.5f, .53f), s * .1f, s * .1f), Brush(C(30, 60, 110)));
    rt->FillEllipse(D2D1::Ellipse(P(.47f, .5f), s * .03f, s * .03f), Brush(C(255, 255, 255, .7f)));
    break;
  }
  case SI_CALENDAR: {
    Grad(rt, r, rad, { C(255, 255, 255), C(244, 244, 246) });
    SYSTEMTIME st; GetLocalTime(&st);
    std::wstring wd = FormatDate(L"ddd", st);
    wd.erase(std::remove(wd.begin(), wd.end(), L'.'), wd.end());
    CharUpperBuffW(wd.data(), (DWORD)wd.size());
    DrawText(rt, wd, Font(s * .16f, DWRITE_FONT_WEIGHT_SEMI_BOLD), R(0, .06f, 1, .3f), C(255, 59, 48), AL_CENTER);
    DrawText(rt, std::to_wstring(st.wDay), Font(s * .46f, DWRITE_FONT_WEIGHT_LIGHT, true), R(0, .24f, 1, .9f), C(28, 28, 30), AL_CENTER);
    break;
  }
  case SI_MAIL: {
    Grad(rt, r, rad, { C(90, 190, 255), C(18, 116, 240) });
    FillRound(rt, R(.18f, .3f, .82f, .72f), s * .05f, C(255, 255, 255));
    line(.2f, .33f, .5f, .55f, .035f, C(40, 140, 245));
    line(.8f, .33f, .5f, .55f, .035f, C(40, 140, 245));
    break;
  }
  case SI_CLOCK: {
    Grad(rt, r, rad, { C(30, 30, 32), C(8, 8, 10) });
    rt->FillEllipse(D2D1::Ellipse(P(.5f, .5f), s * .38f, s * .38f), Brush(C(255, 255, 255)));
    for (int i = 0; i < 12; i++) {
      float an = i * 3.14159f / 6;
      line(.5f + sinf(an) * .31f, .5f - cosf(an) * .31f, .5f + sinf(an) * .35f, .5f - cosf(an) * .35f, .02f, C(40, 40, 40));
    }
    line(.5f, .5f, .38f, .36f, .035f, C(20, 20, 20));
    line(.5f, .5f, .7f, .32f, .025f, C(20, 20, 20));
    line(.5f, .5f, .5f, .8f, .012f, C(255, 149, 0));
    rt->FillEllipse(D2D1::Ellipse(P(.5f, .5f), s * .025f, s * .025f), Brush(C(255, 149, 0)));
    break;
  }
  case SI_PAINT: {
    Grad(rt, r, rad, { C(255, 255, 255), C(240, 240, 244) });
    line(.22f, .68f, .5f, .3f, .1f, C(255, 69, 58));
    line(.36f, .76f, .72f, .36f, .1f, C(255, 204, 0));
    line(.52f, .8f, .8f, .52f, .1f, C(10, 132, 255));
    break;
  }
  case SI_SCREENSHOT: {
    Grad(rt, r, rad, { C(210, 210, 216), C(150, 150, 158) });
    ID2D1StrokeStyle* dash = nullptr;
    float d[] = { 2.f, 1.5f };
    g_d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                             D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM), d, 2, &dash);
    rt->DrawRectangle(R(.2f, .24f, .8f, .7f), Brush(C(255, 255, 255)), s * .035f, dash);
    if (dash) dash->Release();
    line(.62f, .58f, .62f, .82f, .03f, C(40, 40, 44));
    line(.5f, .7f, .74f, .7f, .03f, C(40, 40, 44));
    break;
  }
  case SI_MAPS: {
    Grad(rt, r, rad, { C(170, 225, 150), C(120, 200, 120) });
    rt->FillRectangle(R(0, .55f, 1, 1), Brush(C(250, 236, 200)));
    line(-.1f, .9f, 1.1f, .2f, .07f, C(255, 255, 255));
    line(.2f, -.1f, .6f, 1.1f, .05f, C(255, 214, 10));
    line(.3f, .7f, .7f, .42f, .045f, C(10, 132, 255));
    rt->FillEllipse(D2D1::Ellipse(P(.7f, .36f), s * .08f, s * .08f), Brush(C(255, 59, 48)));
    rt->FillEllipse(D2D1::Ellipse(P(.7f, .36f), s * .03f, s * .03f), Brush(C(255, 255, 255)));
    break;
  }
  case SI_WEATHER: {
    Grad(rt, r, rad, { C(40, 140, 240), C(120, 190, 250) });
    rt->FillEllipse(D2D1::Ellipse(P(.38f, .38f), s * .17f, s * .17f), Brush(C(255, 214, 10)));
    D2D1_COLOR_F w = C(255, 255, 255);
    rt->FillEllipse(D2D1::Ellipse(P(.5f, .6f), s * .14f, s * .14f), Brush(w));
    rt->FillEllipse(D2D1::Ellipse(P(.66f, .56f), s * .17f, s * .17f), Brush(w));
    FillRound(rt, R(.32f, .6f, .86f, .76f), s * .08f, w);
    break;
  }
  case SI_MUSIC: {
    Grad(rt, r, rad, { C(255, 92, 120), C(250, 36, 60) });
    D2D1_COLOR_F w = C(255, 255, 255);
    line(.42f, .3f, .42f, .66f, .045f, w);
    line(.7f, .24f, .7f, .6f, .045f, w);
    line(.42f, .3f, .7f, .24f, .07f, w);
    rt->FillEllipse(D2D1::Ellipse(P(.35f, .67f), s * .09f, s * .07f), Brush(w));
    rt->FillEllipse(D2D1::Ellipse(P(.63f, .61f), s * .09f, s * .07f), Brush(w));
    break;
  }
  case SI_TV: {
    Grad(rt, r, rad, { C(36, 36, 40), C(8, 8, 10) });
    DrawText(rt, L"tv", Font(s * .36f, DWRITE_FONT_WEIGHT_BOLD, true), r, C(255, 255, 255), AL_CENTER);
    break;
  }
  case SI_VOICE: {
    Grad(rt, r, rad, { C(36, 36, 40), C(8, 8, 10) });
    float hs[] = { .1f, .2f, .32f, .18f, .38f, .24f, .14f, .28f, .12f };
    for (int i = 0; i < 9; i++) line(.18f + i * .08f, .5f - hs[i] / 2, .18f + i * .08f, .5f + hs[i] / 2, .035f, i == 4 ? C(255, 59, 48) : C(255, 255, 255));
    break;
  }
  case SI_STICKY: {
    Grad(rt, r, rad, { C(255, 236, 120), C(255, 204, 0) });
    auto fold = MakePath([&](ID2D1GeometrySink* g) {
      g->BeginFigure(P(.7f, 1), D2D1_FIGURE_BEGIN_FILLED);
      g->AddLine(P(1, .7f)); g->AddLine(P(1, 1));
      g->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    rt->FillGeometry(fold, Brush(C(200, 150, 0)));
    fold->Release();
    for (int i = 0; i < 3; i++) line(.2f, .3f + i * .14f, .8f, .3f + i * .14f, .02f, C(180, 140, 0, .6f));
    break;
  }
  case SI_GAMES: {
    // Zargch Game: degradado vivo (cian -> violeta -> magenta), brillo de vidrio y un mando
    // blanco minimalista con los controles "recortados" en el color del fondo
    Grad(rt, r, rad, { C(0, 198, 255), C(106, 92, 255), C(255, 46, 136) }, true);
    Grad(rt, R(0, 0, 1, .52f), 0, { C(255, 255, 255, .32f), C(255, 255, 255, 0) });
    rt->FillEllipse(D2D1::Ellipse(P(.5f, .56f), s * .36f, s * .2f), Brush(C(255, 255, 255, .18f)));  // halo
    auto pad = MakePath([&](ID2D1GeometrySink* g) {
      g->BeginFigure(P(.34f, .35f), D2D1_FIGURE_BEGIN_FILLED);
      g->AddLine(P(.66f, .35f));
      g->AddBezier({ P(.8f, .35f), P(.86f, .42f), P(.89f, .55f) });
      g->AddBezier({ P(.92f, .7f), P(.86f, .76f), P(.79f, .73f) });
      g->AddBezier({ P(.74f, .71f), P(.71f, .63f), P(.64f, .63f) });
      g->AddLine(P(.36f, .63f));
      g->AddBezier({ P(.29f, .63f), P(.26f, .71f), P(.21f, .73f) });
      g->AddBezier({ P(.14f, .76f), P(.08f, .7f), P(.11f, .55f) });
      g->AddBezier({ P(.14f, .42f), P(.2f, .35f), P(.34f, .35f) });
      g->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    // sombra suave del mando
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, s * .025f) * old);
    rt->FillGeometry(pad, Brush(C(40, 10, 80, .28f)));
    rt->SetTransform(old);
    rt->FillGeometry(pad, Brush(C(255, 255, 255)));
    pad->Release();
    D2D1_COLOR_F ink = C(98, 84, 240);
    line(.25f, .49f, .37f, .49f, .05f, ink);   // cruceta
    line(.31f, .43f, .31f, .55f, .05f, ink);
    rt->FillEllipse(D2D1::Ellipse(P(.66f, .53f), s * .035f, s * .035f), Brush(C(255, 46, 136)));
    rt->FillEllipse(D2D1::Ellipse(P(.74f, .45f), s * .035f, s * .035f), Brush(C(0, 170, 240)));
    break;
  }
  case SI_ZARGCH: {
    // logo Zargch PC: degradado vivo, brillo de vidrio y una "Z" blanca con hoja
    Grad(rt, r, rad, { C(70, 90, 255), C(170, 80, 245), C(255, 60, 120) }, true);
    Grad(rt, R(0, 0, 1, .5f), 0, { C(255, 255, 255, .28f), C(255, 255, 255, 0) });
    D2D1_COLOR_F w = C(255, 255, 255);
    auto z = MakePath([&](ID2D1GeometrySink* g) {
      g->BeginFigure(P(.3f, .34f), D2D1_FIGURE_BEGIN_HOLLOW);
      g->AddLine(P(.7f, .34f)); g->AddLine(P(.3f, .72f)); g->AddLine(P(.7f, .72f));
      g->EndFigure(D2D1_FIGURE_END_OPEN);
    }, false);
    rt->DrawGeometry(z, Brush(C(0, 0, 0, .18f)), s * .1f, Round());
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -s * .015f) * old);
    rt->DrawGeometry(z, Brush(w), s * .1f, Round());
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(-35, P(.66f, .2f)) * old);
    rt->FillEllipse(D2D1::Ellipse(P(.66f, .2f), s * .085f, s * .04f), Brush(w));
    rt->SetTransform(old);
    z->Release();
    break;
  }
  }
  PopClip(rt);
  StrokeRound(rt, { r.left + .4f, r.top + .4f, r.right - .4f, r.bottom - .4f }, rad, RGBA(255, 255, 255, .22f * a), .8f);
  if (pressed) FillRound(rt, r, rad, RGBA(0, 0, 0, .3f * a));
  return true;
}
