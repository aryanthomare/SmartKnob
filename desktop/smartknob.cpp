// SmartKnob desktop companion: tray icon + small panel to pick what the knob does and how it feels.
//
// Native Win32, statically linked, no runtime to install. Event driven: volume changes come from
// Core Audio callbacks, focus changes from a WinEvent hook and USB plug-ins from WM_DEVICECHANGE,
// so it does next to nothing while idle.
//
// The panel and the volume popup use a Liquid Glass style material rendered in software: the screen
// behind the window is captured (the windows are excluded from capture, so they never see
// themselves), blurred, bent at the rim like a lens and lit with a specular edge. It only refreshes
// while a window is visible, and only re-renders when what's behind it changed.
//
// Talks to the knob with `CMD key=value` lines (see volume_knob/src/main.cpp) over its USB CDC
// serial port, or over Bluetooth LE (Nordic UART Service) once the knob is paired in Windows
// Bluetooth settings. USB wins when both are there. Build with build.bat. `SmartKnob.exe --snapshot <dir>` renders
// the panel and popup over the current screen to PNGs and exits.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <setupapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "user32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "advapi32")
#pragma comment(lib, "setupapi")
#pragma comment(lib, "gdiplus")
#pragma comment(lib, "windowsapp")

using namespace Gdiplus;
namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

// ---------------------------------------------------------------- tuning
static const float MAX_DETENT_V = 1.5f;  // click strength slider at 100 %
static const float WALL_K = 0.08f;       // end-wall spring, volts per degree past the end
static const DWORD TURN_HOLDOFF_MS = 600;  // ignore PC-side changes this long after the knob was turned
static const DWORD HANDSHAKE_MS = 1500, RX_TIMEOUT_MS = 3000;
static const int ESPRESSIF_VID = 0x303A, USB_JTAG_PID = 0x1001;  // 0x1001 = flashing port, never opened
static const GUID PORTS_CLASS = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
static const GUID OUR_AUDIO_CTX = {0x5b1f6e2a, 0x93c4, 0x4d1e, {0x8a, 0x52, 0x3e, 0x71, 0x0c, 0x9d, 0x24, 0xf6}};

enum : UINT {
  WM_APP_LINE = WM_APP + 1,  // wParam = link generation, lParam = std::string*
  WM_APP_LINKERR,            // wParam = link generation
  WM_APP_BLE,                // lParam = BleLink* from a connect attempt (null if it failed)
  WM_APP_VOLUME,             // wParam = master volume %, changed outside this app
  WM_APP_DEVICE,             // default playback device changed
  WM_APP_TRAY,
  WM_APP_SHOWPANEL,
};
enum { T_LINK = 1, T_APP, T_RESYNC, T_OSD_HIDE, T_ANIM, T_GLASS };
enum { ID_OPEN = 100, ID_MODE0 = 110, ID_RELEASE = 130, ID_AUTOSTART, ID_QUIT };

// ---------------------------------------------------------------- modes
enum { M_VOLUME, M_APP, M_SCROLL, M_ZOOM, M_ARROWS, M_COUNT };
struct ModeDef {
  const wchar_t *key, *name, *subtitle;
  int lo, hi;
  float step;     // knob degrees per value
  int defDetent;  // % of MAX_DETENT_V
  bool relative;  // unbounded: only the change matters
};
static const int REL = 1000000;
static const ModeDef MODES[M_COUNT] = {
    {L"volume", L"Volume", L"System volume", 0, 100, 2.5f, 0, false},
    {L"appvolume", L"App volume", nullptr, 0, 100, 2.5f, 0, false},
    {L"scroll", L"Scroll", L"Scrolls what’s under the pointer", -REL, REL, 4.0f, 25, true},
    {L"zoom", L"Zoom", L"Zooms what’s under the pointer", -REL, REL, 8.0f, 35, true},
    {L"arrows", L"Arrow keys", L"Presses ← and → in the active app", -REL, REL, 6.0f, 45, true},
};

// ---------------------------------------------------------------- state
static HINSTANCE g_inst;
static HWND g_main, g_panel, g_osd;
static UINT g_taskbarCreated;
static std::wstring g_ini;

static int g_mode = M_VOLUME;
static int g_value = 0;
static int g_detent[M_COUNT];
static DWORD g_lastTurn = 0;

// link
static HANDLE g_port = INVALID_HANDLE_VALUE, g_reader = nullptr, g_writeEvent = nullptr;
static volatile LONG g_readerStop = 0;
static UINT g_gen = 0;
static std::wstring g_portName;
static bool g_alive = false, g_paused = false, g_haptics = false, g_scanNow = true;
static float g_vm = 0;
static DWORD g_openedAt = 0, g_lastRx = 0;
static int g_scanTick = 0;
struct BleLink {
  gatt::GattSession session{nullptr};
  gatt::GattDeviceService svc{nullptr};
  gatt::GattCharacteristic rx{nullptr}, tx{nullptr};
  winrt::event_token tok{};
};
static std::unique_ptr<BleLink> g_ble;  // set while the link runs over Bluetooth
static bool g_bleBusy = false;           // a connect attempt is in flight
static std::map<std::wstring, DWORD> g_skipUntil;

// audio
static IMMDeviceEnumerator *g_devEnum;
static IAudioEndpointVolume *g_epVol;
static IAudioSessionManager2 *g_sessMgr;
static std::vector<ISimpleAudioVolume *> g_appVols;
static std::wstring g_appTarget, g_fgExe;

// system appearance
static bool g_sysDark = true, g_transparency = true, g_animations = true, g_highContrast = false;

// ui
static NOTIFYICONDATAW g_nid;
static HICON g_iconOn, g_iconOff;
static std::wstring g_lastTip;
static int g_hover = -1;  // 0..4 mode buttons, H_CLOSE, H_SLIDER
static bool g_sliderDrag = false, g_tracking = false;
static float g_panelScale = 1;
static POINT g_osdPos;
enum { H_CLOSE = 10, H_SLIDER = 11 };

static void refreshUi();
static void showOsd();

// ---------------------------------------------------------------- settings
static std::wstring iniGet(const wchar_t *key, const wchar_t *def) {
  wchar_t buf[256];
  GetPrivateProfileStringW(L"SmartKnob", key, def, buf, 256, g_ini.c_str());
  return buf;
}
static int iniGetInt(const wchar_t *key, int def) {
  std::wstring s = iniGet(key, L"");
  return s.empty() ? def : _wtoi(s.c_str());
}
static void iniSet(const wchar_t *key, int v) {
  WritePrivateProfileStringW(L"SmartKnob", key, std::to_wstring(v).c_str(), g_ini.c_str());
}
static void iniSet(const wchar_t *key, const wchar_t *v) { WritePrivateProfileStringW(L"SmartKnob", key, v, g_ini.c_str()); }
static std::wstring detentKey(int m) { return std::wstring(L"detent_") + MODES[m].key; }

static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool autostartEnabled() {
  return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"SmartKnob", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
static void setAutostart(bool on) {
  if (on) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"SmartKnob", REG_SZ, cmd.c_str(), (DWORD)((cmd.size() + 1) * 2));
  } else {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"SmartKnob");
  }
}

static DWORD regDword(const wchar_t *path, const wchar_t *name, DWORD def) {
  DWORD v = def, sz = sizeof(v);
  RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
  return v;
}
static void readSystemAppearance() {
  const wchar_t *personalize = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
  g_sysDark = regDword(personalize, L"AppsUseLightTheme", 1) == 0;
  g_transparency = regDword(personalize, L"EnableTransparency", 1) != 0;
  BOOL anim = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
  g_animations = anim != FALSE;
  HIGHCONTRASTW hc = {sizeof(hc)};
  SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
  g_highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

// ---------------------------------------------------------------- audio
struct VolumeCallback : IAudioEndpointVolumeCallback {
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }  // static object
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **pp) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
      *pp = this;
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
    if (d->guidEventContext != OUR_AUDIO_CTX)
      PostMessageW(g_main, WM_APP_VOLUME, (WPARAM)lroundf(d->fMasterVolume * 100), 0);
    return S_OK;
  }
} g_volCb;

struct DeviceCallback : IMMNotificationClient {
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **pp) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *pp = this;
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) PostMessageW(g_main, WM_APP_DEVICE, 0, 0);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
} g_devCb;

template <class T> static void release(T *&p) {
  if (p) p->Release();
  p = nullptr;
}
static void releaseAppVols() {
  for (auto *v : g_appVols) v->Release();
  g_appVols.clear();
}

static void acquireEndpoint() {
  if (g_epVol) g_epVol->UnregisterControlChangeNotify(&g_volCb);
  release(g_epVol);
  release(g_sessMgr);
  releaseAppVols();
  g_appTarget.clear();
  IMMDevice *dev = nullptr;
  if (!g_devEnum || FAILED(g_devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return;
  if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void **)&g_epVol)))
    g_epVol->RegisterControlChangeNotify(&g_volCb);
  dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void **)&g_sessMgr);
  dev->Release();
}

