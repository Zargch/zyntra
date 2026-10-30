// Zyntra - launcher de juegos para Windows.
// Creado por Zargch (https://github.com/Zargch). Codigo abierto bajo licencia MIT (ver LICENSE).
#include "common.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "winhttp.lib")

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
  // deja rastro en %LOCALAPPDATA%\Zyntra\crash.log (codigo y direccion relativa al exe)
  std::wstring f = g_dir + L"\\crash.log";
  HANDLE h = CreateFileW(f.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    SYSTEMTIME st; GetLocalTime(&st);
    char line[256];
    ULONG_PTR addr = ep ? (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress : 0, base = (ULONG_PTR)GetModuleHandleW(nullptr);
    int n = sprintf_s(line, "%04d-%02d-%02d %02d:%02d:%02d  codigo=0x%08lX  exe+0x%llX  version=" ZYNTRA_VERSION_STR "\r\n", st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond, ep ? ep->ExceptionRecord->ExceptionCode : 0, (unsigned long long)(addr - base));
    DWORD w; WriteFile(h, line, n, &w, nullptr);
    CloseHandle(h);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  switch (m) {
  case WM_APP_RENDER: Menu_OnMessage(wp); return 0;
  case WM_APP_ICONREADY: GameMode_Invalidate(); return 0;
  case WM_APP_CATALOG: {
    auto v = (std::vector<AppInfo>*)lp;
    g_apps = std::move(*v);
    delete v;
    g_catalogReady = true;
    return 0;
  }
  }
  return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  g_inst = inst;
  HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Zargch.Zyntra.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {  // ya esta abierto: traerlo al frente
    HWND w = FindWindowW(L"ZyntraWindow", nullptr);
    if (w) { if (IsIconic(w)) ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
    return 0;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // datos del usuario fuera de la carpeta de instalacion (Program Files es de solo lectura)
  g_dir = KnownPath(FOLDERID_LocalAppData) + L"\\Zyntra";
  CreateDirectoryW(g_dir.c_str(), nullptr);
  CreateDirectoryW((g_dir + L"\\icons").c_str(), nullptr);
  SetUnhandledExceptionFilter(CrashHandler);
  OleInitialize(nullptr);  // STA + arrastrar y soltar
  g_scale = GetDpiForSystem() / 96.f;
  ID2D1Factory1* f1 = nullptr;
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f1);
  g_d2d = f1;
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
  if (!g_d2d || !g_dw || !g_wic) {
    MessageBoxW(nullptr, L"Zyntra necesita Windows 10 o 11 con Direct2D.", L"Zyntra", MB_ICONERROR);
    return 1;
  }
  // render por software: evita cargar el driver de la GPU (decenas de MB) en este proceso
  auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * g_scale, 96 * g_scale);
  g_d2d->CreateDCRenderTarget(&props, &g_rt);
  g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  g_rt->QueryInterface(&g_dc);
  LoadConfig();
  LoadTheme();

  WNDCLASSW wc{};
  wc.lpfnWndProc = MsgProc; wc.hInstance = inst; wc.lpszClassName = L"ZyntraMsg";
  RegisterClassW(&wc);
  g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Zyntra", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
  StartCatalog();  // para abrir los juegos de Xbox por su entrada de aplicacion
  SetCurrentProcessExplicitAppUserModelID(L"Zargch.Zyntra");  // la intro ya se agrupa con Zyntra en la barra de tareas
  if (!Intro_Play([] { GameMode_RunStandalone(); })) GameMode_RunStandalone();
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  ReleaseMutex(mtx);
  return 0;
}
