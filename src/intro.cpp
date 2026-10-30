// Animacion de inicio: intro.mov (junto al exe) en una ventana sin bordes, con sonido.
// Clic, Esc, Enter o Espacio la saltan. Se desactiva con [Zyntra] intro=0 en config.ini.
// El reproductor (MFPlay) se libera al terminar, asi no queda memoria de video en el proceso.
#include "common.h"
#include <mfapi.h>
#include <mfplay.h>
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")

enum { WM_INTRO_END = WM_APP + 60, TIMER_GUARD = 1, TIMER_SHOW = 2 };

static HWND s_wnd = nullptr;
static IMFPMediaPlayer* s_player = nullptr;
static std::function<void()> s_done;
static bool s_shown = false;

struct IntroCallback : IMFPMediaPlayerCallback {
  STDMETHODIMP QueryInterface(REFIID r, void** p) override {
    if (r == __uuidof(IUnknown) || r == __uuidof(IMFPMediaPlayerCallback)) { *p = this; return S_OK; }
    *p = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }   // objeto estatico
  STDMETHODIMP_(ULONG) Release() override { return 1; }
  void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* e) override {
    // se muestra la ventana cuando ya hay imagen: sin destello negro al abrir
    if (e->eEventType == MFP_EVENT_TYPE_PLAY && SUCCEEDED(e->hrEvent)) SetTimer(s_wnd, TIMER_SHOW, 30, nullptr);
    // fin o error: se cierra fuera del callback (MFPlay no admite Shutdown dentro de el)
    if (e->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED || FAILED(e->hrEvent)) PostMessageW(s_wnd, WM_INTRO_END, 0, 0);
  }
};
static IntroCallback s_cb;

static void ShowIntroWindow() {
  if (s_shown || !s_wnd) return;
  s_shown = true;
  ShowWindow(s_wnd, SW_SHOW);
  SetForegroundWindow(s_wnd);
}

static void Finish() {
  if (!s_wnd) return;
  HWND w = s_wnd;
  s_wnd = nullptr;
  KillTimer(w, TIMER_GUARD);
  KillTimer(w, TIMER_SHOW);
  if (s_player) { s_player->Shutdown(); s_player->Release(); s_player = nullptr; }
  MFShutdown();
  // la ventana principal se crea antes de cerrar la intro: el foco pasa directo a ella
  auto done = std::move(s_done);
  s_done = nullptr;
  if (done) done();
  DestroyWindow(w);
  TrimMemory();
}

static LRESULT CALLBACK IntroProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
  case WM_INTRO_END: Finish(); return 0;
  case WM_TIMER:
    if (wp == TIMER_SHOW) { KillTimer(h, TIMER_SHOW); ShowIntroWindow(); }
    else Finish();  // TIMER_GUARD: el video no termino a tiempo
    return 0;
  case WM_LBUTTONUP: case WM_RBUTTONUP: Finish(); return 0;
  case WM_KEYDOWN:
    if (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_SPACE) Finish();
    return 0;
  case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
  case WM_ERASEBKGND: return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
    EndPaint(h, &ps);
    if (s_player) s_player->UpdateVideo();
    return 0;
  }
  case WM_CLOSE: Finish(); return 0;
  }
  return DefWindowProcW(h, m, wp, lp);
}

static std::wstring IntroFile() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring d = exe;
  return d.substr(0, d.find_last_of(L'\\')) + L"\\intro.mov";
}

bool Intro_Play(std::function<void()> done) {
  if (!GetPrivateProfileIntW(L"Zyntra", L"intro", 1, (g_dir + L"\\config.ini").c_str())) return false;
  std::wstring file = IntroFile();
  if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

  WNDCLASSW wc{};
  wc.lpfnWndProc = IntroProc; wc.hInstance = g_inst; wc.lpszClassName = L"ZyntraIntro";
  wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
  RegisterClassW(&wc);
  // 16:9 centrado, del 60 % del ancho de la pantalla
  RECT mon = MonitorRect();
  int mw = mon.right - mon.left, mh = mon.bottom - mon.top;
  int w = std::min(mw * 6 / 10, mh * 16 / 9 * 8 / 10), h = w * 9 / 16;
  s_wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Zyntra", WS_POPUP, mon.left + (mw - w) / 2, mon.top + (mh - h) / 2, w, h,
                          nullptr, nullptr, g_inst, nullptr);
  if (!s_wnd) return false;
  int corner = 2;  // DWMWCP_ROUND: esquinas redondeadas y sombra de Windows 11
  DwmSetWindowAttribute(s_wnd, 33, &corner, sizeof corner);

  s_done = std::move(done);
  MFStartup(MF_VERSION);
  if (FAILED(MFPCreateMediaPlayer(file.c_str(), TRUE, 0, &s_cb, s_wnd, &s_player))) {
    s_player = nullptr;
    MFShutdown();
    s_done = nullptr;
    DestroyWindow(s_wnd);
    s_wnd = nullptr;
    return false;
  }
  SetTimer(s_wnd, TIMER_SHOW, 1500, nullptr);    // si el evento de PLAY no llega, se muestra igual
  SetTimer(s_wnd, TIMER_GUARD, 15000, nullptr);  // nunca bloquea el arranque
  return true;
}
