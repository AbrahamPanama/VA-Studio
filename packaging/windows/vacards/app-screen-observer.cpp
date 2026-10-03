// SPDX-License-Identifier: GPL-2.0-or-later
// app-screen-observer.cpp — bounded native DXGI Desktop Duplication observer.
//
// One small console tool, built with the existing MSYS2 UCRT64 g++ against the
// real Windows DXGI/D3D11 headers and system d3d11.dll/dxgi.dll (no C#/COM-vtable
// interop, no new dependencies).
//
// Measurement notes:
//   * LastPresentTime is a compositor-observed software presentation timestamp in
//     QPC units (NOT photons, NOT physical display FPS/refresh, NOT GDI completion).
//   * Content cadence is decided by actual ROI pixel changes, never by metadata.
//   * Pointer-only frames (LastPresentTime == 0 && AccumulatedFrames == 0) never
//     count as a content update.
//
// Exit codes:
//   0  normal completion (stop file or a bounded limit)
//   2  argument/parse error
//   3  environment/validation failure (fail closed)
//   4  runtime fatal (e.g. duplication access lost)
//   5  self-test failure
//
// Build (on dev-en, UCRT64):
//   g++ -O2 -std=c++17 -Wall -Wextra app-screen-observer.cpp -o app-screen-observer.exe
//       -ld3d11 -ldxgi -ldxguid -lshcore -luser32 -lkernel32 -ladvapi32 -lole32 -lgdi32

#define __USE_MINGW_ANSI_STDIO 1
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#define OBS_VERSION "next4-observer-1"

// ---------------------------------------------------------------------------
// Minimal RAII COM holder (avoids relying on WRL availability).
// ---------------------------------------------------------------------------
template <class T>
class ComPtr {
 public:
  ComPtr() : p_(nullptr) {}
  ~ComPtr() { reset(); }
  T** put() {
    reset();
    return &p_;
  }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  void reset() {
    if (p_) {
      p_->Release();
      p_ = nullptr;
    }
  }
  void attach(T* p) {
    reset();
    p_ = p;
  }
  T* detach() {
    T* t = p_;
    p_ = nullptr;
    return t;
  }

 private:
  ComPtr(const ComPtr&);
  ComPtr& operator=(const ComPtr&);
  T* p_;
};

// ---------------------------------------------------------------------------
// Small pure helpers (also used by --self-test).
// ---------------------------------------------------------------------------
static long long qpc_now() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

static std::string hr_hex(HRESULT hr) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return std::string(buf);
}

static std::string last_error_hex() {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "err=0x%08lX",
                static_cast<unsigned long>(GetLastError()));
  return std::string(buf);
}

static std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                              nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                      &out[0], n, nullptr, nullptr);
  return out;
}

static std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                              nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0],
                      n);
  return out;
}

static std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o;
}

static std::wstring join_path(const std::wstring& dir, const wchar_t* name) {
  if (dir.empty()) return std::wstring(name);
  wchar_t last = dir[dir.size() - 1];
  if (last == L'\\' || last == L'/') return dir + name;
  return dir + L"\\" + name;
}

// ---------------------------------------------------------------------------
// ROI strip scan (pure).
// ---------------------------------------------------------------------------
enum class TargetClass { Missing, Ambiguous, Valid };

struct ScanResult {
  TargetClass cls = TargetClass::Missing;
  int runs_total = 0;
  int max_runs_in_row = 0;
  int rows_with_run = 0;
  int rows_scanned = 0;
  int left = -1;
  int right = -1;
  int width = -1;
  bool width_drift = false;
  uint64_t signature = 0;
  std::string reason;
};

struct PixelFormat {
  bool bgra = true;  // true: bytes are B,G,R,A; false: R,G,B,A
  const char* name = "B8G8R8A8_UNORM";
};

static inline void rgb_of(const uint8_t* px, bool bgra, int* r, int* g, int* b) {
  if (bgra) {
    *b = px[0];
    *g = px[1];
    *r = px[2];
  } else {
    *r = px[0];
    *g = px[1];
    *b = px[2];
  }
}

static inline bool pixel_match(const uint8_t* px, bool bgra, int er, int eg,
                               int eb, int tol) {
  int r, g, b;
  rgb_of(px, bgra, &r, &g, &b);
  return std::abs(r - er) <= tol && std::abs(g - eg) <= tol &&
         std::abs(b - eb) <= tol;
}

// Hash only R,G,B in canonical order so BGRA and RGBA of the same image produce
// the same signature (alpha is ignored).
static ScanResult scan_strip(const uint8_t* data, int w, int h, int stride,
                             const PixelFormat& fmt, int er, int eg, int eb,
                             int tol, int expected_width, int width_tol,
                             int edge_tol) {
  ScanResult r;
  r.rows_scanned = h;
  if (!data || w <= 0 || h <= 0 || stride < w * 4) {
    r.cls = TargetClass::Ambiguous;
    r.reason = "invalid_strip";
    return r;
  }

  uint64_t sig = 1469598103934665603ULL;  // FNV-1a 64 offset basis
  int min_first = INT32_MAX, max_first = INT32_MIN;
  int min_last = INT32_MAX, max_last = INT32_MIN;

  for (int y = 0; y < h; ++y) {
    const uint8_t* row = data + static_cast<size_t>(y) * stride;
    // Signature over the whole ROI row (RGB only).
    for (int x = 0; x < w; ++x) {
      const uint8_t* px = row + static_cast<size_t>(x) * 4;
      int cr, cg, cb;
      rgb_of(px, fmt.bgra, &cr, &cg, &cb);
      sig ^= static_cast<uint64_t>(cr);
      sig *= 1099511628211ULL;
      sig ^= static_cast<uint64_t>(cg);
      sig *= 1099511628211ULL;
      sig ^= static_cast<uint64_t>(cb);
      sig *= 1099511628211ULL;
    }

    int runs = 0;
    int first = -1, last = -1;
    int run_start = -1;
    for (int x = 0; x < w; ++x) {
      const uint8_t* px = row + static_cast<size_t>(x) * 4;
      bool m = pixel_match(px, fmt.bgra, er, eg, eb, tol);
      if (m) {
        if (run_start < 0) run_start = x;
      } else if (run_start >= 0) {
        ++runs;
        if (first < 0) first = run_start;
        last = x - 1;
        run_start = -1;
      }
    }
    if (run_start >= 0) {
      ++runs;
      if (first < 0) first = run_start;
      last = w - 1;
    }
    r.runs_total += runs;
    if (runs > r.max_runs_in_row) r.max_runs_in_row = runs;
    if (runs > 0) ++r.rows_with_run;
    if (runs == 1) {
      min_first = std::min(min_first, first);
      max_first = std::max(max_first, first);
      min_last = std::min(min_last, last);
      max_last = std::max(max_last, last);
    }
  }
  r.signature = sig;

  if (r.max_runs_in_row > 1) {
    r.cls = TargetClass::Ambiguous;
    r.reason = "multiple_runs";
    return r;
  }
  if (r.rows_with_run == 0) {
    r.cls = TargetClass::Missing;
    r.reason = "no_target_pixels";
    return r;
  }
  if (r.rows_with_run != r.rows_scanned) {
    // A clipped/partial rectangle (some strip rows lack the target) must never
    // be admitted as a valid edge measurement.
    r.cls = TargetClass::Ambiguous;
    r.reason = "partial_rows";
    return r;
  }
  if (max_first - min_first > edge_tol || max_last - min_last > edge_tol) {
    r.cls = TargetClass::Ambiguous;
    r.reason = "inconsistent_edges";
    return r;
  }
  r.left = min_first;
  r.right = max_last;
  r.width = r.right - r.left + 1;
  // Expected width is mandatory for a real run (enforced by parse_args). A run
  // outside tolerance is a drift and is NOT admitted.
  if (expected_width > 0 && std::abs(r.width - expected_width) > width_tol) {
    r.width_drift = true;
    r.cls = TargetClass::Ambiguous;
    r.reason = "width_drift";
    return r;
  }
  r.cls = TargetClass::Valid;
  r.reason = "ok";
  return r;
}

// Tracks target edges across valid frames. Motion is ONLY actual edge
// displacement from the previous valid frame; a whole-ROI signature change is
// reported separately as a diagnostic and never counted as motion.
struct MotionTracker {
  bool have_prev = false;
  int prev_left = -1;
  int prev_right = -1;
  unsigned long long prev_sig = 0;

  void seed(int left, int right, unsigned long long sig) {
    have_prev = true;
    prev_left = left;
    prev_right = right;
    prev_sig = sig;
  }