static int masterGet() {
  float f;
  if (!g_epVol || FAILED(g_epVol->GetMasterVolumeLevelScalar(&f))) return -1;
  return (int)lroundf(f * 100);
}
static void masterSet(int pct) {
  if (!g_epVol) return;
  g_epVol->SetMasterVolumeLevelScalar(std::clamp(pct, 0, 100) / 100.0f, &OUR_AUDIO_CTX);
  BOOL muted = FALSE;
  if (pct > 0 && SUCCEEDED(g_epVol->GetMute(&muted)) && muted) g_epVol->SetMute(FALSE, &OUR_AUDIO_CTX);
}

static std::wstring exeOfPid(DWORD pid) {
  std::wstring r;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return r;
  wchar_t buf[MAX_PATH];
  DWORD n = MAX_PATH;
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
    const wchar_t *s = wcsrchr(buf, L'\\');
    r = s ? s + 1 : buf;
  }
  CloseHandle(p);
  return r;
}

// All audio sessions of one executable (e.g. every chrome.exe process).
static std::vector<ISimpleAudioVolume *> sessionsOf(const std::wstring &exe) {
  std::vector<ISimpleAudioVolume *> out;
  IAudioSessionEnumerator *en = nullptr;
  if (!g_sessMgr || exe.empty() || FAILED(g_sessMgr->GetSessionEnumerator(&en))) return out;
  int n = 0;
  en->GetCount(&n);
  for (int i = 0; i < n; i++) {
    IAudioSessionControl *c = nullptr;
    IAudioSessionControl2 *c2 = nullptr;
    if (FAILED(en->GetSession(i, &c))) continue;
    DWORD pid = 0;
    if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&c2))) {
      c2->GetProcessId(&pid);
      c2->Release();
    }
    ISimpleAudioVolume *v = nullptr;
    if (pid && _wcsicmp(exeOfPid(pid).c_str(), exe.c_str()) == 0 &&
        SUCCEEDED(c->QueryInterface(__uuidof(ISimpleAudioVolume), (void **)&v)))
      out.push_back(v);
    c->Release();
  }
  en->Release();
  return out;
}

static int appGet() {
  float f;
  for (auto *v : g_appVols)
    if (SUCCEEDED(v->GetMasterVolume(&f))) return (int)lroundf(f * 100);
  return -1;
}
static void appSet(int pct) {
  for (auto *v : g_appVols) v->SetMasterVolume(std::clamp(pct, 0, 100) / 100.0f, &OUR_AUDIO_CTX);
}

// Switch the app-volume target to the focused app if it plays audio. Returns true if it changed.
static bool followFocus() {
  if (g_fgExe.empty() || _wcsicmp(g_fgExe.c_str(), g_appTarget.c_str()) == 0) return false;
  auto vols = sessionsOf(g_fgExe);
  if (vols.empty()) return false;
  releaseAppVols();
  g_appVols = vols;
  g_appTarget = g_fgExe;
  return true;
}

static void updateForeground(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid && pid != GetCurrentProcessId()) g_fgExe = exeOfPid(pid);  // our own windows keep the target
}

// ---------------------------------------------------------------- input
static void sendWheel(int clicks, bool ctrl) {
  INPUT in[3] = {};
  int n = 0;
  if (ctrl) {
    in[n].type = INPUT_KEYBOARD;
    in[n++].ki.wVk = VK_CONTROL;
  }
  in[n].type = INPUT_MOUSE;
  in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
  in[n++].mi.mouseData = (DWORD)(clicks * WHEEL_DELTA);
  if (ctrl) {
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = VK_CONTROL;
    in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
  }
  SendInput(n, in, sizeof(INPUT));
}
static void tapKey(WORD vk, int times) {
  for (int i = 0; i < times; i++) {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    in[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
  }
}

// ---------------------------------------------------------------- serial link
struct PortInfo {
  std::wstring name;
  int vid = -1, pid = -1;
};

static std::vector<PortInfo> listPorts() {
  std::vector<PortInfo> out;
  HDEVINFO set = SetupDiGetClassDevsW(&PORTS_CLASS, nullptr, nullptr, DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) return out;
  SP_DEVINFO_DATA d = {sizeof(d)};
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
    PortInfo p;
    wchar_t hw[512] = {};
    SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, (BYTE *)hw, sizeof(hw) - 4, nullptr);
    if (const wchar_t *v = wcsstr(hw, L"VID_")) p.vid = (int)wcstol(v + 4, nullptr, 16);
    if (const wchar_t *q = wcsstr(hw, L"PID_")) p.pid = (int)wcstol(q + 4, nullptr, 16);
    HKEY k = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (k != INVALID_HANDLE_VALUE) {
      wchar_t nm[64] = {};
      DWORD sz = sizeof(nm) - 2, type = 0;
      if (RegQueryValueExW(k, L"PortName", nullptr, &type, (BYTE *)nm, &sz) == ERROR_SUCCESS && type == REG_SZ)
        p.name = nm;
      RegCloseKey(k);
    }
    if (!p.name.empty()) out.push_back(p);
  }
  SetupDiDestroyDeviceInfoList(set);
  return out;
}

// Posts each complete line in `buf` to the main window (any thread) and keeps the remainder.
static void postLines(std::string &buf, UINT gen) {
  size_t p;
  while ((p = buf.find('\n')) != std::string::npos) {
    std::string line = buf.substr(0, p);
    buf.erase(0, p + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) PostMessageW(g_main, WM_APP_LINE, gen, (LPARAM) new std::string(line));
  }
  if (buf.size() > 4096) buf.clear();
}

