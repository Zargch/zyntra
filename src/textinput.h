#pragma once
#include "common.h"

// Campo de texto minimo (sin controles de Windows): solo agrega/borra al final, con cursor parpadeante.
struct TextInput {
  std::wstring text;
  bool caret = true;

  // devuelve true si el texto cambio
  bool OnChar(wchar_t c) {
    if (c == 8) {  // backspace
      if (text.empty()) return false;
      text.pop_back();
      return true;
    }
    if (c == 127 || (c == 8 && (GetKeyState(VK_CONTROL) & 0x8000))) { text.clear(); return true; }
    if (c < 32) return false;
    text.push_back(c);
    return true;
  }

  bool Paste(HWND h) {
    if (!OpenClipboard(h)) return false;
    bool ok = false;
    if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) {
      if (auto p = (const wchar_t*)GlobalLock(d)) {
        for (const wchar_t* c = p; *c && *c != L'\r' && *c != L'\n'; c++) text.push_back(*c);
        ok = true;
        GlobalUnlock(d);
      }
    }
    CloseClipboard();
    return ok;
  }

  void Draw(ID2D1RenderTarget* rt, D2D1_RECT_F r, IDWriteTextFormat* f, const std::wstring& placeholder, D2D1_COLOR_F c, D2D1_COLOR_F dim, Align a = AL_LEFT) {
    if (text.empty()) {
      DrawText(rt, placeholder, f, r, dim, a);
      if (caret) {
        float x = a == AL_CENTER ? (r.left + r.right) / 2 - TextWidth(placeholder, f) / 2 - 2 : r.left;
        float hh = f->GetFontSize() * .62f, cy = (r.top + r.bottom) / 2;
        rt->FillRectangle({ x, cy - hh, x + 1.5f, cy + hh }, Brush(g_th.accent));
      }
      return;
    }
    DrawText(rt, text, f, r, c, a);
    if (caret) {
      float tw = TextWidth(text, f);
      float x = a == AL_CENTER ? (r.left + r.right) / 2 + tw / 2 + 1 : r.left + tw + 1;
      float hh = f->GetFontSize() * .62f, cy = (r.top + r.bottom) / 2;
      rt->FillRectangle({ x, cy - hh, x + 1.5f, cy + hh }, Brush(g_th.accent));
    }
  }
};