  // Returns true on edge displacement. Invalid frames do not update state.
  bool observe(const ScanResult& sr, bool* signature_changed) {
    *signature_changed = false;
    if (sr.cls != TargetClass::Valid) return false;
    bool motion = false;
    if (have_prev) {
      motion = (sr.left != prev_left || sr.right != prev_right);
      *signature_changed = (sr.signature != prev_sig);
    }
    prev_left = sr.left;
    prev_right = sr.right;
    prev_sig = sr.signature;
    have_prev = true;
    return motion;
  }
};

struct FrameClass {
  bool pointer_only = false;
  bool content_eligible = false;
};

static FrameClass classify_frame(long long last_present,
                                 unsigned accumulated_frames) {
  FrameClass fc;
  fc.pointer_only = (last_present == 0 && accumulated_frames == 0);
  fc.content_eligible = !fc.pointer_only;
  return fc;
}

// A ROI must be non-empty, fully inside the provided monitor rectangle and
// within the narrow-strip budget (so a full-frame readback can never happen).
static bool validate_roi(int rx, int ry, int rw, int rh, int bx, int by, int bw,
                         int bh, bool enforce_narrow, std::string& err) {
  if (rw <= 0 || rh <= 0) {
    err = "roi width/height must be positive";
    return false;
  }
  if (enforce_narrow && (rw > 1024 || rh > 64 || rw * rh > 65536)) {
    err = "roi exceeds narrow-strip budget (<=1024x64, <=65536 px)";
    return false;
  }
  if (rx < bx || ry < by || rx + rw > bx + bw || ry + rh > by + bh) {
    err = "roi lies outside the expected monitor bounds";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// CLI parsing.
// ---------------------------------------------------------------------------
struct Args {
  bool self_test = false;
  bool help = false;

  unsigned long long pid = 0;
  bool have_pid = false;
  unsigned long long hwnd = 0;
  bool have_hwnd = false;

  std::wstring monitor;  // e.g. \\.\DISPLAY2

  bool have_bounds = false;
  int bleft = 0, btop = 0, bright = 0, bbottom = 0;

  bool have_dpi = false;
  unsigned dpi = 0;

  bool have_roi = false;
  int rx = 0, ry = 0, rw = 0, rh = 0;

  unsigned rgb = 0x204080;
  int tolerance = 8;
  int expected_width = -1;
  int width_tolerance = 2;

  long long max_duration_ms = 40000;
  long long max_frames = 200000;
  long long max_output_bytes = 64LL * 1024 * 1024;

  std::wstring out_dir;
  std::wstring ready_file;
  std::wstring stop_file;

  unsigned acquire_timeout_ms = 20;
  long long warmup_ms = 5000;
  long long stale_ms = 2000;
};

static bool parse_u64(const char* s, unsigned long long& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  out = std::strtoull(s, &end, 0);
  return end && end != s && *end == '\0';
}

static bool parse_int(const char* s, int& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (!end || end == s || *end != '\0') return false;
  if (v < INT32_MIN || v > INT32_MAX) return false;
  out = static_cast<int>(v);
  return true;
}

static bool parse_bounds(const char* s, int& l, int& t, int& r, int& b) {
  return std::sscanf(s, "%d,%d,%d,%d", &l, &t, &r, &b) == 4;
}

static bool parse_roi(const char* s, int& x, int& y, int& w, int& h) {
  return std::sscanf(s, "%d,%d,%d,%d", &x, &y, &w, &h) == 4;
}

static bool parse_args(int argc, const char* const* argv, Args& a,
                       std::string& err) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i] ? argv[i] : "";
    if (arg == "--self-test") {
      a.self_test = true;
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      a.help = true;
      continue;
    }
    if (arg.rfind("--", 0) != 0) {
      err = "unexpected positional argument: " + arg;
      return false;
    }
    size_t eq = arg.find('=');
    if (eq == std::string::npos) {
      err = "expected --key=value, got: " + arg;
      return false;
    }
    std::string key = arg.substr(2, eq - 2);
    std::string val = arg.substr(eq + 1);
    const char* v = val.c_str();
    if (key == "pid") {
      if (!parse_u64(v, a.pid)) { err = "bad --pid"; return false; }
      a.have_pid = true;
    } else if (key == "hwnd") {
      if (!parse_u64(v, a.hwnd)) { err = "bad --hwnd"; return false; }
      a.have_hwnd = true;
    } else if (key == "monitor") {
      a.monitor = utf8_to_wide(val);
      if (a.monitor.empty()) { err = "bad --monitor"; return false; }
    } else if (key == "bounds") {
      if (!parse_bounds(v, a.bleft, a.btop, a.bright, a.bbottom)) {
        err = "bad --bounds (want l,t,r,b)";
        return false;
      }
      a.have_bounds = true;
    } else if (key == "dpi") {
      int dpi = 0;
      if (!parse_int(v, dpi) || dpi <= 0) {
        err = "bad --dpi";
        return false;
      }
      a.dpi = static_cast<unsigned>(dpi);
      a.have_dpi = true;
    } else if (key == "roi") {
      if (!parse_roi(v, a.rx, a.ry, a.rw, a.rh)) {
        err = "bad --roi (want x,y,w,h)";
        return false;
      }
      a.have_roi = true;
    } else if (key == "rgb") {
      char* end = nullptr;
      unsigned long x = std::strtoul(v, &end, 16);
      if (!end || end == v || *end != '\0' || x > 0xFFFFFF) {
        err = "bad --rgb";
        return false;
      }
      a.rgb = static_cast<unsigned>(x);
    } else if (key == "tolerance") {
      if (!parse_int(v, a.tolerance) || a.tolerance < 0 || a.tolerance > 255) {
        err = "bad --tolerance";
        return false;
      }
    } else if (key == "expected-width") {
      if (!parse_int(v, a.expected_width)) { err = "bad --expected-width"; return false; }
    } else if (key == "width-tolerance") {
      if (!parse_int(v, a.width_tolerance) || a.width_tolerance < 0) {
        err = "bad --width-tolerance";
        return false;
      }
    } else if (key == "max-duration-ms") {
      unsigned long long u = 0;
      if (!parse_u64(v, u) || u == 0) { err = "bad --max-duration-ms"; return false; }
      a.max_duration_ms = static_cast<long long>(u);
    } else if (key == "max-frames") {
      unsigned long long u = 0;
      if (!parse_u64(v, u) || u == 0) { err = "bad --max-frames"; return false; }
      a.max_frames = static_cast<long long>(u);
    } else if (key == "max-output-bytes") {
      unsigned long long u = 0;
      if (!parse_u64(v, u) || u == 0) { err = "bad --max-output-bytes"; return false; }
      a.max_output_bytes = static_cast<long long>(u);
    } else if (key == "out-dir") {
      a.out_dir = utf8_to_wide(val);
      if (a.out_dir.empty()) { err = "bad --out-dir"; return false; }
    } else if (key == "ready-file") {
      a.ready_file = utf8_to_wide(val);
    } else if (key == "stop-file") {
      a.stop_file = utf8_to_wide(val);
    } else if (key == "acquire-timeout-ms") {
      int t = 0;
      if (!parse_int(v, t) || t < 0 || t > 1000) {
        err = "bad --acquire-timeout-ms";
        return false;
      }
      a.acquire_timeout_ms = static_cast<unsigned>(t);
    } else if (key == "warmup-ms") {
      unsigned long long u = 0;
      if (!parse_u64(v, u)) { err = "bad --warmup-ms"; return false; }
      a.warmup_ms = static_cast<long long>(u);
    } else if (key == "stale-ms") {
      unsigned long long u = 0;
      if (!parse_u64(v, u) || u == 0) { err = "bad --stale-ms"; return false; }
      a.stale_ms = static_cast<long long>(u);
    } else {
      err = "unknown option --" + key;
      return false;
    }
  }

  if (a.self_test || a.help) return true;

  if (!a.have_pid || !a.have_hwnd) {
    err = "--pid and --hwnd are required for a real run";
    return false;
  }
  if (!a.have_bounds || !a.have_dpi) {
    err = "--bounds and --dpi are required";
    return false;
  }
  if (!a.have_roi) {
    err = "--roi is required";
    return false;
  }
  if (a.expected_width <= 0) {
    err = "--expected-width (>0) is required for a real run";
    return false;
  }
  if (a.monitor.empty()) {
    err = "--monitor is required";
    return false;
  }
  if (a.out_dir.empty()) {
    err = "--out-dir is required";
    return false;
  }
  if (!validate_roi(a.rx, a.ry, a.rw, a.rh, a.bleft, a.btop,
                    a.bright - a.bleft, a.bbottom - a.btop, true, err)) {
    return false;
  }
  return true;
}