static DWORD WINAPI readerThread(void *arg) {
  const UINT gen = (UINT)(UINT_PTR)arg;
  OVERLAPPED ov = {};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::string buf;
  char tmp[256];
  while (!g_readerStop) {
    ResetEvent(ov.hEvent);
    DWORD n = 0;
    if (!ReadFile(g_port, tmp, sizeof(tmp), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
    if (!GetOverlappedResult(g_port, &ov, &n, TRUE)) break;
    buf.append(tmp, n);
    postLines(buf, gen);
  }
  if (!g_readerStop) PostMessageW(g_main, WM_APP_LINKERR, gen, 0);
  CloseHandle(ov.hEvent);
  return 0;
}

static void linkClose() {
  if (g_port == INVALID_HANDLE_VALUE && !g_ble) return;
  if (g_port != INVALID_HANDLE_VALUE) {
    InterlockedExchange(&g_readerStop, 1);
    CancelIoEx(g_port, nullptr);
    WaitForSingleObject(g_reader, 2000);
    CloseHandle(g_reader);
    CloseHandle(g_port);
    g_reader = nullptr;
    g_port = INVALID_HANDLE_VALUE;
  }
  if (g_ble) {
    try {
      g_ble->tx.ValueChanged(g_ble->tok);
      g_ble->svc.Close();
      g_ble->session.Close();
    } catch (...) {  // device already gone
    }
    g_ble.reset();
  }
  g_gen++;
  bool was = g_alive;
  g_alive = g_haptics = false;
  if (was) refreshUi();
}

static void linkWrite(const char *s) {
  if (g_ble) {
    // Fire and forget: never block the UI thread on Bluetooth; failures come back as WM_APP_LINKERR.
    const UINT gen = g_gen;
    size_t len = strlen(s), chunk = std::max<size_t>(20, g_ble->session.MaxPduSize() - 3);
    try {
      for (size_t o = 0; o < len; o += chunk) {
        winrt::Windows::Storage::Streams::DataWriter w;
        w.WriteBytes(winrt::array_view<const uint8_t>((const uint8_t *)s + o, (uint32_t)std::min(chunk, len - o)));
        g_ble->rx.WriteValueAsync(w.DetachBuffer(), gatt::GattWriteOption::WriteWithoutResponse)
            .Completed([gen](auto const &op, winrt::Windows::Foundation::AsyncStatus st) {
              if (st != winrt::Windows::Foundation::AsyncStatus::Completed ||
                  op.GetResults() != gatt::GattCommunicationStatus::Success)
                PostMessageW(g_main, WM_APP_LINKERR, gen, 0);
            });
      }
    } catch (...) {
      linkClose();
    }
    return;
  }
  if (g_port == INVALID_HANDLE_VALUE) return;
  OVERLAPPED ov = {};
  ov.hEvent = g_writeEvent;
  ResetEvent(g_writeEvent);
  DWORD n = 0, len = (DWORD)strlen(s);
  if (!WriteFile(g_port, s, len, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
    linkClose();
    return;
  }
  if (!GetOverlappedResult(g_port, &ov, &n, TRUE)) linkClose();
}

static void linkStarted(const std::wstring &name) {
  g_portName = name;
  g_alive = false;
  g_openedAt = GetTickCount();
  g_lastRx = 0;
  g_gen++;
}

static void linkOpen(const std::wstring &name) {
  std::wstring path = L"\\\\.\\" + name;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    g_skipUntil[name] = GetTickCount() + 5000;  // busy, e.g. a serial monitor has it
    return;
  }
  DCB dcb = {sizeof(dcb)};
  GetCommState(h, &dcb);
  dcb.BaudRate = 115200;
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = dcb.fDsrSensitivity = dcb.fOutX = dcb.fInX = FALSE;
  // DTR on (the ESP32 only transmits while DTR is set), RTS off: never forms the DTR/RTS
  // sequence that reboots the chip into the bootloader.
  dcb.fDtrControl = DTR_CONTROL_ENABLE;
  dcb.fRtsControl = RTS_CONTROL_DISABLE;
  SetCommState(h, &dcb);
  COMMTIMEOUTS to = {MAXDWORD, MAXDWORD, 1000, 0, 200};  // reads return as soon as any byte arrives
  SetCommTimeouts(h, &to);
  PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

  linkClose();  // drops Bluetooth when the cable shows up
  g_port = h;
  linkStarted(name);
  InterlockedExchange(&g_readerStop, 0);
  g_reader = CreateThread(nullptr, 0, readerThread, (void *)(UINT_PTR)g_gen, 0, nullptr);
  linkWrite("HELLO\n");
}

// ---- Bluetooth LE: the knob's Nordic UART Service, found among paired devices
static const winrt::guid NUS{0x6E400001, 0xB5A3, 0xF393, {0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E}};
static const winrt::guid NUS_RX{0x6E400002, 0xB5A3, 0xF393, {0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E}};
static const winrt::guid NUS_TX{0x6E400003, 0xB5A3, 0xF393, {0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E}};

// Worker thread: the WinRT calls block for seconds when the knob is off, so they stay off the UI.
static DWORD WINAPI bleConnectThread(void *) {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  BleLink *l = nullptr;
  try {
    using namespace winrt::Windows::Devices;
    auto found = Enumeration::DeviceInformation::FindAllAsync(gatt::GattDeviceService::GetDeviceSelectorFromUuid(NUS)).get();
    if (found.Size()) {
      auto svc = gatt::GattDeviceService::FromIdAsync(found.GetAt(0).Id()).get();
      auto charOf = [&](const winrt::guid &g) {
        auto r = svc.GetCharacteristicsForUuidAsync(g).get();
        return r.Status() == gatt::GattCommunicationStatus::Success && r.Characteristics().Size()
                   ? r.Characteristics().GetAt(0)
                   : nullptr;
      };
      auto rx = svc ? charOf(NUS_RX) : nullptr, tx = svc ? charOf(NUS_TX) : nullptr;
      if (rx && tx &&
          tx.WriteClientCharacteristicConfigurationDescriptorAsync(
                gatt::GattClientCharacteristicConfigurationDescriptorValue::Notify)
                  .get() == gatt::GattCommunicationStatus::Success) {
        auto session = gatt::GattSession::FromDeviceIdAsync(svc.Device().BluetoothDeviceId()).get();
        session.MaintainConnection(true);
        l = new BleLink{session, svc, rx, tx};
      } else if (svc) {
        svc.Close();  // paired but switched off or out of range
      }
    }
  } catch (...) {  // e.g. Bluetooth turned off
  }
  PostMessageW(g_main, WM_APP_BLE, 0, (LPARAM)l);
  winrt::uninit_apartment();
  return 0;
}

static void onBleConnected(BleLink *raw) {
  std::unique_ptr<BleLink> l(raw);
  g_bleBusy = false;
  if (!l) return;
  if (g_paused || g_port != INVALID_HANDLE_VALUE || g_ble) {  // USB won while we were connecting
    try {
      l->svc.Close();
      l->session.Close();
    } catch (...) {
    }
    return;
  }
  g_ble = std::move(l);
  linkStarted(L"Bluetooth");
  auto buf = std::make_shared<std::string>();  // notifications arrive in order on one thread-pool thread
  g_ble->tok = g_ble->tx.ValueChanged([buf, gen = g_gen](auto const &, gatt::GattValueChangedEventArgs const &e) {
    auto v = e.CharacteristicValue();
    buf->append((const char *)v.data(), v.Length());
    postLines(*buf, gen);
  });
  linkWrite("HELLO\n");
}

static void linkSendf(const char *fmt, ...) {
  if (!g_alive) return;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  linkWrite(buf);
}

// Opens the first free SmartKnob USB port. Returns true if one is now open.
static bool openUsbKnob(DWORD now) {
  for (const auto &p : listPorts()) {
    if (p.vid != ESPRESSIF_VID || p.pid == USB_JTAG_PID) continue;
    auto it = g_skipUntil.find(p.name);
    if (it != g_skipUntil.end() && (int)(it->second - now) > 0) continue;
    linkOpen(p.name);
    if (g_port != INVALID_HANDLE_VALUE) return true;
  }
  return false;
}

static void linkTick() {
  DWORD now = GetTickCount();
  if (g_port != INVALID_HANDLE_VALUE || g_ble) {
    if (g_ble && ++g_scanTick >= 10) {  // on Bluetooth: switch to USB when it's plugged in
      g_scanTick = 0;
      if (openUsbKnob(now)) return;
    }
    if (!g_alive) {
      if (now - g_openedAt > HANDSHAKE_MS) {
        g_skipUntil[g_portName] = now + 10000;  // not a SmartKnob
        linkClose();
        g_scanNow = true;
      }
    } else if (now - g_lastRx > RX_TIMEOUT_MS) {
      linkClose();
    } else {
      int vol = masterGet();
      if (vol >= 0)
        linkSendf("PING vol=%d\n", vol);
      else
        linkSendf("PING\n");
    }
    return;
  }
  if (g_paused || (++g_scanTick < 10 && !g_scanNow)) return;  // fallback scan every 5 s
  g_scanTick = 0;
  g_scanNow = false;
  if (openUsbKnob(now) || g_bleBusy) return;
  auto it = g_skipUntil.find(L"Bluetooth");
  if (it != g_skipUntil.end() && (int)(it->second - now) > 0) return;
  g_bleBusy = true;
  if (HANDLE t = CreateThread(nullptr, 0, bleConnectThread, nullptr, 0, nullptr))
    CloseHandle(t);
  else
    g_bleBusy = false;
}

static std::string argOf(const std::string &line, const char *key) {
  std::string k = std::string(" ") + key + "=";
  size_t p = line.find(k);
  if (p == std::string::npos) return "";
  p += k.size();
  return line.substr(p, line.find(' ', p) - p);
}

// ---------------------------------------------------------------- controller
static int initialValue() {
  switch (g_mode) {
    case M_VOLUME: return std::max(0, masterGet());
    case M_APP: {
      followFocus();
      int v = appGet();
      return v >= 0 ? v : 100;
    }
    default: return 0;
  }
}

static void pushCfg() {
  const ModeDef &m = MODES[g_mode];
  linkSendf("CFG lo=%d hi=%d val=%d step=%.2f det=%.3f wall=%.3f\n", m.lo, m.hi, g_value, m.step,
            g_detent[g_mode] / 100.0f * MAX_DETENT_V, WALL_K);
}

static void setMode(int m) {
  if (m < 0 || m >= M_COUNT) return;
  g_mode = m;
  iniSet(L"mode", MODES[m].key);
  g_value = initialValue();
  pushCfg();
  if (m == M_APP)
    SetTimer(g_main, T_APP, 1000, nullptr);
  else
    KillTimer(g_main, T_APP);
  refreshUi();
}

static void setDetent(int pct) {
  g_detent[g_mode] = std::clamp(pct, 0, 100);
  iniSet(detentKey(g_mode).c_str(), g_detent[g_mode]);
  pushCfg();
  refreshUi();
}

// The PC-side value changed elsewhere (keyboard keys, mixer, another app): move the knob's value.
static void syncExternal(int v) {
  if (v < 0 || v == g_value) return;
  if (GetTickCount() - g_lastTurn < TURN_HOLDOFF_MS) {
    SetTimer(g_main, T_RESYNC, TURN_HOLDOFF_MS + 100, nullptr);
    return;
  }
  g_value = v;
  linkSendf("VAL v=%d\n", v);
  refreshUi();
}

static void onKnobValue(int v) {
  int d = v - g_value;
  g_value = v;
  g_lastTurn = GetTickCount();
  if (!d) return;
  switch (g_mode) {
    case M_VOLUME: masterSet(v); break;
    case M_APP: appSet(v); break;
    case M_SCROLL: sendWheel(-d, false); break;  // clockwise scrolls down, like a wheel rolled towards you
    case M_ZOOM: sendWheel(d, true); break;
    case M_ARROWS: tapKey(d > 0 ? VK_RIGHT : VK_LEFT, std::abs(d)); break;
  }
  if (!MODES[g_mode].relative && !IsWindowVisible(g_panel)) showOsd();
  refreshUi();
}

static void onLine(const std::string &line) {
  g_lastRx = GetTickCount();
  std::string cmd = line.substr(0, line.find(' '));
  if (!g_alive) {
    if (cmd == "INFO" && argOf(line, "name") == "SmartKnob") {
      g_alive = true;
      g_haptics = argOf(line, "hap") == "1";
      g_value = initialValue();
      pushCfg();
      refreshUi();
    }
    return;
  }
  if (cmd == "VAL") {
    std::string v = argOf(line, "v");
    if (!v.empty()) onKnobValue(atoi(v.c_str()));
  } else if (cmd == "PONG") {
    bool hap = argOf(line, "hap") == "1";
    float vm = (float)atof(argOf(line, "vm").c_str());
    if (hap != g_haptics || fabsf(vm - g_vm) > 0.3f) {
      g_haptics = hap;
      g_vm = vm;
      refreshUi();
    }
  }
}

// ---------------------------------------------------------------- copy
static std::wstring appName() {  // "spotify.exe" -> "Spotify"
  std::wstring s = g_appTarget;
  if (s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0) s.resize(s.size() - 4);
  if (!s.empty()) s[0] = (wchar_t)towupper(s[0]);
  return s;
}
static std::wstring valueText() {
  if (MODES[g_mode].relative) return MODES[g_mode].name;
  if (g_mode == M_APP && g_appTarget.empty()) return L"–";
  return std::to_wstring(g_value) + L"%";
}
static std::wstring subtitle() {
  if (g_mode != M_APP) return MODES[g_mode].subtitle;
  return g_appTarget.empty() ? L"Click an app that’s playing sound" : appName();
}
static std::wstring statusText() {
  if (g_paused) return L"Disconnected for firmware update";
  if (!g_alive) return L"Plug in or pair your knob";
  return g_haptics ? L"Connected" : L"Connected · Plug in power for haptics";
}

// ---------------------------------------------------------------- glass material
// Captured, saturated and blurred screen content behind a window.
struct Backdrop {
  int w = 0, h = 0;           // capture size (= window size, px)
  int hw = 0, hh = 0;         // half-resolution blurred size
  std::vector<uint32_t> raw;  // BGRA, for change detection and snapshots
  std::vector<float> blur;    // half-res RGB
  uint64_t hash = 0;
  float lum = 0.5f;  // average luminance 0..1
  bool valid = false;
};

static void blurLine(const float *s, float *d, int n, int stride, int r) {
  float acc = 0, inv = 1.0f / (2 * r + 1);
  for (int i = -r; i <= r; i++) acc += s[std::clamp(i, 0, n - 1) * stride];
  for (int i = 0; i < n; i++) {
    d[i * stride] = acc * inv;
    acc += s[std::min(i + r + 1, n - 1) * stride] - s[std::max(i - r, 0) * stride];
  }
}

// Captures the screen under r. Returns true if it differs from the last capture.
static bool captureBackdrop(Backdrop &b, const RECT &r, float scale) {
  int w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return false;
  HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
  void *bits = nullptr;
  HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, dib);
  BOOL ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
  std::vector<uint32_t> raw((uint32_t *)bits, (uint32_t *)bits + (size_t)w * h);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (!ok) return false;

  uint64_t hash = 1469598103934665603ull;
  for (uint32_t px : raw) hash = (hash ^ (px & 0xFFFFFF)) * 1099511628211ull;
  if (b.valid && b.w == w && b.h == h && b.hash == hash) return false;
  b.raw.swap(raw);
  b.w = w;
  b.h = h;
  b.hash = hash;
  b.valid = true;

  // Half resolution, saturation boost (glass concentrates colour), then a 3-pass box blur.
  b.hw = (w + 1) / 2;
  b.hh = (h + 1) / 2;
  b.blur.assign((size_t)b.hw * b.hh * 3, 0);
  double lumSum = 0;
  for (int j = 0; j < b.hh; j++)
    for (int i = 0; i < b.hw; i++) {
      float c[3] = {0, 0, 0};
      for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
          uint32_t px = b.raw[(size_t)std::min(2 * j + dy, h - 1) * w + std::min(2 * i + dx, w - 1)];
          c[0] += (px >> 16) & 255;
          c[1] += (px >> 8) & 255;
          c[2] += px & 255;
        }
      float l = (0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]) / 4;
      lumSum += l;
      float *o = &b.blur[((size_t)j * b.hw + i) * 3];
      for (int k = 0; k < 3; k++) o[k] = std::clamp(l + (c[k] / 4 - l) * 1.45f, 0.0f, 255.0f);
    }
  b.lum = (float)(lumSum / ((double)b.hw * b.hh) / 255.0);
  int rad = std::max(1, (int)lroundf(2.5f * scale));
  std::vector<float> tmp(b.blur.size());
  for (int pass = 0; pass < 3; pass++) {
    for (int j = 0; j < b.hh; j++)
      for (int k = 0; k < 3; k++)
        blurLine(&b.blur[(size_t)j * b.hw * 3 + k], &tmp[(size_t)j * b.hw * 3 + k], b.hw, 3, rad);
    for (int i = 0; i < b.hw; i++)
      for (int k = 0; k < 3; k++)
        blurLine(&tmp[(size_t)i * 3 + k], &b.blur[(size_t)i * 3 + k], b.hh, b.hw * 3, rad);
  }
  return true;
}

