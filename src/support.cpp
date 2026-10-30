// Utilidades pequenas que Zyntra comparte con el resto del codigo: HTTP, texto, dialogos y carpetas.
#include "common.h"
#include <winhttp.h>

std::wstring KnownPath(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r;
}

// GET por HTTPS; devuelve el cuerpo o "" si falla
std::string HttpGet(const wchar_t* host, const std::wstring& path) {
  std::string out;
  HINTERNET s = WinHttpOpen(L"Zyntra/" ZYNTRA_VERSION_WSTR L" (+https://github.com/Zargch/zyntra)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
  if (!s) return out;
  HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
  if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD n;
    while (WinHttpQueryDataAvailable(r, &n) && n) {
      std::string buf(n, 0);
      DWORD got = 0;
      if (!WinHttpReadData(r, buf.data(), n, &got)) break;
      out.append(buf.data(), got);
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return out;
}

std::wstring Utf8(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

bool ConfirmBox(const wchar_t* text) {
  return MessageBoxW(nullptr, text, L"Zyntra", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDYES;
}

// cuadro de texto modal de una linea; "" si se cancela
std::wstring InputBox(const wchar_t* title, const wchar_t* prompt, const std::wstring& def) {
  static std::wstring result;
  static bool done, ok;
  WNDCLASSW wc{};
  wc.lpfnWndProc = [](HWND h, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
      ok = LOWORD(wp) == IDOK;
      wchar_t b[256]; GetDlgItemTextW(h, 100, b, 256); result = b;
      done = true; return 0;
    }
    if (m == WM_CLOSE) { done = true; ok = false; return 0; }
    return DefWindowProcW(h, m, wp, lp);
  };
  wc.hInstance = g_inst; wc.lpszClassName = L"ZyntraInput"; wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  int W = Px(380), H = Px(150);
  RECT m = MonitorRect();
  HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, wc.lpszClassName, title, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                           (m.left + m.right - W) / 2, (m.top + m.bottom - H) / 3, W, H, nullptr, nullptr, g_inst, nullptr);
  HFONT f = CreateFontW(-Px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int x, int y, int w, int hh, int id) {
    HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | st, Px(x), Px(y), Px(w), Px(hh), h, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
  };
  mk(L"STATIC", prompt, 0, 14, 12, 350, 20, 0);
  HWND e = mk(L"EDIT", def.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 14, 38, 340, 26, 100);
  mk(L"BUTTON", L"Aceptar", WS_TABSTOP | BS_DEFPUSHBUTTON, 174, 76, 86, 28, IDOK);
  mk(L"BUTTON", L"Cancelar", WS_TABSTOP, 268, 76, 86, 28, IDCANCEL);
  ShowWindow(h, SW_SHOW);
  SetForegroundWindow(h);
  SetFocus(e);
  SendMessageW(e, EM_SETSEL, 0, -1);
  done = false; ok = false;
  MSG msg;
  while (!done && GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  }
  DestroyWindow(h);
  DeleteObject(f);
  return ok ? result : L"";
}

// "Acerca de": version, autor y licencia
void ShowAbout(HWND owner) {
  std::wstring t = L"Zyntra " ZYNTRA_VERSION_WSTR L"\n"
                   L"Launcher de juegos para Windows.\n\n"
                   L"Creado por Zargch.\n"
                   L"Código abierto bajo licencia MIT.\n"
                   L"https://github.com/Zargch/zyntra\n\n"
                   L"Zyntra no está afiliado a Valve, Epic Games, Microsoft ni Riot Games.\n"
                   L"Las marcas, carátulas y videos pertenecen a sus dueños.\n\n"
                   L"¿Abrir la página del proyecto?";
  if (MessageBoxW(owner, t.c_str(), L"Acerca de Zyntra", MB_YESNO | MB_ICONINFORMATION) == IDYES)
    RunShell(L"https://github.com/Zargch/zyntra");
}