static void print_usage() {
  std::printf(
      "app-screen-observer %s — bounded DXGI Desktop Duplication observer\n"
      "\n"
      "Real run:\n"
      "  --pid=N            target process id\n"
      "  --hwnd=0xNNN       target top-level owner window (must belong to --pid)\n"
      "  --monitor=\\.\\DISPLAY2\n"
      "  --bounds=l,t,r,b   expected physical monitor bounds (desktop coords)\n"
      "  --dpi=144          expected effective monitor DPI\n"
      "  --roi=x,y,w,h      narrow ROI strip in desktop coords (<=1024x64)\n"
      "  --rgb=204080       expected target RGB (hex), default 204080\n"
      "  --tolerance=8      per-channel tolerance, default 8\n"
      "  --expected-width=N required target width (drift is rejected)\n"
      "  --width-tolerance=2\n"
      "  --max-duration-ms=40000 --max-frames=200000 --max-output-bytes=67108864\n"
      "  --out-dir=DIR      new (non-existing) output directory\n"
      "  --ready-file=PATH  default OUTDIR\\observer-ready.json\n"
      "  --stop-file=PATH   default OUTDIR\\observer-stop\n"
      "  --acquire-timeout-ms=20 --warmup-ms=5000 --stale-ms=2000\n"
      "\n"
      "Pure test:\n"
      "  --self-test\n",
      OBS_VERSION);
}

// ---------------------------------------------------------------------------
// Observer state.
// ---------------------------------------------------------------------------
struct Counts {
  long long acquires = 0;
  long long timeouts = 0;
  long long pointer_only = 0;
  long long skipped = 0;
  long long merged = 0;
  long long valid = 0;
  long long missing = 0;
  long long ambiguous = 0;
  long long cursor_overlap = 0;
  long long stale = 0;
  long long errors = 0;
  long long content_changes = 0;
  long long signature_changes = 0;
  long long frames_written = 0;
};

struct TargetRef {
  DWORD session = 0;
  bool elevated = false;
  std::string image_path;
};

static bool query_elevation(HANDLE proc, bool& elevated, std::string& err) {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
    err = "OpenProcessToken failed " + last_error_hex();
    return false;
  }
  TOKEN_ELEVATION te;
  std::memset(&te, 0, sizeof(te));
  DWORD ret = 0;
  BOOL ok = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &ret);
  CloseHandle(tok);
  if (!ok) {
    err = "GetTokenInformation(TokenElevation) failed " + last_error_hex();
    return false;
  }
  elevated = te.TokenIsElevated != 0;
  return true;
}

static bool check_environment(const Args& a, TargetRef& tgt, std::string& err) {
  // PMv2 must be established before geometry. ERROR_ACCESS_DENIED means it was
  // already set for the process, but that alone is not proof: verify the actual
  // thread awareness is PMv2.
  if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    DWORD e = GetLastError();
    if (e != ERROR_ACCESS_DENIED) {
      err = "SetProcessDpiAwarenessContext(PMv2) failed " + last_error_hex();
      return false;
    }
  }
  if (!AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    err = "thread DPI awareness is not PER_MONITOR_AWARE_V2";
    return false;
  }

  if (GetSystemMetrics(SM_REMOTESESSION) != 0) {
    err = "running in an RDP/remote session (SM_REMOTESESSION != 0)";
    return false;
  }

  DWORD self_session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &self_session)) {
    err = "ProcessIdToSessionId(self) failed " + last_error_hex();
    return false;
  }
  if (self_session == 0) {
    err = "observer is in session 0 (no interactive desktop)";
    return false;
  }
  DWORD active = WTSGetActiveConsoleSessionId();
  if (active == 0xFFFFFFFF || self_session != active) {
    err = "observer session is not the active console session";
    return false;
  }

  bool self_elevated = false;
  if (!query_elevation(GetCurrentProcess(), self_elevated, err)) return false;
  if (self_elevated) {
    err = "observer process is elevated; refusing to observe";
    return false;
  }

  HANDLE target =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                  static_cast<DWORD>(a.pid));
  if (!target) {
    err = "OpenProcess(target) failed " + last_error_hex();
    return false;
  }
  bool target_elevated = false;
  std::string eerr;
  bool ok = query_elevation(target, target_elevated, eerr);
  CloseHandle(target);
  if (!ok) {
    err = "target elevation query failed: " + eerr;
    return false;
  }
  if (target_elevated) {
    err = "target process is elevated; refusing to observe";
    return false;
  }

  DWORD target_session = 0;
  if (!ProcessIdToSessionId(static_cast<DWORD>(a.pid), &target_session)) {
    err = "ProcessIdToSessionId(target) failed " + last_error_hex();
    return false;
  }
  if (target_session != self_session) {
    err = "target process is in a different session";
    return false;
  }

  HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(a.hwnd));
  if (!IsWindow(hwnd)) {
    err = "--hwnd is not a valid window";
    return false;
  }
  if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
    err = "--hwnd is not a top-level owner window";
    return false;
  }
  DWORD wpid = 0;
  GetWindowThreadProcessId(hwnd, &wpid);
  if (wpid != static_cast<DWORD>(a.pid)) {
    err = "--hwnd owner process does not match --pid";
    return false;
  }

  tgt.session = target_session;
  tgt.elevated = target_elevated;
  return true;
}

// ---------------------------------------------------------------------------
// Self-test (pure; no COM/D3D/desktop).
// ---------------------------------------------------------------------------
static std::vector<uint8_t> make_strip(int w, int h, bool bgra, int r, int g,
                                       int b, int x0, int x1, int bg_r, int bg_g,
                                       int bg_b) {
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4, 0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* px = &buf[(static_cast<size_t>(y) * w + x) * 4];
      int cr = (x >= x0 && x <= x1 && x1 >= 0) ? r : bg_r;
      int cg = (x >= x0 && x <= x1 && x1 >= 0) ? g : bg_g;
      int cb = (x >= x0 && x <= x1 && x1 >= 0) ? b : bg_b;
      if (bgra) {
        px[0] = static_cast<uint8_t>(cb);
        px[1] = static_cast<uint8_t>(cg);
        px[2] = static_cast<uint8_t>(cr);
        px[3] = 255;
      } else {
        px[0] = static_cast<uint8_t>(cr);
        px[1] = static_cast<uint8_t>(cg);
        px[2] = static_cast<uint8_t>(cb);
        px[3] = 255;
      }
    }
  }
  return buf;
}