static void sampleBlur(const Backdrop &b, float x, float y, float *rgb) {
  float u = std::clamp(x * 0.5f - 0.5f, 0.0f, b.hw - 1.001f), v = std::clamp(y * 0.5f - 0.5f, 0.0f, b.hh - 1.001f);
  int i0 = (int)u, j0 = (int)v, i1 = std::min(i0 + 1, b.hw - 1), j1 = std::min(j0 + 1, b.hh - 1);
  float fu = u - i0, fv = v - j0;
  const float *p00 = &b.blur[((size_t)j0 * b.hw + i0) * 3], *p10 = &b.blur[((size_t)j0 * b.hw + i1) * 3];
  const float *p01 = &b.blur[((size_t)j1 * b.hw + i0) * 3], *p11 = &b.blur[((size_t)j1 * b.hw + i1) * 3];
  for (int k = 0; k < 3; k++)
    rgb[k] = (p00[k] * (1 - fu) + p10[k] * fu) * (1 - fv) + (p01[k] * (1 - fu) + p11[k] * fu) * fv;
}

struct Shape {
  float x, y, w, h, r;  // px
};
// Signed distance to a rounded rectangle (negative inside) and the outward normal.
static float sdShape(const Shape &s, float px, float py, float &nx, float &ny) {
  float dx = px - (s.x + s.w / 2), dy = py - (s.y + s.h / 2);
  float sx = dx < 0 ? -1.0f : 1.0f, sy = dy < 0 ? -1.0f : 1.0f;
  float qx = fabsf(dx) - (s.w / 2 - s.r), qy = fabsf(dy) - (s.h / 2 - s.r);
  if (qx > 0 && qy > 0) {
    float l = sqrtf(qx * qx + qy * qy);
    nx = sx * qx / l;
    ny = sy * qy / l;
    return l - s.r;
  }
  if (qx > qy) {
    nx = sx;
    ny = 0;
    return qx - s.r;
  }
  nx = 0;
  ny = sy;
  return qy - s.r;
}