static int run_self_test() {
  int pass = 0, total = 0;
  auto check = [&](const char* name, bool ok, const std::string& detail) {
    ++total;
    if (ok) ++pass;
    std::printf("SELFTEST case=%-24s ok=%d %s\n", name, ok ? 1 : 0,
                detail.c_str());
  };

  const PixelFormat bgra{true, "B8G8R8A8_UNORM"};
  const int er = 0x20, eg = 0x40, eb = 0x80;

  // 1. known rectangle
  {
    int w = 64, h = 4;
    auto buf = make_strip(w, h, true, er, eg, eb, 10, 29, 0, 0, 0);
    ScanResult sr = scan_strip(buf.data(), w, h, w * 4, bgra, er, eg, eb, 8, -1,
                               2, 2);
    bool ok = sr.cls == TargetClass::Valid && sr.left == 10 && sr.right == 29 &&
              sr.width == 20 && sr.runs_total == 4 && !sr.width_drift;
    check("known_rect", ok,
          "cls=" + std::to_string(static_cast<int>(sr.cls)) +
              " left=" + std::to_string(sr.left) +
              " right=" + std::to_string(sr.right) +
              " width=" + std::to_string(sr.width));
  }

  // 2. disconnected distractor: a second same-colour run in the same row
  {
    int w = 64, h = 4;
    auto buf = make_strip(w, h, true, er, eg, eb, 10, 29, 0, 0, 0);
    for (int y = 0; y < h; ++y) {
      for (int x = 40; x <= 45; ++x) {
        uint8_t* px = &buf[(static_cast<size_t>(y) * w + x) * 4];
        px[0] = static_cast<uint8_t>(eb);
        px[1] = static_cast<uint8_t>(eg);
        px[2] = static_cast<uint8_t>(er);
      }
    }
    ScanResult sr = scan_strip(buf.data(), w, h, w * 4, bgra, er, eg, eb, 8, -1,
                               2, 2);
    bool ok = sr.cls == TargetClass::Ambiguous && sr.max_runs_in_row == 2;
    check("disconnected_distractor", ok,
          "cls=" + std::to_string(static_cast<int>(sr.cls)) +
              " max_runs=" + std::to_string(sr.max_runs_in_row));
  }

  // 3. absent target
  {
    int w = 64, h = 4;
    auto buf = make_strip(w, h, true, er, eg, eb, -1, -1, 0, 0, 0);
    ScanResult sr = scan_strip(buf.data(), w, h, w * 4, bgra, er, eg, eb, 8, -1,
                               2, 2);
    bool ok = sr.cls == TargetClass::Missing && sr.rows_with_run == 0;
    check("absent_target", ok,
          "cls=" + std::to_string(static_cast<int>(sr.cls)) +
              " rows_with_run=" + std::to_string(sr.rows_with_run));
  }

  // 4. width drift vs expected width must NOT be admitted as valid
  {
    int w = 64, h = 4;
    auto buf = make_strip(w, h, true, er, eg, eb, 10, 24, 0, 0, 0);
    ScanResult sr = scan_strip(buf.data(), w, h, w * 4, bgra, er, eg, eb, 8, 20,
                               2, 2);
    bool ok = sr.cls != TargetClass::Valid && sr.width == 15 &&
              sr.width_drift && sr.reason == "width_drift";
    check("width_drift_rejected", ok,
          "cls=" + std::to_string(static_cast<int>(sr.cls)) +
              " width=" + std::to_string(sr.width) +
              " drift=" + std::to_string(sr.width_drift) +
              " reason=" + sr.reason);
  }

  // 4b. clipped/partial rectangle (one strip row without the target) rejected
  {
    int w = 64, h = 4;
    auto buf = make_strip(w, h, true, er, eg, eb, 10, 29, 0, 0, 0);
    for (int x = 0; x < w; ++x) {
      uint8_t* px = &buf[(static_cast<size_t>(3) * w + x) * 4];
      px[0] = 0; px[1] = 0; px[2] = 0;
    }
    ScanResult sr = scan_strip(buf.data(), w, h, w * 4, bgra, er, eg, eb, 8, -1,
                               2, 2);
    bool ok = sr.cls != TargetClass::Valid && sr.reason == "partial_rows" &&
              sr.rows_with_run == 3 && sr.rows_scanned == 4;
    check("partial_rows_rejected", ok,
          "cls=" + std::to_string(static_cast<int>(sr.cls)) +
              " reason=" + sr.reason +
              " rows=" + std::to_string(sr.rows_with_run) + "/" +
              std::to_string(sr.rows_scanned));
  }

  // 4c. target motion sequence A->B->B->(background-only)->C: 1,0,0,1;
  //     signature change without edge movement must not be motion.
  {
    int w = 64, h = 4;
    auto buf_a = make_strip(w, h, true, er, eg, eb, 10, 29, 0, 0, 0);
    auto buf_b = make_strip(w, h, true, er, eg, eb, 40, 59, 0, 0, 0);
    auto buf_b_bg = make_strip(w, h, true, er, eg, eb, 40, 59, 10, 20, 30);
    auto buf_c = make_strip(w, h, true, er, eg, eb, 20, 39, 0, 0, 0);
    ScanResult sa = scan_strip(buf_a.data(), w, h, w * 4, bgra, er, eg, eb, 8,
                               -1, 2, 2);
    ScanResult sb = scan_strip(buf_b.data(), w, h, w * 4, bgra, er, eg, eb, 8,
                               -1, 2, 2);
    ScanResult sb_bg = scan_strip(buf_b_bg.data(), w, h, w * 4, bgra, er, eg,
                                  eb, 8, -1, 2, 2);
    ScanResult sc = scan_strip(buf_c.data(), w, h, w * 4, bgra, er, eg, eb, 8,
                               -1, 2, 2);
    MotionTracker t;
    t.seed(sa.left, sa.right, sa.signature);
    bool s1 = false, s2 = false, s3 = false, s4 = false;
    bool m1 = t.observe(sb, &s1);
    bool m2 = t.observe(sb, &s2);
    bool m3 = t.observe(sb_bg, &s3);
    bool m4 = t.observe(sc, &s4);
    bool ok = m1 && !m2 && !m3 && m4 && !s2 && s3;
    check("motion_sequence", ok,
          std::string("motion=") + std::to_string(m1) + std::to_string(m2) +
              std::to_string(m3) + std::to_string(m4) +
              " sig_change_mid=" + std::to_string(s3));
  }

  // 5. pointer-only frame classification
  {
    FrameClass po = classify_frame(0, 0);
    FrameClass ct = classify_frame(123456789, 1);
    bool ok = po.pointer_only && !po.content_eligible && !ct.pointer_only &&
              ct.content_eligible;
    check("pointer_only_classify", ok,
          std::string("zero=") + (po.pointer_only ? "pointer" : "content") +
              " nonzero=" + (ct.content_eligible ? "content" : "pointer"));
  }

  // 6. ROI/bounds failures
  {
    std::string err;
    bool zero = validate_roi(0, 0, 0, 4, 0, 0, 3840, 2160, true, err);
    std::string e1 = err;
    bool neg = validate_roi(-1, 0, 16, 4, 0, 0, 3840, 2160, true, err);
    std::string e2 = err;
    bool outside = validate_roi(3830, 0, 16, 4, 0, 0, 3840, 2160, true, err);
    std::string e3 = err;
    bool toowide = validate_roi(0, 0, 2000, 4, -4000, 0, 3840, 2160, true, err);
    std::string e4 = err;
    bool ok = !zero && !neg && !outside && !toowide;
    check("bounds_failures", ok,
          std::string("zero=") + e1 + " outside=" + e3);
  }

  // 7. parser errors are nonzero-returning, good args parse
  {
    const char* bad1[] = {"obs", "--roi=1,2,3"};
    const char* bad2[] = {"obs", "--pid=notanumber", "--hwnd=0"};
    const char* bad3[] = {"obs", "--bogus=1"};
    const char* bad4[] = {"obs", "--pid="};
    const char* bad5[] = {"obs", "--rgb="};
    Args a;
    std::string err;
    bool b1 = !parse_args(2, bad1, a, err);
    bool b2 = !parse_args(3, bad2, a, err);
    bool b3 = !parse_args(2, bad3, a, err);
    bool b4 = !parse_args(2, bad4, a, err);
    bool b5 = !parse_args(2, bad5, a, err);
    const char* good[] = {
        "obs",          "--pid=1234",   "--hwnd=0x1001",
        "--monitor=\\\\.\\DISPLAY2", "--bounds=0,0,3840,2160",
        "--dpi=144",    "--roi=100,100,64,16", "--expected-width=64",
        "--out-dir=C:\\tmp\\obs"};
    Args ga;
    std::string gerr;
    bool bg = parse_args(9, good, ga, gerr);
    bool ok = b1 && b2 && b3 && b4 && b5 && bg && ga.have_roi &&
              ga.pid == 1234 && ga.hwnd == 0x1001 && ga.dpi == 144;
    check("parser_errors_nonzero", ok,
          std::string("badRejected=") +
              std::to_string(b1 && b2 && b3 && b4 && b5) +
              " goodAccepted=" + std::to_string(bg) + " gerr=" + gerr);
  }

  // 8. BGRA vs RGBA signatures must agree for the same visual image
  {
    int w = 32, h = 2;
    auto bgra_buf = make_strip(w, h, true, er, eg, eb, 4, 20, 1, 2, 3);
    auto rgba_buf = make_strip(w, h, false, er, eg, eb, 4, 20, 1, 2, 3);
    const PixelFormat rgba{false, "R8G8B8A8_UNORM"};
    ScanResult sb = scan_strip(bgra_buf.data(), w, h, w * 4, bgra, er, eg, eb, 8,
                               -1, 2, 2);
    ScanResult sr = scan_strip(rgba_buf.data(), w, h, w * 4, rgba, er, eg, eb, 8,
                               -1, 2, 2);
    bool ok = sb.signature == sr.signature && sb.left == sr.left &&
              sb.width == sr.width;
    check("bgra_rgba_signature", ok,
          "sig_equal=" + std::to_string(sb.signature == sr.signature));
  }

  std::printf("SELFTEST RESULT: %s (%d/%d)\n", pass == total ? "PASS" : "FAIL",
              pass, total);
  return pass == total ? 0 : 5;
}