// Paints the glass body, its lensing rim, specular edge and drop shadow into a premultiplied BGRA
// buffer. bd == nullptr gives the opaque (reduced transparency / high contrast) material.
static void composeGlass(uint32_t *dst, int W, int H, const Backdrop *bd, const Shape &sh, float s, bool dark,
                         float lensK) {
  const float bezel = 16 * s, lens = 10 * s * lensK, rimW = 1.4f * s;
  const float Lx = -0.5547f, Ly = -0.8321f;  // light from the top left
  const float base[3] = {dark ? 30.0f : 248.0f, dark ? 30.0f : 248.0f, dark ? 34.0f : 250.0f};
  float dim = 1.0f;  // how much of the material tint covers the backdrop (keeps text legible)
  if (bd) dim = dark ? std::clamp(0.34f + 0.40f * bd->lum, 0.34f, 0.74f) : std::clamp(0.40f + 0.36f * (1 - bd->lum), 0.40f, 0.76f);
  const float shadowK = dark ? 0.36f : 0.20f, shadowSigma = 11 * s;
  Shape shadow = sh;
  shadow.y += 5 * s;

  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      float px = x + 0.5f, py = y + 0.5f, nx, ny;
      float sd = sdShape(sh, px, py, nx, ny);
      float cov = std::clamp(0.5f - sd, 0.0f, 1.0f);
      float sa = 0;
      if (cov < 1) {
        float n2x, n2y, t = std::max(sdShape(shadow, px, py, n2x, n2y), 0.0f) / shadowSigma;
        sa = t < 3 ? shadowK * expf(-t * t) : 0;
      }
      uint32_t &out = dst[(size_t)y * W + x];
      if (cov <= 0) {
        out = (uint32_t)(sa * 255) << 24;
        continue;
      }
      float e = -sd, edge = e < bezel ? 1 - e / bezel : 0;  // 1 at the rim, 0 past the bezel
      float c[3];
      if (bd) {
        float d = edge * edge * lens;  // lensing: the rim bends the backdrop inwards
        sampleBlur(*bd, px - nx * d, py - ny * d, c);
        if (d > 0.5f) {  // slight dispersion at the rim
          float t[3];
          sampleBlur(*bd, px - nx * d * 1.12f, py - ny * d * 1.12f, t);
          c[0] = t[0];
          sampleBlur(*bd, px - nx * d * 0.88f, py - ny * d * 0.88f, t);
          c[2] = t[2];
        }
        for (int k = 0; k < 3; k++) c[k] = c[k] * (1 - dim) + base[k] * dim;
      } else {
        float g = (dark ? 6.0f : 4.0f) * (0.5f - py / H);
        for (int k = 0; k < 3; k++) c[k] = base[k] + g;
      }
      // Specular rim: bright where the edge faces the light, a softer glint on the opposite side.
      float ld = nx * Lx + ny * Ly, light = 0;
      float rim = std::clamp(1 - e / rimW, 0.0f, 1.0f);
      if (rim > 0) {
        float f = std::max(ld, 0.0f), b2 = std::max(-ld, 0.0f);
        light += rim * (0.10f + 0.62f * f * sqrtf(f) + 0.26f * b2 * sqrtf(b2));
      }
      if (edge > 0) light += edge * edge * edge * 0.12f * std::max(ld, 0.0f);  // inner glow under the lit edge
      light *= dark ? 1.0f : 0.8f;
      float a = cov + sa * (1 - cov);
      uint32_t ch[3];
      for (int k = 0; k < 3; k++) ch[k] = (uint32_t)(std::clamp(c[k] + light * 255, 0.0f, 255.0f) * cov);
      out = ((uint32_t)(a * 255) << 24) | (ch[0] << 16) | (ch[1] << 8) | ch[2];
    }
  }
}

// ---------------------------------------------------------------- look
struct Look {
  bool dark;
  Color label, secondary, fill, fillHover, track, accent, onAccent, ok, off, thumb;
};
static Look makeLook(bool dark) {
  if (g_highContrast)
    return dark ? Look{true, Color(255, 255, 255, 255), Color(255, 255, 255, 255), Color(255, 0, 0, 0), Color(255, 60, 60, 60),
                       Color(255, 255, 255, 255), Color(255, 26, 235, 255), Color(255, 0, 0, 0), Color(255, 63, 242, 63),
                       Color(255, 255, 255, 255), Color(255, 255, 255, 255)}
                : Look{false, Color(255, 0, 0, 0), Color(255, 0, 0, 0), Color(255, 255, 255, 255), Color(255, 220, 220, 220),
                       Color(255, 0, 0, 0), Color(255, 0, 0, 160), Color(255, 255, 255, 255), Color(255, 0, 110, 0),
                       Color(255, 0, 0, 0), Color(255, 0, 0, 0)};
  if (dark)
    return {true, Color(245, 255, 255, 255), Color(150, 235, 235, 245), Color(28, 255, 255, 255), Color(48, 255, 255, 255),
            Color(42, 255, 255, 255), Color(255, 10, 132, 255), Color(255, 255, 255, 255), Color(255, 48, 209, 88),
            Color(255, 142, 142, 147), Color(255, 255, 255, 255)};
  return {false, Color(255, 29, 29, 31), Color(153, 60, 60, 67), Color(16, 0, 0, 0), Color(30, 0, 0, 0),
          Color(26, 0, 0, 0), Color(255, 0, 122, 255), Color(255, 255, 255, 255), Color(255, 52, 199, 89),
          Color(255, 142, 142, 147), Color(255, 255, 255, 255)};
}