// ---------------------------------------------------------------------------
// Real observer.
// ---------------------------------------------------------------------------
struct Identity {
  std::wstring adapter_desc;
  LUID adapter_luid{};
  std::wstring output_device;
  RECT output_rect{};
  UINT desc_w = 0, desc_h = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
  bool desktop_in_system_memory = false;
  std::string format_name;
  bool bgra = true;
};

struct MetaWriter {
  Args args;
  Counts counts;
  Identity id;
  TargetRef target;
  std::wstring out_dir;
  std::string exit_reason = "not_started";
  int exit_code = 0;
  bool fatal = false;

  long long freq = 0;
  long long qpc_start = 0;
  long long qpc_ready = 0;
  long long qpc_end = 0;
  long long warmup_start = 0;
  long long cpu_kernel_ms = 0, cpu_user_ms = 0;

  bool baseline_set = false;
  int base_left = -1, base_right = -1, base_width = -1;
  unsigned long long base_sig = 0;
  long long base_qpc = 0;
};

static void write_metadata(const MetaWriter& m) {
  std::wstring p = join_path(m.out_dir, L"observer-metadata.json");
  FILE* f = _wfopen(p.c_str(), L"wb");
  if (!f) return;
  char rgb_hex[8];
  std::snprintf(rgb_hex, sizeof(rgb_hex), "%06X", m.args.rgb);
  std::fprintf(f, "{\n");
  std::fprintf(f, "  \"schema\": \"vacards.screen-observer/1\",\n");
  std::fprintf(f, "  \"observer_version\": \"%s\",\n", OBS_VERSION);
  std::fprintf(f, "  \"qpc_frequency\": %lld,\n", m.freq);
  std::fprintf(f, "  \"qpc_start\": %lld,\n", m.qpc_start);
  std::fprintf(f, "  \"qpc_ready\": %lld,\n", m.qpc_ready);
  std::fprintf(f, "  \"qpc_end\": %lld,\n", m.qpc_end);
  std::fprintf(f, "  \"warmup_start_qpc\": %lld,\n", m.warmup_start);
  std::fprintf(f, "  \"cpu_user_ms\": %lld,\n", m.cpu_user_ms);
  std::fprintf(f, "  \"cpu_kernel_ms\": %lld,\n", m.cpu_kernel_ms);
  std::fprintf(f, "  \"exit_code\": %d,\n", m.exit_code);
  std::fprintf(f, "  \"exit_reason\": \"%s\",\n",
               json_escape(m.exit_reason).c_str());
  std::fprintf(f, "  \"fatal\": %s,\n", m.fatal ? "true" : "false");
  std::fprintf(f, "  \"environment\": {\n");
  std::fprintf(f, "    \"current_process_id\": %lu,\n",
               static_cast<unsigned long>(GetCurrentProcessId()));
  std::fprintf(f, "    \"session\": %lu,\n",
               static_cast<unsigned long>(m.target.session));
  std::fprintf(f, "    \"remote_session\": %d,\n",
               GetSystemMetrics(SM_REMOTESESSION));
  std::fprintf(f, "    \"monitor_device\": \"%s\",\n",
               json_escape(wide_to_utf8(m.args.monitor)).c_str());
  std::fprintf(f, "    \"expected_bounds\": [%d,%d,%d,%d],\n", m.args.bleft,
               m.args.btop, m.args.bright, m.args.bbottom);
  std::fprintf(f, "    \"expected_dpi\": %u,\n", m.args.dpi);
  std::fprintf(f, "    \"adapter_description\": \"%s\",\n",
               json_escape(wide_to_utf8(m.id.adapter_desc)).c_str());
  std::fprintf(f, "    \"adapter_luid\": %lld,\n",
               static_cast<long long>(m.id.adapter_luid.LowPart) +
                   (static_cast<long long>(m.id.adapter_luid.HighPart) << 32));
  std::fprintf(f, "    \"output_device\": \"%s\",\n",
               json_escape(wide_to_utf8(m.id.output_device)).c_str());
  std::fprintf(f, "    \"output_rect\": [%ld,%ld,%ld,%ld],\n",
               static_cast<long>(m.id.output_rect.left),
               static_cast<long>(m.id.output_rect.top),
               static_cast<long>(m.id.output_rect.right),
               static_cast<long>(m.id.output_rect.bottom));
  std::fprintf(f, "    \"actual_desc_width\": %u,\n", m.id.desc_w);
  std::fprintf(f, "    \"actual_desc_height\": %u,\n", m.id.desc_h);
  std::fprintf(f, "    \"actual_format\": \"%s\",\n",
               m.id.format_name.c_str());
  std::fprintf(f, "    \"actual_rotation\": %d,\n",
               static_cast<int>(m.id.rotation));
  std::fprintf(f, "    \"desktop_image_in_system_memory\": %s,\n",
               m.id.desktop_in_system_memory ? "true" : "false");
  std::fprintf(f, "    \"target_pid\": %llu,\n", m.args.pid);
  std::fprintf(f, "    \"target_hwnd\": %llu,\n", m.args.hwnd);
  std::fprintf(f, "    \"target_session\": %lu,\n",
               static_cast<unsigned long>(m.target.session));
  std::fprintf(f, "    \"target_elevated\": %s\n",
               m.target.elevated ? "true" : "false");
  std::fprintf(f, "  },\n");
  std::fprintf(f, "  \"roi\": [%d,%d,%d,%d],\n", m.args.rx, m.args.ry,
               m.args.rw, m.args.rh);
  std::fprintf(f, "  \"expected_rgb\": \"#%s\",\n", rgb_hex);
  std::fprintf(f, "  \"tolerance\": %d,\n", m.args.tolerance);
  std::fprintf(f, "  \"expected_width\": %d,\n", m.args.expected_width);
  std::fprintf(f, "  \"limits\": {\"max_duration_ms\": %lld, \"max_frames\": %lld, "
                  "\"max_output_bytes\": %lld},\n",
               m.args.max_duration_ms, m.args.max_frames, m.args.max_output_bytes);
  std::fprintf(f, "  \"baseline\": {\"set\": %s, \"left\": %d, \"right\": %d, "
                  "\"width\": %d, \"signature\": \"0x%016llX\", \"qpc\": %lld, "
                  "\"note\": \"first valid target frame, outside measurement\"},\n",
               m.baseline_set ? "true" : "false", m.base_left, m.base_right,
               m.base_width, m.base_sig, m.base_qpc);
  std::fprintf(f, "  \"counts\": {\n");
  std::fprintf(f, "    \"acquires\": %lld,\n", m.counts.acquires);
  std::fprintf(f, "    \"timeouts\": %lld,\n", m.counts.timeouts);
  std::fprintf(f, "    \"pointer_only\": %lld,\n", m.counts.pointer_only);
  std::fprintf(f, "    \"skipped\": %lld,\n", m.counts.skipped);
  std::fprintf(f, "    \"merged\": %lld,\n", m.counts.merged);
  std::fprintf(f, "    \"valid\": %lld,\n", m.counts.valid);
  std::fprintf(f, "    \"missing\": %lld,\n", m.counts.missing);
  std::fprintf(f, "    \"ambiguous\": %lld,\n", m.counts.ambiguous);
  std::fprintf(f, "    \"cursor_overlap\": %lld,\n", m.counts.cursor_overlap);
  std::fprintf(f, "    \"stale\": %lld,\n", m.counts.stale);
  std::fprintf(f, "    \"errors\": %lld,\n", m.counts.errors);
  std::fprintf(f, "    \"content_changes\": %lld,\n", m.counts.content_changes);
  std::fprintf(f, "    \"signature_changes\": %lld,\n",
               m.counts.signature_changes);
  std::fprintf(f, "    \"frames_written\": %lld\n", m.counts.frames_written);
  std::fprintf(f, "  },\n");
  std::fprintf(f, "  \"note\": \"LastPresentTime is compositor-observed software "
                  "presentation in QPC units; NOT photons, NOT physical display "
                  "FPS, NOT GDI completion.\"\n");
  std::fprintf(f, "}\n");
  std::fclose(f);
}