// ---------------------------------------------------------------- drawing helpers
struct Rc {
  float x, y, w, h;
  bool hit(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  RectF rf() const { return RectF(x, y, w, h); }
};

static void roundRect(GraphicsPath &p, float x, float y, float w, float h, float r) {
  r = std::min(r, std::min(w, h) / 2);
  float d = 2 * r;
  p.AddArc(x, y, d, d, 180, 90);
  p.AddArc(x + w - d, y, d, d, 270, 90);
  p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  p.AddArc(x, y + h - d, d, d, 90, 90);
  p.CloseFigure();
}
static void fillRound(Graphics &g, const Color &c, float x, float y, float w, float h, float r) {
  GraphicsPath p;
  roundRect(p, x, y, w, h, r);
  SolidBrush b(c);
  g.FillPath(&b, &p);
}
static void text(Graphics &g, const std::wstring &s, float px, bool semibold, const Color &c, const RectF &r,
                 StringAlignment h = StringAlignmentNear) {
  Font f(semibold ? L"Segoe UI Semibold" : L"Segoe UI", px, FontStyleRegular, UnitPixel);
  SolidBrush b(c);
  StringFormat sf;
  sf.SetAlignment(h);
  sf.SetLineAlignment(StringAlignmentCenter);
  sf.SetTrimming(StringTrimmingEllipsisCharacter);
  sf.SetFormatFlags(StringFormatFlagsNoWrap);
  g.DrawString(s.c_str(), -1, &f, r, &sf, &b);
}
static void roundPen(Pen &p) {
  p.SetStartCap(LineCapRound);
  p.SetEndCap(LineCapRound);
}

// ---------------------------------------------------------------- panel
// Logical (DIP) layout. Corners are concentric: inner radius = outer radius - padding.
static const float PW = 260, PH = 356, P_RADIUS = 28, P_PAD = 16, P_MARGIN = 26;
static const float INNER_R = P_RADIUS - P_PAD;
static const Rc RC_CLOSE = {PW - P_PAD - 26, 14, 26, 26}, RC_SLIDER = {10, 318, PW - 20, 32};
static const float SL_X = P_PAD + 2, SL_W = PW - 2 * P_PAD - 4, SL_Y = 334;
static Rc rcMode(int i) {  // volume modes on top (2), input modes below (3); both rows span the content width
  const float cw = PW - 2 * P_PAD, gap = 6, h = 32;
  if (i < 2) return {P_PAD + i * ((cw - gap) / 2 + gap), 228, (cw - gap) / 2, h};
  return {P_PAD + (i - 2) * ((cw - 2 * gap) / 3 + gap), 266, (cw - 2 * gap) / 3, h};
}

static void drawPanel(Graphics &g, const Look &L) {
  const ModeDef &m = MODES[g_mode];
  text(g, L"SmartKnob", 16, true, L.label, RectF(P_PAD + 4, 14, 170, 24));
  SolidBrush dot(g_alive ? L.ok : L.off);
  g.FillEllipse(&dot, P_PAD + 5.0f, 44.0f, 7.0f, 7.0f);
  text(g, statusText(), 12, false, L.secondary, RectF(P_PAD + 17, 38, PW - 2 * P_PAD - 17, 18));

  // Close: a filled circle on the glass (fills, not a second glass layer).
  SolidBrush cb(g_hover == H_CLOSE ? L.fillHover : L.fill);
  g.FillEllipse(&cb, RC_CLOSE.rf());
  Pen xp(g_hover == H_CLOSE ? L.label : L.secondary, 1.7f);
  roundPen(xp);
  float cx = RC_CLOSE.x + RC_CLOSE.w / 2, cy = RC_CLOSE.y + RC_CLOSE.h / 2;
  g.DrawLine(&xp, cx - 4, cy - 4, cx + 4, cy + 4);
  g.DrawLine(&xp, cx - 4, cy + 4, cx + 4, cy - 4);

  // Dial. Tint is reserved for the value; the track is a quiet fill.
  RectF dial(PW / 2 - 70, 70, 140, 140);
  Pen track(L.track, 9), acc(L.accent, 9), marker(L.accent, 11);
  roundPen(track);
  roundPen(acc);
  roundPen(marker);
  if (m.relative) {  // endless ring, marker follows the knob's actual angle
    g.DrawEllipse(&track, dial);
    g.DrawArc(&marker, dial, 270 + fmodf(g_value * m.step, 360.0f) - 4, 8);
  } else {
    g.DrawArc(&track, dial, 135, 270);
    float frac = (g_value - m.lo) / (float)std::max(1, m.hi - m.lo);
    if (frac > 0) g.DrawArc(&acc, dial, 135, std::max(0.5f, 270 * frac));
  }
  text(g, valueText(), m.relative ? 26.0f : 36.0f, true, L.label, RectF(PW / 2 - 90, 114, 180, 44), StringAlignmentCenter);
  text(g, subtitle(), 12, false, L.secondary, RectF(PW / 2 - 92, 156, 184, 18), StringAlignmentCenter);

  for (int i = 0; i < M_COUNT; i++) {
    Rc b = rcMode(i);
    bool sel = i == g_mode;
    fillRound(g, sel ? L.accent : (g_hover == i ? L.fillHover : L.fill), b.x, b.y, b.w, b.h, INNER_R);
    text(g, MODES[i].name, 12.5f, sel, sel ? L.onAccent : L.label, b.rf(), StringAlignmentCenter);
  }

  int det = g_detent[g_mode];
  text(g, L"Click strength", 12, false, L.secondary, RectF(P_PAD + 2, 306, 150, 18));
  text(g, det ? std::to_wstring(det) + L"%" : L"Off", 12, true, L.label, RectF(PW - P_PAD - 82, 306, 80, 18),
       StringAlignmentFar);
  // Capsule slider; the thumb lifts slightly while dragged.
  float hx = SL_X + SL_W * det / 100.0f;
  fillRound(g, L.track, SL_X, SL_Y - 3, SL_W, 6, 3);
  if (det) fillRound(g, L.accent, SL_X, SL_Y - 3, hx - SL_X + 3, 6, 3);
  float tw = g_sliderDrag ? 34.0f : 28.0f, th = g_sliderDrag ? 22.0f : 18.0f;
  float tx = std::clamp(hx - tw / 2, SL_X - 4, SL_X + SL_W - tw + 4);
  for (int i = 3; i >= 1; i--) fillRound(g, Color((BYTE)(14 / i), 0, 0, 0), tx - i * 0.5f, SL_Y - th / 2 + 1, tw + i, th + i * 0.5f, th);
  fillRound(g, L.thumb, tx, SL_Y - th / 2, tw, th, th / 2);
}

// ---------------------------------------------------------------- volume popup (a glass capsule)
static const float OW = 300, OH = 60, O_MARGIN = 24;

static void drawSpeaker(Graphics &g, float x, float cy, int level, const Color &c, const Color &dimC) {
  SolidBrush b(c);
  PointF body[] = {{x, cy - 3.5f}, {x + 4, cy - 3.5f}, {x + 9.5f, cy - 8.5f}, {x + 9.5f, cy + 8.5f}, {x + 4, cy + 3.5f}, {x, cy + 3.5f}};
  g.FillPolygon(&b, body, 6);
  int waves = level <= 0 ? 0 : level < 34 ? 1 : level < 67 ? 2 : 3;
  for (int i = 0; i < 3; i++) {
    Pen p(i < waves ? c : dimC, 1.8f);
    roundPen(p);
    float r = 4.5f + i * 4.0f;
    g.DrawArc(&p, RectF(x + 9.5f - r, cy - r, 2 * r, 2 * r), -45, 90);
  }
  if (level <= 0) {
    Pen p(c, 1.8f);
    roundPen(p);
    g.DrawLine(&p, x - 1, cy - 9, x + 21, cy + 9);
  }
}

static void drawOsd(Graphics &g, const Look &L) {
  const ModeDef &m = MODES[g_mode];
  drawSpeaker(g, 22, OH / 2, g_mode == M_APP && g_appTarget.empty() ? 50 : g_value, L.label, L.track);
  std::wstring title = g_mode == M_APP && !g_appTarget.empty() ? appName() : std::wstring(m.name);
  text(g, title, 12, false, L.secondary, RectF(62, 11, 160, 16));
  text(g, valueText(), 13, true, L.label, RectF(OW - 30 - 60, 11, 60, 16), StringAlignmentFar);
  // Monochrome level bar, like the system volume HUD.
  float bx = 62, bw = OW - 30 - 62, by = 35;
  fillRound(g, L.track, bx, by, bw, 6, 3);
  float frac = (g_value - m.lo) / (float)std::max(1, m.hi - m.lo);
  if (frac > 0) fillRound(g, L.label, bx, by, std::max(6.0f, bw * frac), 6, 3);
}

// ---------------------------------------------------------------- glass windows
struct Anim {
  float from = 0, to = 0;
  DWORD t0 = 0, dur = 0;
  bool active = false;
};
struct GlassWin {
  HWND hwnd = nullptr;
  float lw, lh, radius, margin;
  bool flips;  // small elements flip light/dark with what's behind them; large ones follow the system
  bool excluded = false;  // hidden from screen capture, so it can capture what's behind itself
  Backdrop bd;
  bool dark = true;
  float appear = 0;  // 0..1: materialize / dissolve
  Anim anim;
  std::vector<uint32_t> last;  // last frame, for snapshots
  int lastW = 0, lastH = 0;
};
static GlassWin g_pg = {nullptr, PW, PH, P_RADIUS, P_MARGIN, false};
static GlassWin g_og = {nullptr, OW, OH, OH / 2, O_MARGIN, true};
static bool g_snapshot = false;

// Renders a glass window at pos (screen px). recapture: grab a fresh backdrop; onlyIfChanged: skip
// the render when the backdrop is unchanged (periodic refresh).
static void renderGlass(GlassWin &gw, float s, POINT pos, bool recapture, bool onlyIfChanged,
                        void (*draw)(Graphics &, const Look &)) {
  int W = (int)ceilf((gw.lw + 2 * gw.margin) * s), H = (int)ceilf((gw.lh + 2 * gw.margin) * s);
  RECT r = {pos.x, pos.y, pos.x + W, pos.y + H};
  bool useBd = (gw.excluded || g_snapshot) && g_transparency && !g_highContrast;
  if (useBd && (recapture || !gw.bd.valid || gw.bd.w != W || gw.bd.h != H)) {
    if (!captureBackdrop(gw.bd, r, s) && onlyIfChanged) return;
  } else if (onlyIfChanged) {
    return;
  }
  useBd = useBd && gw.bd.valid && gw.bd.w == W && gw.bd.h == H;
  if (gw.flips && useBd)  // with hysteresis, so it doesn't flicker over mid-grey content
    gw.dark = gw.dark ? gw.bd.lum < 0.60f : gw.bd.lum < 0.42f;
  else
    gw.dark = g_sysDark;
  Look L = makeLook(gw.dark);

  HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
  BITMAPINFO bi = {};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
  void *bits = nullptr;
  HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(mem, dib);
  Shape sh = {gw.margin * s, gw.margin * s, gw.lw * s, gw.lh * s, gw.radius * s};
  composeGlass((uint32_t *)bits, W, H, useBd ? &gw.bd : nullptr, sh, s, gw.dark, 0.35f + 0.65f * gw.appear);
  {
    Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE *)bits);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.ScaleTransform(s, s);
    g.TranslateTransform(gw.margin, gw.margin);
    if (g_highContrast) {
      GraphicsPath p;
      roundRect(p, 0.5f, 0.5f, gw.lw - 1, gw.lh - 1, gw.radius);
      Pen border(L.label, 2);
      g.DrawPath(&border, &p);
    }
    draw(g, L);
  }
  if (g_snapshot) {
    gw.last.assign((uint32_t *)bits, (uint32_t *)bits + (size_t)W * H);
    gw.lastW = W;
    gw.lastH = H;
  }
  SIZE sz = {W, H};
  POINT src = {0, 0};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, (BYTE)lroundf(255 * gw.appear), AC_SRC_ALPHA};
  UpdateLayeredWindow(gw.hwnd, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

static POINT windowPos(HWND h) {
  RECT r;
  GetWindowRect(h, &r);
  return {r.left, r.top};
}
static void renderPanel(bool recapture, bool onlyIfChanged = false) {
  g_panelScale = GetDpiForWindow(g_panel) / 96.0f;
  renderGlass(g_pg, g_panelScale, windowPos(g_panel), recapture, onlyIfChanged, drawPanel);
}
static float osdScale() { return GetDpiForSystem() / 96.0f; }
static void renderOsd(bool recapture, bool onlyIfChanged = false) {
  renderGlass(g_og, osdScale(), g_osdPos, recapture, onlyIfChanged, drawOsd);
}

// Materialize / dissolve: opacity and lensing ramp together.
static void startAnim(GlassWin &gw, float to, DWORD dur) {
  if (!g_animations || g_snapshot) {
    gw.appear = to;
    gw.anim.active = false;
    return;
  }
  gw.anim = {gw.appear, to, GetTickCount(), dur, true};
  SetTimer(g_main, T_ANIM, 15, nullptr);
}
static void stepAnim(GlassWin &gw, void (*render)(bool, bool)) {
  if (!gw.anim.active) return;
  float t = std::min(1.0f, (GetTickCount() - gw.anim.t0) / (float)std::max<DWORD>(1, gw.anim.dur));
  float e = 1 - (1 - t) * (1 - t) * (1 - t);
  gw.appear = gw.anim.from + (gw.anim.to - gw.anim.from) * e;
  if (t >= 1) gw.anim.active = false;
  render(false, false);
  if (!gw.anim.active && gw.appear <= 0) ShowWindow(gw.hwnd, SW_HIDE);
}

static void ensureGlassTimer() { SetTimer(g_main, T_GLASS, 250, nullptr); }

static void showOsd() {
  float s = osdScale();
  RECT wa;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  g_osdPos = {(wa.left + wa.right) / 2 - (int)((OW / 2 + O_MARGIN) * s), wa.bottom - (int)((OH + O_MARGIN + 72) * s)};
  bool visible = IsWindowVisible(g_osd) && g_og.appear > 0;
  if (!visible) {
    g_og.appear = 0;
    startAnim(g_og, 1, 170);
    renderOsd(true);
    ShowWindow(g_osd, SW_SHOWNOACTIVATE);
    ensureGlassTimer();
  } else {
    if (g_og.anim.active && g_og.anim.to == 0) startAnim(g_og, 1, 120);
    renderOsd(false);
  }
  SetTimer(g_main, T_OSD_HIDE, 1300, nullptr);
}

static void hideOsdNow() {
  KillTimer(g_main, T_OSD_HIDE);
  g_og.anim.active = false;
  g_og.appear = 0;
  ShowWindow(g_osd, SW_HIDE);
}

static HICON makeIcon(bool on) {
  int sz = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
  Bitmap bmp(sz, sz, PixelFormat32bppARGB);
  {
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.ScaleTransform(sz / 64.0f, sz / 64.0f);
    // A small glass puck: tinted body, top highlight, pointer.
    Color body = on ? Color(255, 10, 132, 255) : Color(255, 120, 120, 128);
    LinearGradientBrush fill(PointF(0, 4), PointF(0, 60), Color(255, std::min(255, body.GetR() + 70),
                             std::min(255, body.GetG() + 60), std::min(255, body.GetB() + 30)), body);
    g.FillEllipse(&fill, 4, 4, 56, 56);
    Pen rim(Color(150, 255, 255, 255), 3);
    g.DrawArc(&rim, RectF(7, 7, 50, 50), 200, 110);
    Pen ptr(Color(255, 255, 255, 255), 8);
    roundPen(ptr);
    g.DrawLine(&ptr, 32, 15, 32, 27);
  }
  HICON h = nullptr;
  bmp.GetHICON(&h);
  return h;
}

static void updateTray() {
  std::wstring tip = std::wstring(L"SmartKnob · ") + MODES[g_mode].name + L"\n" + statusText();
  if (g_alive) tip += L" (" + g_portName + L")";
  HICON icon = g_alive ? g_iconOn : g_iconOff;
  if (tip == g_lastTip && g_nid.hIcon == icon) return;
  g_lastTip = tip;
  g_nid.hIcon = icon;
  g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void addTray() {
  g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
  Shell_NotifyIconW(NIM_ADD, &g_nid);
  g_nid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void refreshUi() {
  if (IsWindowVisible(g_panel)) renderPanel(false);
  if (IsWindowVisible(g_osd)) renderOsd(false);
  updateTray();
}

static void showPanel(bool show) {
  iniSet(L"showPanel", show ? 1 : 0);
  if (!show) {
    startAnim(g_pg, 0, 160);
    if (!g_pg.anim.active) ShowWindow(g_panel, SW_HIDE);
    return;
  }
  if (IsWindowVisible(g_panel) && g_pg.appear > 0) {
    if (g_pg.anim.active) startAnim(g_pg, 1, 160);
    return;
  }
  int x = iniGetInt(L"panelX", INT_MIN), y = iniGetInt(L"panelY", INT_MIN);
  float s = GetDpiForSystem() / 96.0f;
  if (x == INT_MIN || y == INT_MIN || !MonitorFromPoint({x + (int)(P_MARGIN * s) + 20, y + (int)(P_MARGIN * s) + 20}, MONITOR_DEFAULTTONULL)) {
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    x = wa.right - (int)((PW + P_MARGIN + 12) * s);
    y = wa.bottom - (int)((PH + P_MARGIN + 12) * s);
  }
  SetWindowPos(g_panel, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
  g_pg.appear = 0;
  startAnim(g_pg, 1, 220);
  renderPanel(true);
  ShowWindow(g_panel, SW_SHOWNOACTIVATE);
  if (IsWindowVisible(g_osd)) hideOsdNow();
  ensureGlassTimer();
}

// ---------------------------------------------------------------- window procs
static int panelHit(float x, float y) {  // logical coordinates inside the glass
  if (RC_CLOSE.hit(x, y)) return H_CLOSE;
  if (RC_SLIDER.hit(x, y)) return H_SLIDER;
  for (int i = 0; i < M_COUNT; i++)
    if (rcMode(i).hit(x, y)) return i;
  return -1;
}
static void toLogical(LPARAM lp, bool screen, float &x, float &y) {
  POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  if (screen) ScreenToClient(g_panel, &p);
  x = p.x / g_panelScale - P_MARGIN;
  y = p.y / g_panelScale - P_MARGIN;
}

static void sliderTo(LPARAM lp) {
  float x, y;
  toLogical(lp, false, x, y);
  g_detent[g_mode] = std::clamp((int)lroundf((x - SL_X) / SL_W * 100), 0, 100);
  renderPanel(false);
}

static LRESULT CALLBACK panelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCHITTEST: {
      float x, y;
      toLogical(lp, true, x, y);
      if (x < 0 || y < 0 || x > PW || y > PH) return HTNOWHERE;  // the shadow
      return panelHit(x, y) >= 0 ? HTCLIENT : HTCAPTION;         // drag from anywhere else
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // keep focus on the user's app (matters for App volume)
    case WM_NCLBUTTONDBLCLK: return 0;
    case WM_MOVE:
      if (IsWindowVisible(h)) renderPanel(true);  // the glass shows what's behind it while dragging
      return 0;
    case WM_EXITSIZEMOVE: {
      RECT r;
      GetWindowRect(h, &r);
      iniSet(L"panelX", r.left);
      iniSet(L"panelY", r.top);
      return 0;
    }
    case WM_DPICHANGED: {
      const RECT *r = (const RECT *)lp;
      SetWindowPos(h, nullptr, r->left, r->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      renderPanel(true);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (g_sliderDrag) {
        sliderTo(lp);
        return 0;
      }
      if (!g_tracking) {
        TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        g_tracking = true;
      }
      float x, y;
      toLogical(lp, false, x, y);
      int hv = panelHit(x, y);
      if (hv != g_hover) {
        g_hover = hv;
        renderPanel(false);
      }
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_MOUSELEAVE:
      g_tracking = false;
      if (g_hover != -1) {
        g_hover = -1;
        renderPanel(false);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      float x, y;
      toLogical(lp, false, x, y);
      int hv = panelHit(x, y);
      if (hv == H_CLOSE) {
        showPanel(false);
      } else if (hv == H_SLIDER) {
        g_sliderDrag = true;
        SetCapture(h);
        sliderTo(lp);
      } else if (hv >= 0) {
        setMode(hv);
      }
      return 0;
    }
    case WM_LBUTTONUP:
      if (g_sliderDrag) {
        g_sliderDrag = false;
        ReleaseCapture();
        setDetent(g_detent[g_mode]);
      }
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static void showMenu() {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, ID_OPEN, IsWindowVisible(g_panel) ? L"Hide SmartKnob" : L"Show SmartKnob");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  for (int i = 0; i < M_COUNT; i++) AppendMenuW(m, MF_STRING, ID_MODE0 + i, MODES[i].name);
  CheckMenuRadioItem(m, ID_MODE0, ID_MODE0 + M_COUNT - 1, ID_MODE0 + g_mode, MF_BYCOMMAND);
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_RELEASE, g_paused ? L"Reconnect knob" : L"Disconnect for firmware update");
  AppendMenuW(m, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit SmartKnob");
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g_main);
  TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
  PostMessageW(g_main, WM_NULL, 0, 0);
  DestroyMenu(m);
}

static LRESULT CALLBACK mainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == g_taskbarCreated && msg) {  // Explorer restarted
    addTray();
    return 0;
  }
  switch (msg) {
    case WM_APP_LINE: {
      std::string *s = (std::string *)lp;
      if ((UINT)wp == g_gen) onLine(*s);
      delete s;
      return 0;
    }
    case WM_APP_LINKERR:
      if ((UINT)wp == g_gen) linkClose();
      return 0;
    case WM_APP_BLE:
      onBleConnected((BleLink *)lp);
      return 0;
    case WM_APP_VOLUME:
      if (g_mode == M_VOLUME) syncExternal((int)wp);
      return 0;
    case WM_APP_DEVICE:
      acquireEndpoint();
      if (!MODES[g_mode].relative) syncExternal(initialValue());
      return 0;
    case WM_APP_SHOWPANEL: showPanel(true); return 0;
    case WM_DEVICECHANGE: g_scanNow = true; return TRUE;
    case WM_SETTINGCHANGE:  // light/dark, transparency, animation or contrast settings
      readSystemAppearance();
      refreshUi();
      return 0;
    case WM_TIMER:
      switch (wp) {
        case T_LINK: linkTick(); break;
        case T_APP: {  // App volume: pick up new audio sessions and changes made in the mixer
          bool changed = followFocus();
          if (!g_appTarget.empty()) {
            auto vols = sessionsOf(g_appTarget);
            if (!vols.empty()) {
              releaseAppVols();
              g_appVols = vols;
            }
          }
          if (changed) {
            g_lastTurn = 0;
            refreshUi();
          }
          syncExternal(appGet());
          break;
        }
        case T_RESYNC:
          KillTimer(h, T_RESYNC);
          syncExternal(g_mode == M_VOLUME ? masterGet() : g_mode == M_APP ? appGet() : -1);
          break;
        case T_OSD_HIDE:
          KillTimer(h, T_OSD_HIDE);
          startAnim(g_og, 0, 280);
          if (!g_og.anim.active) ShowWindow(g_osd, SW_HIDE);
          break;
        case T_ANIM:
          stepAnim(g_pg, [](bool r, bool o) { renderPanel(r, o); });
          stepAnim(g_og, [](bool r, bool o) { renderOsd(r, o); });
          if (!g_pg.anim.active && !g_og.anim.active) KillTimer(h, T_ANIM);
          break;
        case T_GLASS: {  // keep the glass in step with what's behind it, only while visible
          bool pv = IsWindowVisible(g_panel), ov = IsWindowVisible(g_osd);
          if (pv && !g_pg.anim.active && !g_sliderDrag) renderPanel(true, true);
          if (ov && !g_og.anim.active) renderOsd(true, true);
          if (!pv && !ov) KillTimer(h, T_GLASS);
          break;
        }
      }
      return 0;
    case WM_APP_TRAY:
      switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT: showPanel(!IsWindowVisible(g_panel) || g_pg.anim.to == 0); break;
        case WM_CONTEXTMENU: showMenu(); break;
      }
      return 0;
    case WM_COMMAND: {
      int id = LOWORD(wp);
      if (id == ID_OPEN) showPanel(!IsWindowVisible(g_panel) || g_pg.anim.to == 0);
      if (id >= ID_MODE0 && id < ID_MODE0 + M_COUNT) setMode(id - ID_MODE0);
      if (id == ID_RELEASE) {
        g_paused = !g_paused;
        if (g_paused) linkClose();
        g_scanNow = true;
        refreshUi();
      }
      if (id == ID_AUTOSTART) setAutostart(!autostartEnabled());
      if (id == ID_QUIT) DestroyWindow(h);
      return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static void CALLBACK foregroundHook(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
  updateForeground(hwnd);
  if (g_mode == M_APP && followFocus()) {
    g_value = initialValue();
    g_lastTurn = 0;
    linkSendf("VAL v=%d\n", g_value);
    refreshUi();
  }
}

// ---------------------------------------------------------------- snapshots (design check)
static bool pngEncoder(CLSID &clsid) {
  UINT n = 0, size = 0;
  GetImageEncodersSize(&n, &size);
  std::vector<BYTE> buf(size);
  auto *enc = (ImageCodecInfo *)buf.data();
  GetImageEncoders(n, size, enc);
  for (UINT i = 0; i < n; i++)
    if (wcscmp(enc[i].MimeType, L"image/png") == 0) {
      clsid = enc[i].Clsid;
      return true;
    }
  return false;
}

// Saves the last frame composited over the captured screen, i.e. what the user would see.
static void saveSnapshot(const GlassWin &gw, const std::wstring &path) {
  int W = gw.lastW, H = gw.lastH;
  Bitmap out(W, H, PixelFormat32bppARGB);
  BitmapData d;
  Gdiplus::Rect rc(0, 0, W, H);
  out.LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &d);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      size_t i = (size_t)y * W + x;
      uint32_t f = gw.last[i], b = gw.bd.valid && gw.bd.w == W ? gw.bd.raw[i] : 0xFF7F7F7F;
      uint32_t a = f >> 24, o = 0xFF000000;
      for (int sh = 0; sh < 24; sh += 8)
        o |= std::min<uint32_t>(255, ((f >> sh) & 255) + ((b >> sh) & 255) * (255 - a) / 255) << sh;
      ((uint32_t *)((BYTE *)d.Scan0 + (size_t)y * d.Stride))[x] = o;
    }
  out.UnlockBits(&d);
  CLSID png;
  if (pngEncoder(png)) out.Save(path.c_str(), &png, nullptr);
}

static void runSnapshots(const std::wstring &dir) {
  g_snapshot = true;
  bool sys = g_sysDark;
  for (int dark = 1; dark >= 0; dark--) {
    g_sysDark = dark != 0;
    const wchar_t *tag = dark ? L"dark" : L"light";
    for (int m : {M_VOLUME, M_SCROLL}) {
      g_mode = m;
      g_value = m == M_VOLUME ? std::max(0, masterGet()) : 7;
      g_detent[M_SCROLL] = 40;
      g_hover = m == M_VOLUME ? M_ZOOM : -1;
      SetWindowPos(g_panel, HWND_TOPMOST, 200, 120, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
      g_pg.appear = 1;
      renderPanel(true);
      saveSnapshot(g_pg, dir + L"\\panel_" + MODES[m].key + L"_" + tag + L".png");
    }
    g_mode = M_VOLUME;
    g_value = std::max(0, masterGet());
    showOsd();
    saveSnapshot(g_og, dir + L"\\osd_" + tag + L".png");
  }
  g_sysDark = sys;
}

// ---------------------------------------------------------------- main
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
  g_inst = inst;
  std::wstring snapshotDir;
  if (wcsncmp(cmdLine, L"--snapshot ", 11) == 0) {
    snapshotDir = cmdLine + 11;
    while (!snapshotDir.empty() && (snapshotDir.back() == L' ' || snapshotDir.back() == L'"')) snapshotDir.pop_back();
    if (!snapshotDir.empty() && snapshotDir[0] == L'"') snapshotDir.erase(0, 1);
  }
  HANDLE mutex = nullptr;
  if (snapshotDir.empty()) {
    mutex = CreateMutexW(nullptr, TRUE, L"SmartKnobDesktop");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {  // already running: just show its panel
      if (HWND other = FindWindowW(L"SmartKnobMain", nullptr)) PostMessageW(other, WM_APP_SHOWPANEL, 0, 0);
      return 0;
    }
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ULONG_PTR gdipToken;
  GdiplusStartupInput gsi;
  GdiplusStartup(&gdipToken, &gsi, nullptr);
  readSystemAppearance();

  wchar_t appdata[MAX_PATH];
  GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  std::wstring dir = std::wstring(appdata) + L"\\SmartKnob";
  CreateDirectoryW(dir.c_str(), nullptr);
  g_ini = dir + L"\\settings.ini";
  std::wstring modeKey = iniGet(L"mode", L"volume");
  for (int i = 0; i < M_COUNT; i++) {
    g_detent[i] = std::clamp(iniGetInt(detentKey(i).c_str(), MODES[i].defDetent), 0, 100);
    if (modeKey == MODES[i].key) g_mode = i;
  }

  WNDCLASSW wc = {};
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpfnWndProc = mainProc;
  wc.lpszClassName = L"SmartKnobMain";
  RegisterClassW(&wc);
  wc.lpfnWndProc = panelProc;
  wc.lpszClassName = L"SmartKnobPanel";
  RegisterClassW(&wc);
  wc.lpfnWndProc = DefWindowProcW;
  wc.lpszClassName = L"SmartKnobOsd";
  RegisterClassW(&wc);

  g_main = CreateWindowExW(0, L"SmartKnobMain", L"SmartKnob", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
  g_panel = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"SmartKnobPanel",
                            L"SmartKnob", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
  g_osd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                          L"SmartKnobOsd", L"SmartKnob", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
  g_pg.hwnd = g_panel;
  g_og.hwnd = g_osd;
  // Hidden from screen capture so each window can capture what's behind it (Windows 10 2004+).
  // Without it the glass falls back to the opaque material.
  g_pg.excluded = SetWindowDisplayAffinity(g_panel, WDA_EXCLUDEFROMCAPTURE) != FALSE;
  g_og.excluded = SetWindowDisplayAffinity(g_osd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
  g_writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                 (void **)&g_devEnum)))
    g_devEnum->RegisterEndpointNotificationCallback(&g_devCb);
  acquireEndpoint();

  if (!snapshotDir.empty()) {
    runSnapshots(snapshotDir);
  } else {
    updateForeground(GetForegroundWindow());
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundHook, 0, 0,
                                         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_iconOn = makeIcon(true);
    g_iconOff = makeIcon(false);
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_iconOff;
    wcscpy_s(g_nid.szTip, L"SmartKnob");
    addTray();

    setMode(g_mode);
    SetTimer(g_main, T_LINK, 500, nullptr);
    linkTick();
    if (iniGetInt(L"showPanel", 1)) showPanel(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    linkClose();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    UnhookWinEvent(hook);
  }

  releaseAppVols();
  if (g_epVol) g_epVol->UnregisterControlChangeNotify(&g_volCb);
  release(g_epVol);
  release(g_sessMgr);
  if (g_devEnum) g_devEnum->UnregisterEndpointNotificationCallback(&g_devCb);
  release(g_devEnum);
  GdiplusShutdown(gdipToken);
  CoUninitialize();
  if (mutex) ReleaseMutex(mutex);
  return 0;
}