static bool write_ready_file(const MetaWriter& m) {
  if (m.args.ready_file.empty()) return true;
  FILE* f = _wfopen(m.args.ready_file.c_str(), L"wb");
  if (!f) return false;
  std::fprintf(f, "{\"ready\": true, \"qpc_ready\": %lld, \"qpc_frequency\": %lld, "
                  "\"baseline\": {\"left\": %d, \"right\": %d, \"width\": %d, "
                  "\"signature\": \"0x%016llX\"}}\n",
               m.qpc_ready, m.freq, m.base_left, m.base_right, m.base_width,
               m.base_sig);
  std::fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
// Acquire/release frame guard.
// ---------------------------------------------------------------------------
struct FrameGuard {
  IDXGIOutputDuplication* d = nullptr;
  bool armed = false;
  explicit FrameGuard(IDXGIOutputDuplication* dd) : d(dd), armed(true) {}
  ~FrameGuard() {
    if (armed && d) d->ReleaseFrame();
  }
  HRESULT Release() {
    if (!armed) return S_OK;
    armed = false;
    return d->ReleaseFrame();
  }
};

static bool rects_overlap(const RECT& x, const RECT& y) {
  RECT out;
  return IntersectRect(&out, &x, &y) != 0;
}

// Conservative cursor-region overlap test against the ROI.
//
// The cursor hotspot is the reference point, but the bitmap can extend left/up,
// so the actual bitmap dimensions and hotspot (GetIconInfo) are used where
// available, with a symmetric conservative fallback otherwise. The duplication
// frame's pointer metadata position is also checked, interpreted both as
// desktop coordinates and as output-local coordinates translated by the output
// origin, so a convention mismatch cannot hide a cursor inside the ROI.
// Limits: if GetCursorInfo fails the result is "overlap" (fail closed); shape
// changes between the query and the captured frame are not modelled.
static bool cursor_overlaps(const Args& a, int origin_x, int origin_y,
                            const DXGI_OUTDUPL_FRAME_INFO* info) {
  RECT roi{a.rx, a.ry, a.rx + a.rw, a.ry + a.rh};

  CURSORINFO ci;
  std::memset(&ci, 0, sizeof(ci));
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci)) return true;  // fail closed: cursor state unknown

  if ((ci.flags & CURSOR_SHOWING) != 0) {
    int cw = GetSystemMetrics(SM_CXCURSOR);
    int ch = GetSystemMetrics(SM_CYCURSOR);
    int hot_x = 0, hot_y = 0;
    if (ci.hCursor) {
      ICONINFO ii;
      std::memset(&ii, 0, sizeof(ii));
      if (GetIconInfo(ci.hCursor, &ii)) {
        BITMAP bm;
        std::memset(&bm, 0, sizeof(bm));
        HBITMAP hb = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
        if (hb && GetObjectW(hb, sizeof(bm), &bm) && bm.bmWidth > 0 &&
            bm.bmHeight > 0) {
          cw = static_cast<int>(bm.bmWidth);
          ch = ii.hbmColor ? static_cast<int>(bm.bmHeight)
                           : static_cast<int>(bm.bmHeight / 2);  // mask is 2x
        }
        hot_x = static_cast<int>(ii.xHotspot);
        hot_y = static_cast<int>(ii.yHotspot);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
      }
    }
    RECT cr{ci.ptScreenPos.x - hot_x, ci.ptScreenPos.y - hot_y,
            ci.ptScreenPos.x - hot_x + cw, ci.ptScreenPos.y - hot_y + ch};
    if (rects_overlap(cr, roi)) return true;
  }

  if (info && info->PointerPosition.Visible) {
    int cw = GetSystemMetrics(SM_CXCURSOR);
    int ch = GetSystemMetrics(SM_CYCURSOR);
    POINT p = info->PointerPosition.Position;
    RECT b0{p.x - cw, p.y - ch, p.x + cw, p.y + ch};
    POINT q{p.x + origin_x, p.y + origin_y};
    RECT b1{q.x - cw, q.y - ch, q.x + cw, q.y + ch};
    if (rects_overlap(b0, roi) || rects_overlap(b1, roi)) return true;
  }
  return false;
}

static std::string format_name(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    default: {
      char b[32];
      std::snprintf(b, sizeof(b), "DXGI_FORMAT_%d", static_cast<int>(f));
      return b;
    }
  }
}

static int run_real(const Args& a) {
  MetaWriter m;
  m.args = a;
  m.out_dir = a.out_dir;
  m.qpc_start = qpc_now();

  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  m.freq = f.QuadPart;
  if (m.freq <= 0) {
    std::fprintf(stderr, "FATAL: QueryPerformanceFrequency failed\n");
    return 4;
  }

  if (m.args.ready_file.empty())
    m.args.ready_file = join_path(a.out_dir, L"observer-ready.json");
  if (m.args.stop_file.empty())
    m.args.stop_file = join_path(a.out_dir, L"observer-stop");

  // New output directory only.
  if (!CreateDirectoryW(a.out_dir.c_str(), nullptr)) {
    DWORD e = GetLastError();
    std::string err = (e == ERROR_ALREADY_EXISTS)
                          ? "output directory already exists"
                          : ("CreateDirectoryW failed " + last_error_hex());
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    return 3;
  }

  FILE* csv = _wfopen(join_path(a.out_dir, L"frames.csv").c_str(), L"wb");
  if (!csv) {
    std::fprintf(stderr, "FATAL: cannot open frames.csv\n");
    m.exit_reason = "csv_open_failed";
    m.exit_code = 3;
    write_metadata(m);
    return 3;
  }
  std::fprintf(csv,
               "seq,phase,acquired_qpc,qpc_before,qpc_after,last_present_time,"
               "last_present_sec,last_mouse_update_time,accumulated_frames,"
               "copied,cursor_overlap,map_start_qpc,map_end_qpc,rows_scanned,"
               "rows_with_run,max_runs_in_row,left,right,width,width_drift,"
               "valid,scan_reason,content_changed,signature_changed,signature,"
               "stale,pointer_only,error\n");

  // Environment + target gates.
  std::string err;
  if (!check_environment(a, m.target, err)) {
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = "environment: " + err;
    m.exit_code = 3;
    m.fatal = true;
    std::fflush(csv);
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  // Enumerate adapters/outputs and match the requested monitor.
  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1,
                                  reinterpret_cast<void**>(factory.put()));
  if (FAILED(hr)) {
    err = "CreateDXGIFactory1 failed " + hr_hex(hr);
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput> output;
  bool found = false;
  std::string seen;
  for (UINT ai = 0;; ++ai) {
    hr = factory->EnumAdapters1(ai, adapter.put());
    if (hr == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(hr)) {
      err = "EnumAdapters1 failed " + hr_hex(hr);
      break;
    }
    DXGI_ADAPTER_DESC1 ad;
    std::memset(&ad, 0, sizeof(ad));
    adapter->GetDesc1(&ad);
    for (UINT oi = 0;; ++oi) {
      ComPtr<IDXGIOutput> cand;
      hr = adapter->EnumOutputs(oi, cand.put());
      if (hr == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(hr)) break;
      DXGI_OUTPUT_DESC od;
      std::memset(&od, 0, sizeof(od));
      cand->GetDesc(&od);
      seen += " [" + wide_to_utf8(od.DeviceName) + "]";
      bool name_ok = (std::wcscmp(od.DeviceName, a.monitor.c_str()) == 0);
      bool rect_ok = od.DesktopCoordinates.left == a.bleft &&
                     od.DesktopCoordinates.top == a.btop &&
                     od.DesktopCoordinates.right == a.bright &&
                     od.DesktopCoordinates.bottom == a.bbottom;
      if (name_ok && rect_ok) {
        output.attach(cand.get());
        cand.detach();
        m.id.adapter_desc = ad.Description;
        m.id.adapter_luid = ad.AdapterLuid;
        DXGI_OUTPUT_DESC od2;
        std::memset(&od2, 0, sizeof(od2));
        output->GetDesc(&od2);
        m.id.output_device = od2.DeviceName;
        m.id.output_rect = od2.DesktopCoordinates;
        found = true;
        break;
      }
    }
    if (found) break;
  }
  if (!found) {
    err = "no adapter/output matches --monitor and --bounds; saw" + seen;
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  // Monitor geometry + rotation.
  DXGI_OUTPUT_DESC od;
  std::memset(&od, 0, sizeof(od));
  output->GetDesc(&od);
  if (od.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
    err = "output is rotated; refusing (rotation must be IDENTITY)";
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }
  MONITORINFOEXW mi;
  std::memset(&mi, 0, sizeof(mi));
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(od.Monitor, &mi)) {
    err = "GetMonitorInfoW failed " + last_error_hex();
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }
  if (mi.rcMonitor.left != a.bleft || mi.rcMonitor.top != a.btop ||
      mi.rcMonitor.right != a.bright || mi.rcMonitor.bottom != a.bbottom) {
    err = "monitor geometry does not match --bounds";
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }
  UINT dpi_x = 0, dpi_y = 0;
  HRESULT dhr = GetDpiForMonitor(od.Monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
  if (FAILED(dhr) || dpi_x != a.dpi || dpi_y != a.dpi) {
    char b[96];
    std::snprintf(b, sizeof(b), "DPI mismatch: got %u,%u want %u (hr=%s)",
                  dpi_x, dpi_y, a.dpi, hr_hex(dhr).c_str());
    err = b;
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  // D3D11 device on the matched adapter.
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_10_0;
  hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                         static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                         D3D11_SDK_VERSION, device.put(), &obtained,
                         context.put());
  if (FAILED(hr)) {
    err = "D3D11CreateDevice failed " + hr_hex(hr);
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  hr = device->QueryInterface(IID_IDXGIDevice,
                              reinterpret_cast<void**>(dxgiDevice.put()));
  if (FAILED(hr)) {
    err = "QI IDXGIDevice failed " + hr_hex(hr);
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }
  ComPtr<IDXGIAdapter> dev_adapter;
  hr = dxgiDevice->GetAdapter(dev_adapter.put());
  if (FAILED(hr)) {
    err = "IDXGIDevice::GetAdapter failed " + hr_hex(hr);
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }
  DXGI_ADAPTER_DESC dad;
  std::memset(&dad, 0, sizeof(dad));
  dev_adapter->GetDesc(&dad);
  if (dad.AdapterLuid.LowPart != m.id.adapter_luid.LowPart ||
      dad.AdapterLuid.HighPart != m.id.adapter_luid.HighPart) {
    err = "device adapter LUID does not match the enumerated output adapter";
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  ComPtr<IDXGIOutput1> output1;
  hr = output->QueryInterface(IID_IDXGIOutput1,
                              reinterpret_cast<void**>(output1.put()));
  if (FAILED(hr)) {
    err = "QI IDXGIOutput1 failed " + hr_hex(hr);
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }
  ComPtr<IDXGIOutputDuplication> dupl;
  hr = output1->DuplicateOutput(device.get(), dupl.put());
  if (FAILED(hr)) {
    err = "DuplicateOutput failed " + hr_hex(hr);
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  DXGI_OUTDUPL_DESC dd;
  std::memset(&dd, 0, sizeof(dd));
  dupl->GetDesc(&dd);
  m.id.desc_w = dd.ModeDesc.Width;
  m.id.desc_h = dd.ModeDesc.Height;
  m.id.format = dd.ModeDesc.Format;
  m.id.rotation = dd.Rotation;
  m.id.desktop_in_system_memory = dd.DesktopImageInSystemMemory != 0;
  m.id.format_name = format_name(dd.ModeDesc.Format);

  if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
    err = "duplication descriptor reports rotation; refusing";
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }
  if (dd.ModeDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
      dd.ModeDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
    m.id.bgra = (dd.ModeDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
  } else {
    err = "unsupported duplicated texture format: " + m.id.format_name;
    std::fprintf(stderr, "FATAL: %s\n", err.c_str());
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  int lx = a.rx - m.id.output_rect.left;
  int ly = a.ry - m.id.output_rect.top;
  if (lx < 0 || ly < 0 || lx + a.rw > static_cast<int>(m.id.desc_w) ||
      ly + a.rh > static_cast<int>(m.id.desc_h)) {
    err = "roi is outside the actual output descriptor dimensions";
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  D3D11_TEXTURE2D_DESC td;
  std::memset(&td, 0, sizeof(td));
  td.Width = static_cast<UINT>(a.rw);
  td.Height = static_cast<UINT>(a.rh);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = dd.ModeDesc.Format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  hr = device->CreateTexture2D(&td, nullptr, staging.put());
  if (FAILED(hr)) {
    err = "CreateTexture2D(staging) failed " + hr_hex(hr);
    m.exit_reason = err;
    m.exit_code = 3;
    m.fatal = true;
    std::fclose(csv);
    write_metadata(m);
    return 3;
  }

  int er = (a.rgb >> 16) & 0xFF;
  int eg = (a.rgb >> 8) & 0xFF;
  int eb = a.rgb & 0xFF;
  const PixelFormat fmt{m.id.bgra, m.id.format_name.c_str()};

  // -------------------------------------------------------------------------
  // Warm-up: first valid target frame establishes baseline (outside timing).
  // -------------------------------------------------------------------------
  m.warmup_start = qpc_now();
  long long warmup_deadline = m.warmup_start + (a.warmup_ms * m.freq) / 1000;
  long long prev_last_present = -1;
  MotionTracker motion;
  long long seq = 0;
  char errbuf[192];
  errbuf[0] = '\0';
  std::string fatal_reason;
  bool fatal_flag = false;

  auto scan_frame_into = [&](IDXGIResource* res, long long* map_start,
                             long long* map_end, ScanResult* out) -> bool {
    ComPtr<ID3D11Texture2D> src;
    HRESULT qhr = res->QueryInterface(
        IID_ID3D11Texture2D, reinterpret_cast<void**>(src.put()));
    if (FAILED(qhr)) {
      std::snprintf(errbuf, sizeof(errbuf), "resource_qi=%s", hr_hex(qhr).c_str());
      return false;
    }
    D3D11_BOX box;
    std::memset(&box, 0, sizeof(box));
    box.left = static_cast<UINT>(lx);
    box.top = static_cast<UINT>(ly);
    box.right = static_cast<UINT>(lx + a.rw);
    box.bottom = static_cast<UINT>(ly + a.rh);
    box.front = 0;
    box.back = 1;
    context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, src.get(), 0,
                                   &box);
    D3D11_MAPPED_SUBRESOURCE ms;
    std::memset(&ms, 0, sizeof(ms));
    *map_start = qpc_now();
    HRESULT mhr = context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(mhr)) {
      std::snprintf(errbuf, sizeof(errbuf), "map=%s", hr_hex(mhr).c_str());
      *map_end = qpc_now();
      return false;
    }
    *out = scan_strip(static_cast<const uint8_t*>(ms.pData), a.rw, a.rh,
                      static_cast<int>(ms.RowPitch), fmt, er, eg, eb,
                      a.tolerance, a.expected_width, a.width_tolerance, 2);
    context->Unmap(staging.get(), 0);
    *map_end = qpc_now();
    return true;
  };

  auto write_row = [&](const char* phase, long long seqv,
                       const DXGI_OUTDUPL_FRAME_INFO& info, long long qpc_before,
                       long long qpc_after, bool copied, bool cursor_ov,
                       long long ms_start, long long ms_end,
                       const ScanResult& sr, bool stale, bool pointer_only,
                       bool content_changed, bool signature_changed,
                       const char* err) {
    double present_sec =
        (info.LastPresentTime.QuadPart > 0)
            ? static_cast<double>(info.LastPresentTime.QuadPart) /
                  static_cast<double>(m.freq)
            : 0.0;
    std::fprintf(
        csv,
        "%lld,%s,%lld,%lld,%lld,%lld,%.9f,%lld,%u,%d,%d,%lld,%lld,"
        "%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,0x%016llX,%d,%d,%s\n",
        seqv, phase, qpc_after, qpc_before, qpc_after,
        static_cast<long long>(info.LastPresentTime.QuadPart), present_sec,
        static_cast<long long>(info.LastMouseUpdateTime.QuadPart),
        info.AccumulatedFrames, copied ? 1 : 0, cursor_ov ? 1 : 0, ms_start,
        ms_end, sr.rows_scanned, sr.rows_with_run, sr.max_runs_in_row, sr.left,
        sr.right, sr.width, sr.width_drift ? 1 : 0,
        sr.cls == TargetClass::Valid ? 1 : 0, sr.reason.c_str(),
        content_changed ? 1 : 0, signature_changed ? 1 : 0,
        static_cast<unsigned long long>(sr.signature), stale ? 1 : 0,
        pointer_only ? 1 : 0, err);
    std::fflush(csv);
  };

  // Warm-up loop. Every acquired frame is logged; the first valid target frame
  // establishes the baseline and lies outside the measured window.
  while (!m.baseline_set) {
    if (qpc_now() > warmup_deadline) {
      fatal_reason = "no valid target baseline within warmup window";
      fatal_flag = true;
      m.exit_code = 3;
      break;
    }
    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO info;
    std::memset(&info, 0, sizeof(info));
    long long qpc_before = qpc_now();
    hr = dupl->AcquireNextFrame(a.acquire_timeout_ms, &info, res.put());
    long long qpc_after = qpc_now();
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      ++m.counts.timeouts;
      continue;
    }
    if (FAILED(hr)) {
      fatal_reason = "AcquireNextFrame during warmup failed " + hr_hex(hr);
      fatal_flag = true;
      m.exit_code = 4;
      break;
    }
    FrameGuard fg(dupl.get());
    ++m.counts.acquires;
    ++m.counts.frames_written;
    ++seq;
    FrameClass fc = classify_frame(info.LastPresentTime.QuadPart,
                                   info.AccumulatedFrames);
    bool copied = false, cursor_ov = false, stale = false;
    long long ms_start = 0, ms_end = 0;
    ScanResult sr;
    errbuf[0] = '\0';
    if (fc.pointer_only) {
      ++m.counts.pointer_only;
      ++m.counts.skipped;
    } else {
      if (info.AccumulatedFrames > 1) ++m.counts.merged;
      cursor_ov = cursor_overlaps(a, m.id.output_rect.left,
                                  m.id.output_rect.top, &info);
      if (cursor_ov) {
        ++m.counts.cursor_overlap;
        ++m.counts.skipped;
      } else {
        bool ok = scan_frame_into(res.get(), &ms_start, &ms_end, &sr);
        if (ok) {
          copied = true;
          if (sr.cls == TargetClass::Missing) {
            ++m.counts.missing;
            ++m.counts.skipped;
          } else if (sr.cls == TargetClass::Ambiguous) {
            ++m.counts.ambiguous;
            ++m.counts.skipped;
          }
        } else {
          ++m.counts.errors;
          ++m.counts.skipped;
        }
      }
    }
    fg.Release();
    write_row("warmup", seq, info, qpc_before, qpc_after, copied, cursor_ov,
              ms_start, ms_end, sr, stale, fc.pointer_only, false, false,
              errbuf);
    if (copied && sr.cls == TargetClass::Valid) {
      m.baseline_set = true;
      m.base_left = sr.left;
      m.base_right = sr.right;
      m.base_width = sr.width;
      m.base_sig = sr.signature;
      m.base_qpc = qpc_now();
      motion.seed(sr.left, sr.right, sr.signature);
      m.qpc_ready = qpc_now();
      if (!write_ready_file(m)) {
        fatal_reason = "failed to write ready file";
        fatal_flag = true;
        m.exit_code = 4;
      }
      break;
    }
  }

  // -------------------------------------------------------------------------
  // Measurement loop (only if baseline was established).
  // -------------------------------------------------------------------------
  long long measure_start = 0;
  std::string stop_reason;
  if (m.baseline_set) {
    measure_start = qpc_now();
  }
  long long measure_deadline =
      measure_start + (a.max_duration_ms * m.freq) / 1000;

  while (m.baseline_set && !fatal_flag) {
    if (qpc_now() > measure_deadline) {
      stop_reason = "max_duration";
      break;
    }
    if (m.counts.frames_written >= a.max_frames) {
      stop_reason = "max_frames";
      break;
    }
    if (m.args.stop_file.empty() == false) {
      if (GetFileAttributesW(m.args.stop_file.c_str()) !=
          INVALID_FILE_ATTRIBUTES) {
        stop_reason = "stop_file";
        break;
      }
    }

    ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO info;
    std::memset(&info, 0, sizeof(info));
    long long qpc_before = qpc_now();
    hr = dupl->AcquireNextFrame(a.acquire_timeout_ms, &info, res.put());
    long long qpc_after = qpc_now();
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      ++m.counts.timeouts;
      continue;
    }
    if (FAILED(hr)) {
      fatal_reason = "AcquireNextFrame failed " + hr_hex(hr);
      fatal_flag = true;
      m.exit_code = 4;
      break;
    }
    FrameGuard fg(dupl.get());
    ++m.counts.acquires;
    ++m.counts.frames_written;
    ++seq;

    FrameClass fc = classify_frame(info.LastPresentTime.QuadPart,
                                   info.AccumulatedFrames);
    bool merged = info.AccumulatedFrames > 1;
    if (merged) ++m.counts.merged;

    bool copied = false;
    bool cursor_ov = false;
    long long ms_start = 0, ms_end = 0;
    ScanResult sr;
    bool stale = false;
    bool scanned = false;
    errbuf[0] = '\0';

    if (fc.pointer_only) {
      ++m.counts.pointer_only;
      ++m.counts.skipped;
    } else {
      // Timestamp validation: reject stale / nonmonotonic / future.
      long long lp = info.LastPresentTime.QuadPart;
      if (lp <= 0) {
        stale = true;
        errbuf[0] = '\0';
        std::snprintf(errbuf, sizeof(errbuf), "nonpositive_present");
      } else if (prev_last_present >= 0 && lp < prev_last_present) {
        stale = true;
        std::snprintf(errbuf, sizeof(errbuf), "nonmonotonic_present");
      } else if (lp > qpc_after) {
        stale = true;
        std::snprintf(errbuf, sizeof(errbuf), "future_present");
      } else if (qpc_after - lp > (a.stale_ms * m.freq) / 1000) {
        stale = true;
        std::snprintf(errbuf, sizeof(errbuf), "stale_present");
      }
      if (stale) {
        ++m.counts.stale;
        ++m.counts.skipped;
      } else {
        prev_last_present = lp;
      }

      cursor_ov = cursor_overlaps(a, m.id.output_rect.left,
                                  m.id.output_rect.top, &info);
      if (cursor_ov) {
        ++m.counts.cursor_overlap;
        ++m.counts.skipped;
      } else if (!stale) {
        bool ok = scan_frame_into(res.get(), &ms_start, &ms_end, &sr);
        if (ok) {
          copied = true;
          scanned = true;
          if (sr.cls == TargetClass::Valid) {
            ++m.counts.valid;
          } else if (sr.cls == TargetClass::Missing) {
            ++m.counts.missing;
            ++m.counts.skipped;
          } else {
            ++m.counts.ambiguous;
            ++m.counts.skipped;
          }
        } else {
          ++m.counts.errors;
          ++m.counts.skipped;
        }
      }
    }

    // Target motion is actual edge displacement from the previous valid frame;
    // a whole-ROI signature change is recorded separately and never counts.
    bool content_changed = false;
    bool signature_changed = false;
    if (scanned && sr.cls == TargetClass::Valid && !stale && !cursor_ov) {
      content_changed = motion.observe(sr, &signature_changed);
      if (content_changed) ++m.counts.content_changes;
      if (signature_changed) ++m.counts.signature_changes;
    }

    // Explicit release on every acquired frame before writing.
    fg.Release();

    write_row("measure", seq, info, qpc_before, qpc_after, copied, cursor_ov,
              ms_start, ms_end, sr, stale, fc.pointer_only, content_changed,
              signature_changed, errbuf);
    if (std::ftell(csv) > a.max_output_bytes) {
      stop_reason = "max_output_bytes";
      break;
    }
  }

  if (csv) {
    std::fflush(csv);
    std::fclose(csv);
  }
  m.qpc_end = qpc_now();
  FILETIME ct, et, kt, ut;
  std::memset(&ct, 0, sizeof(ct));
  std::memset(&et, 0, sizeof(et));
  std::memset(&kt, 0, sizeof(kt));
  std::memset(&ut, 0, sizeof(ut));
  if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
    auto ft_ms = [](const FILETIME& ft) -> long long {
      ULARGE_INTEGER u;
      u.LowPart = ft.dwLowDateTime;
      u.HighPart = ft.dwHighDateTime;
      return static_cast<long long>(u.QuadPart / 10000ULL);
    };
    m.cpu_kernel_ms = ft_ms(kt);
    m.cpu_user_ms = ft_ms(ut);
  }

  if (fatal_flag) {
    m.exit_reason = fatal_reason;
    m.exit_code = (m.exit_code == 0) ? 4 : m.exit_code;
    m.fatal = true;
  } else if (!m.baseline_set) {
    m.exit_reason = fatal_reason.empty() ? "no_baseline" : fatal_reason;
    m.exit_code = (m.exit_code == 0) ? 3 : m.exit_code;
    m.fatal = true;
  } else {
    m.exit_reason = stop_reason.empty() ? "completed" : stop_reason;
    m.exit_code = 0;
    m.fatal = false;
  }

  write_metadata(m);
  std::fprintf(stderr, "observer finished: reason=%s exit=%d\n",
               m.exit_reason.c_str(), m.exit_code);
  return m.exit_code;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  Args a;
  std::string err;
  if (!parse_args(argc, argv, a, err)) {
    std::fprintf(stderr, "argument error: %s\n", err.c_str());
    print_usage();
    return 2;
  }
  if (a.help) {
    print_usage();
    return 0;
  }
  if (a.self_test) {
    return run_self_test();
  }
  return run_real(a);
}
