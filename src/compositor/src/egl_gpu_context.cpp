// Linux EGL/OpenGL backend for GpuContext (ADR 0017).
//
// Design constraints:
//  * No build-time dependency on EGL/GL headers or libraries: every entry
//    point is loaded with dlopen/dlsym (or eglGetProcAddress) at runtime, so
//    builds succeed on machines and CI runners without a graphics stack, and
//    the probe honestly reports "unavailable" there.
//  * Headless-capable: the display connection prefers the surfaceless platform,
//    then a GBM render node (no display server at all), then the default
//    display (X11/Wayland session).
//  * Exact pixels: composition runs through one fragment shader that performs
//    the same integer fixed-point math as Surface::BlendPixel, so the device
//    result is byte-identical to the software compositor's output (the tests
//    pin this down).
//  * Threading: the context is confined to the thread that created it and
//    stays current on that thread for its lifetime (every operation needs the
//    current context; no other thread may touch it).
//
// The compositor's presentable output remains the CPU Surface (ADR 0017); this
// context renders to offscreen FBOs and exposes Readback() for headless
// screenshots, parity tests and diagnostics.  Window-system presentation
// (swapchains) is a separate, unimplemented milestone.

#include "egl_gpu_context.h"

#include "neko/base/logging.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(__linux__)

#include <algorithm>
#include <cctype>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace neko::compositor {
namespace {

// ---------------------------------------------------------------------------
// Minimal EGL / OpenGL ES / GL definitions.
//
// Deliberately not including <EGL/egl.h> or <GL/gl.h>: the backend must build
// on machines without a graphics development stack (and CI images do not
// necessarily ship one).  Only the tokens actually used are defined.
// ---------------------------------------------------------------------------

using EglDisplay = void*;
using EglContext = void*;
using EglSurface = void*;
using EglConfig = void*;
using Eglint = std::int32_t;
using EglBoolean = unsigned int;
using EglEnum = unsigned int;
using EglProc = void (*)();

constexpr EglDisplay kEglNoDisplay = nullptr;
constexpr EglContext kEglNoContext = nullptr;
constexpr EglSurface kEglNoSurface = nullptr;

constexpr EglEnum kEglPBufferBit = 0x0001;
constexpr EglEnum kEglOpenGlBit = 0x0008;
constexpr EglEnum kEglNone = 0x3038;
constexpr EglEnum kEglAlphaSize = 0x3021;
constexpr EglEnum kEglSurfaceType = 0x3033;
constexpr EglEnum kEglRenderableType = 0x3040;
constexpr EglEnum kEglColorBufferType = 0x303F;
constexpr EglEnum kEglRgbBuffer = 0x308E;
constexpr EglEnum kEglWidth = 0x3057;
constexpr EglEnum kEglHeight = 0x3056;
constexpr EglEnum kEglExtensions = 0x3055;
constexpr EglEnum kEglOpenGlApi = 0x30A2;
constexpr EglEnum kEglContextMajorVersion = 0x3098;
constexpr EglEnum kEglContextMinorVersion = 0x30FB;
constexpr EglEnum kEglContextOpenGlProfileMask = 0x30FD;
constexpr EglEnum kEglContextOpenGlCoreProfileBit = 0x0001;
constexpr EglEnum kEglPlatformGbmKhr = 0x31D7;
constexpr EglEnum kEglPlatformSurfacelessMesa = 0x31DD;

using PfnEglGetProcAddress = EglProc (*)(const char*);
using PfnEglGetDisplay = EglDisplay (*)(void*);
using PfnEglGetPlatformDisplayExt = EglDisplay (*)(EglEnum, void*, const Eglint*);
using PfnEglInitialize = EglBoolean (*)(EglDisplay, Eglint*, Eglint*);
using PfnEglTerminate = EglBoolean (*)(EglDisplay);
using PfnEglQueryString = const char* (*)(EglDisplay, Eglint);
using PfnEglChooseConfig = EglBoolean (*)(EglDisplay, const Eglint*, EglConfig*, Eglint, Eglint*);
using PfnEglBindApi = EglBoolean (*)(EglEnum);
using PfnEglCreateContext = EglContext (*)(EglDisplay, EglConfig, EglContext, const Eglint*);
using PfnEglDestroyContext = EglBoolean (*)(EglDisplay, EglContext);
using PfnEglMakeCurrent = EglBoolean (*)(EglDisplay, EglSurface, EglSurface, EglContext);
using PfnEglCreatePbufferSurface = EglSurface (*)(EglDisplay, EglConfig, const Eglint*);
using PfnEglDestroySurface = EglBoolean (*)(EglDisplay, EglSurface);
using PfnEglGetError = Eglint (*)();

constexpr unsigned int kGlTexture2d = 0x0DE1;
constexpr unsigned int kGlUnsignedByte = 0x1401;
constexpr unsigned int kGlRgba = 0x1908;
constexpr unsigned int kGlRgba8 = 0x8058;
constexpr unsigned int kGlNearest = 0x2600;
constexpr unsigned int kGlClampToEdge = 0x812F;
constexpr unsigned int kGlTextureMagFilter = 0x2800;
constexpr unsigned int kGlTextureMinFilter = 0x2801;
constexpr unsigned int kGlTextureWrapS = 0x2802;
constexpr unsigned int kGlTextureWrapT = 0x2803;
constexpr unsigned int kGlFramebuffer = 0x8D40;
constexpr unsigned int kGlColorAttachment0 = 0x8CE0;
constexpr unsigned int kGlFramebufferComplete = 0x8CD5;
constexpr unsigned int kGlVertexShader = 0x8B31;
constexpr unsigned int kGlFragmentShader = 0x8B30;
constexpr unsigned int kGlCompileStatus = 0x8B81;
constexpr unsigned int kGlLinkStatus = 0x8B82;
constexpr unsigned int kGlInfoLogLength = 0x8B84;
constexpr unsigned int kGlTriangles = 0x0004;
constexpr unsigned int kGlVersion = 0x1F02;
constexpr unsigned int kGlRenderer = 0x1F01;
constexpr unsigned int kGlVendor = 0x1F00;
constexpr unsigned int kGlMaxTextureSize = 0x0D33;
constexpr unsigned int kGlDither = 0x0BD0;
constexpr unsigned int kGlColorBufferBit = 0x00004000;
constexpr unsigned int kGlUnpackRowLength = 0x0CF2;
constexpr unsigned int kGlUnpackAlignment = 0x0CF5;
constexpr unsigned int kGlPackAlignment = 0x0D05;
constexpr unsigned int kGlTexture0 = 0x84C0;
constexpr unsigned int kGlTexture1 = 0x84C1;

// ---------------------------------------------------------------------------
// Runtime loading helpers.
//
// dlsym returns void*; converting that to a function pointer is the documented
// POSIX mechanism (and what every GL loader does).  memcpy avoids function
// pointer casts that -Wpedantic would flag.
// ---------------------------------------------------------------------------

template <typename Fn> Fn FunctionFromDataPointer(void* symbol)
{
  Fn function = nullptr;
  static_assert(sizeof(Fn) == sizeof(symbol),
                "function and data pointers must have equal size on supported platforms");
  std::memcpy(&function, &symbol, sizeof(function));
  return function;
}

template <typename Fn> Fn FunctionFromProc(EglProc proc)
{
  Fn function = nullptr;
  static_assert(sizeof(Fn) == sizeof(proc),
                "function and data pointers must have equal size on supported platforms");
  std::memcpy(&function, &proc, sizeof(function));
  return function;
}

template <typename Fn> bool LoadSymbol(void* library, const char* name, Fn* out)
{
  void* symbol = library != nullptr ? dlsym(library, name) : nullptr;
  *out = FunctionFromDataPointer<Fn>(symbol);
  return *out != nullptr;
}

bool HasExtension(const char* extensions, std::string_view name)
{
  if (extensions == nullptr) {
    return false;
  }
  const std::string_view all(extensions);
  std::size_t pos = 0;
  while (pos <= all.size()) {
    const std::size_t next = all.find(' ', pos);
    const std::string_view token =
        next == std::string_view::npos ? all.substr(pos) : all.substr(pos, next - pos);
    if (token == name) {
      return true;
    }
    if (next == std::string_view::npos) {
      break;
    }
    pos = next + 1;
  }
  return false;
}

bool LooksLikeSoftwareRenderer(const std::string& renderer)
{
  static constexpr const char* kSoftwareNames[] = {
      "llvmpipe", "softpipe", "swrast", "lavapipe", "software rasterizer"};
  std::string lowered = renderer;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  for (const char* name : kSoftwareNames) {
    if (lowered.find(name) != std::string::npos) {
      return true;
    }
  }
  return false;
}

constexpr const char* kVertexShaderSource = R"glsl(#version 330 core
void main()
{
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

// The composition shader replicates Surface::BlendPixel byte-for-byte:
// straight-alpha "source over" with integer fixed-point math, including the
// (x*255 + 127)/255 rounding and truncating division semantics.  Pixels are
// recovered from the RGBA8 textures exactly (normalized fixed-point round trip
// is lossless for 8-bit values) and written back as k/255.
constexpr const char* kFragmentShaderSource = R"glsl(#version 330 core
uniform sampler2D uDst;
uniform sampler2D uSrc;
uniform ivec2 uOffset;
uniform ivec2 uLayerSize;
uniform float uOpacity;
uniform int uCopy;
out vec4 oColor;

uvec4 FetchU(sampler2D tex, ivec2 p)
{
  return uvec4(round(texelFetch(tex, p, 0) * 255.0));
}

void main()
{
  ivec2 p = ivec2(gl_FragCoord.xy);
  uvec4 d = FetchU(uDst, p);
  ivec2 q = p - uOffset;
  if (q.x < 0 || q.y < 0 || q.x >= uLayerSize.x || q.y >= uLayerSize.y) {
    oColor = vec4(d) / 255.0;
    return;
  }
  uvec4 s = FetchU(uSrc, q);
  if (uCopy != 0) {
    oColor = vec4(s) / 255.0;
    return;
  }
  uint opacity = uint(clamp(uOpacity, 0.0, 1.0) * 255.0);
  uint sa = (s.a * opacity) / 255u;
  if (sa == 0u) {
    oColor = vec4(d) / 255.0;
    return;
  }
  uint out_a = sa + ((d.a * (255u - sa) + 127u) / 255u);
  if (out_a == 0u) {
    oColor = vec4(d) / 255.0;
    return;
  }
  uint r = (s.r * sa + (d.r * d.a * (255u - sa) + 127u) / 255u) / out_a;
  uint g = (s.g * sa + (d.g * d.a * (255u - sa) + 127u) / 255u) / out_a;
  uint b = (s.b * sa + (d.b * d.a * (255u - sa) + 127u) / 255u) / out_a;
  oColor = vec4(r, g, b, out_a) / 255.0;
}
)glsl";

// The EGL entry points, loaded from libEGL at runtime.
struct EglApi
{
  PfnEglGetProcAddress get_proc_address = nullptr;
  PfnEglGetDisplay get_display = nullptr;
  PfnEglGetPlatformDisplayExt get_platform_display = nullptr;
  PfnEglInitialize initialize = nullptr;
  PfnEglTerminate terminate = nullptr;
  PfnEglQueryString query_string = nullptr;
  PfnEglChooseConfig choose_config = nullptr;
  PfnEglBindApi bind_api = nullptr;
  PfnEglCreateContext create_context = nullptr;
  PfnEglDestroyContext destroy_context = nullptr;
  PfnEglMakeCurrent make_current = nullptr;
  PfnEglCreatePbufferSurface create_pbuffer_surface = nullptr;
  PfnEglDestroySurface destroy_surface = nullptr;
  PfnEglGetError get_error = nullptr;
};

// The GL core entry points the composition pipeline needs.
struct GlApi
{
  const unsigned char* (*get_string)(unsigned int) = nullptr;
  unsigned int (*get_error)() = nullptr;
  void (*get_integer_v)(unsigned int, int*) = nullptr;
  void (*pixel_store_i)(unsigned int, int) = nullptr;
  void (*active_texture)(unsigned int) = nullptr;
  void (*gen_textures)(int, unsigned int*) = nullptr;
  void (*bind_texture)(unsigned int, unsigned int) = nullptr;
  void (*delete_textures)(int, const unsigned int*) = nullptr;
  void (*tex_parameter_i)(unsigned int, unsigned int, int) = nullptr;
  void (*tex_image_2d)(
      unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*) = nullptr;
  void (*tex_sub_image_2d)(
      unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*) = nullptr;
  void (*gen_framebuffers)(int, unsigned int*) = nullptr;
  void (*bind_framebuffer)(unsigned int, unsigned int) = nullptr;
  void (*framebuffer_texture_2d)(unsigned int, unsigned int, unsigned int, unsigned int, int) =
      nullptr;
  unsigned int (*check_framebuffer_status)(unsigned int) = nullptr;
  void (*delete_framebuffers)(int, const unsigned int*) = nullptr;
  void (*clear_color)(float, float, float, float) = nullptr;
  void (*clear)(unsigned int) = nullptr;
  void (*disable)(unsigned int) = nullptr;
  void (*viewport)(int, int, int, int) = nullptr;
  void (*read_pixels)(int, int, int, int, unsigned int, unsigned int, void*) = nullptr;
  void (*gen_vertex_arrays)(int, unsigned int*) = nullptr;
  void (*bind_vertex_array)(unsigned int) = nullptr;
  void (*delete_vertex_arrays)(int, const unsigned int*) = nullptr;
  unsigned int (*create_shader)(unsigned int) = nullptr;
  void (*shader_source)(unsigned int, int, const char* const*, const int*) = nullptr;
  void (*compile_shader)(unsigned int) = nullptr;
  void (*get_shader_iv)(unsigned int, unsigned int, int*) = nullptr;
  void (*get_shader_info_log)(unsigned int, int, int*, char*) = nullptr;
  void (*delete_shader)(unsigned int) = nullptr;
  unsigned int (*create_program)() = nullptr;
  void (*attach_shader)(unsigned int, unsigned int) = nullptr;
  void (*link_program)(unsigned int) = nullptr;
  void (*get_program_iv)(unsigned int, unsigned int, int*) = nullptr;
  void (*get_program_info_log)(unsigned int, int, int*, char*) = nullptr;
  void (*delete_program)(unsigned int) = nullptr;
  void (*use_program)(unsigned int) = nullptr;
  int (*get_uniform_location)(unsigned int, const char*) = nullptr;
  void (*uniform_1_i)(int, int) = nullptr;
  void (*uniform_1_f)(int, float) = nullptr;
  void (*uniform_2_i)(int, int, int) = nullptr;
  void (*draw_arrays)(unsigned int, int, int) = nullptr;

  bool Load(PfnEglGetProcAddress get_proc, void* lib_gl)
  {
    bool ok = true;
    ok = LoadOne(get_proc, lib_gl, "glGetString", &get_string) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGetError", &get_error) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGetIntegerv", &get_integer_v) && ok;
    ok = LoadOne(get_proc, lib_gl, "glPixelStorei", &pixel_store_i) && ok;
    ok = LoadOne(get_proc, lib_gl, "glActiveTexture", &active_texture) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGenTextures", &gen_textures) && ok;
    ok = LoadOne(get_proc, lib_gl, "glBindTexture", &bind_texture) && ok;
    ok = LoadOne(get_proc, lib_gl, "glDeleteTextures", &delete_textures) && ok;
    ok = LoadOne(get_proc, lib_gl, "glTexParameteri", &tex_parameter_i) && ok;
    ok = LoadOne(get_proc, lib_gl, "glTexImage2D", &tex_image_2d) && ok;
    ok = LoadOne(get_proc, lib_gl, "glTexSubImage2D", &tex_sub_image_2d) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGenFramebuffers", &gen_framebuffers) && ok;
    ok = LoadOne(get_proc, lib_gl, "glBindFramebuffer", &bind_framebuffer) && ok;
    ok = LoadOne(get_proc, lib_gl, "glFramebufferTexture2D", &framebuffer_texture_2d) && ok;
    ok = LoadOne(get_proc, lib_gl, "glCheckFramebufferStatus", &check_framebuffer_status) && ok;
    ok = LoadOne(get_proc, lib_gl, "glDeleteFramebuffers", &delete_framebuffers) && ok;
    ok = LoadOne(get_proc, lib_gl, "glClearColor", &clear_color) && ok;
    ok = LoadOne(get_proc, lib_gl, "glClear", &clear) && ok;
    ok = LoadOne(get_proc, lib_gl, "glDisable", &disable) && ok;
    ok = LoadOne(get_proc, lib_gl, "glViewport", &viewport) && ok;
    ok = LoadOne(get_proc, lib_gl, "glReadPixels", &read_pixels) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGenVertexArrays", &gen_vertex_arrays) && ok;
    ok = LoadOne(get_proc, lib_gl, "glBindVertexArray", &bind_vertex_array) && ok;
    ok = LoadOne(get_proc, lib_gl, "glDeleteVertexArrays", &delete_vertex_arrays) && ok;
    ok = LoadOne(get_proc, lib_gl, "glCreateShader", &create_shader) && ok;
    ok = LoadOne(get_proc, lib_gl, "glShaderSource", &shader_source) && ok;
    ok = LoadOne(get_proc, lib_gl, "glCompileShader", &compile_shader) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGetShaderiv", &get_shader_iv) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGetShaderInfoLog", &get_shader_info_log) && ok;
    ok = LoadOne(get_proc, lib_gl, "glDeleteShader", &delete_shader) && ok;
    ok = LoadOne(get_proc, lib_gl, "glCreateProgram", &create_program) && ok;
    ok = LoadOne(get_proc, lib_gl, "glAttachShader", &attach_shader) && ok;
    ok = LoadOne(get_proc, lib_gl, "glLinkProgram", &link_program) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGetProgramiv", &get_program_iv) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGetProgramInfoLog", &get_program_info_log) && ok;
    ok = LoadOne(get_proc, lib_gl, "glDeleteProgram", &delete_program) && ok;
    ok = LoadOne(get_proc, lib_gl, "glUseProgram", &use_program) && ok;
    ok = LoadOne(get_proc, lib_gl, "glGetUniformLocation", &get_uniform_location) && ok;
    ok = LoadOne(get_proc, lib_gl, "glUniform1i", &uniform_1_i) && ok;
    ok = LoadOne(get_proc, lib_gl, "glUniform1f", &uniform_1_f) && ok;
    ok = LoadOne(get_proc, lib_gl, "glUniform2i", &uniform_2_i) && ok;
    ok = LoadOne(get_proc, lib_gl, "glDrawArrays", &draw_arrays) && ok;
    return ok;
  }

private:
  template <typename Fn>
  static bool LoadOne(PfnEglGetProcAddress get_proc, void* lib_gl, const char* name, Fn* out)
  {
    Fn function = nullptr;
    if (get_proc != nullptr) {
      const EglProc proc = get_proc(name);
      if (proc != nullptr) {
        function = FunctionFromProc<Fn>(proc);
      }
    }
    if (function == nullptr && lib_gl != nullptr) {
      function = FunctionFromDataPointer<Fn>(dlsym(lib_gl, name));
    }
    *out = function;
    return function != nullptr;
  }
};

// ---------------------------------------------------------------------------
// The context.
// ---------------------------------------------------------------------------

class EglGpuContext final : public GpuContext
{
public:
  EglGpuContext() = default;
  ~EglGpuContext() override;

  EglGpuContext(const EglGpuContext&) = delete;
  EglGpuContext& operator=(const EglGpuContext&) = delete;

  // Performs the full bring-up (display, context, GL loading, pipeline).
  // Returns false and releases everything on failure.
  bool Initialize();

  GpuBackend backend() const override
  {
    return GpuBackend::OpenGL;
  }
  std::string renderer() const override
  {
    return renderer_;
  }
  std::string vendor() const override
  {
    return vendor_;
  }
  std::string version() const override
  {
    return version_;
  }
  bool hardware_accelerated() const override
  {
    return hardware_accelerated_;
  }
  int max_texture_size() const override
  {
    return max_texture_size_;
  }

  base::Status ResizeSurface(int width, int height) override;
  base::Status CreateTexture(int width, int height, std::uintptr_t* handle) override;
  base::Status UploadTexture(std::uintptr_t handle,
                             const std::uint8_t* pixels,
                             int stride,
                             int x,
                             int y,
                             int w,
                             int h) override;
  void DestroyTexture(std::uintptr_t handle) override;
  void BeginFrame(std::uint32_t rgba) override;
  void Draw(const GpuDrawCall& call) override;
  base::Status EndFrame() override;
  base::Status Readback(std::vector<std::uint8_t>* out_rgba8888) override;

private:
  struct Texture
  {
    unsigned int id = 0;
    int width = 0;
    int height = 0;
  };

  bool LoadEgl();
  bool ConnectDisplay();
  bool TryDisplay(EglDisplay display);
  bool TryGbmDisplay();
  bool CreateContextAndSurface();
  bool ChooseConfig(EglConfig* config) const;
  bool LoadGlApi();
  bool CreateCompositionPipeline();
  unsigned int CompileShader(unsigned int type, const char* source) const;
  bool CreateSurfaceTargets();
  void DestroySurfaceTargets();
  void DestroyGlResources();
  void Destroy();

  Texture* FindTexture(std::uintptr_t handle);
  void LogGlError(const char* where) const;

  EglApi egl_;
  GlApi gl_;
  EglDisplay display_ = kEglNoDisplay;
  EglContext context_ = kEglNoContext;
  EglSurface surface_ = kEglNoSurface;
  Eglint egl_version_major_ = 0;
  Eglint egl_version_minor_ = 0;

  void* lib_egl_ = nullptr;
  void* lib_gl_ = nullptr;
  void* lib_gbm_ = nullptr;
  void* gbm_device_ = nullptr;
  void (*gbm_device_destroy_)(void*) = nullptr;
  int gbm_fd_ = -1;

  std::string renderer_;
  std::string vendor_;
  std::string version_;
  int max_texture_size_ = 0;
  bool hardware_accelerated_ = false;

  int surface_width_ = 0;
  int surface_height_ = 0;
  bool surface_valid_ = false;
  unsigned int surface_textures_[2] = {0, 0};
  unsigned int surface_framebuffers_[2] = {0, 0};

  unsigned int program_ = 0;
  unsigned int vertex_array_ = 0;
  int u_dst_ = -1;
  int u_src_ = -1;
  int u_offset_ = -1;
  int u_layer_size_ = -1;
  int u_opacity_ = -1;
  int u_copy_ = -1;

  std::uint32_t clear_rgba_ = 0;
  std::vector<GpuDrawCall> pending_draws_;
  int presented_target_ = 0;
  bool frame_presented_ = false;

  std::unordered_map<std::uintptr_t, Texture> textures_;
  std::uintptr_t next_handle_ = 1;
};

EglGpuContext::~EglGpuContext()
{
  Destroy();
}

bool EglGpuContext::Initialize()
{
  if (!LoadEgl()) {
    Destroy();
    return false;
  }
  if (!ConnectDisplay()) {
    Destroy();
    return false;
  }
  if (!LoadGlApi()) {
    Destroy();
    return false;
  }
  if (!CreateCompositionPipeline()) {
    Destroy();
    return false;
  }

  // Dithering must be off: it would perturb the exact 8-bit blend results the
  // shader computes.
  if (gl_.disable != nullptr) {
    gl_.disable(kGlDither);
  }

  const unsigned char* renderer = gl_.get_string != nullptr ? gl_.get_string(kGlRenderer) : nullptr;
  const unsigned char* vendor = gl_.get_string != nullptr ? gl_.get_string(kGlVendor) : nullptr;
  const unsigned char* version = gl_.get_string != nullptr ? gl_.get_string(kGlVersion) : nullptr;
  renderer_ = renderer != nullptr ? reinterpret_cast<const char*>(renderer) : "";
  vendor_ = vendor != nullptr ? reinterpret_cast<const char*>(vendor) : "";
  version_ = version != nullptr ? reinterpret_cast<const char*>(version) : "";

  int max_texture_size = 0;
  if (gl_.get_integer_v != nullptr) {
    gl_.get_integer_v(kGlMaxTextureSize, &max_texture_size);
  }
  max_texture_size_ = std::max(0, max_texture_size);
  // A software rasterizer (llvmpipe et al.) is a correct but non-accelerated
  // device; report that honestly so callers can decide on their own.
  hardware_accelerated_ = !renderer_.empty() && !LooksLikeSoftwareRenderer(renderer_);
  return true;
}

bool EglGpuContext::LoadEgl()
{
  lib_egl_ = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (lib_egl_ == nullptr) {
    return false;
  }
  // Optional: some EGL implementations do not expose core GL functions
  // through eglGetProcAddress, in which case dlsym on libGL is the fallback.
  lib_gl_ = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);

  if (!LoadSymbol(lib_egl_, "eglGetProcAddress", &egl_.get_proc_address) ||
      !LoadSymbol(lib_egl_, "eglGetDisplay", &egl_.get_display) ||
      !LoadSymbol(lib_egl_, "eglInitialize", &egl_.initialize) ||
      !LoadSymbol(lib_egl_, "eglTerminate", &egl_.terminate) ||
      !LoadSymbol(lib_egl_, "eglQueryString", &egl_.query_string) ||
      !LoadSymbol(lib_egl_, "eglChooseConfig", &egl_.choose_config) ||
      !LoadSymbol(lib_egl_, "eglBindAPI", &egl_.bind_api) ||
      !LoadSymbol(lib_egl_, "eglCreateContext", &egl_.create_context) ||
      !LoadSymbol(lib_egl_, "eglDestroyContext", &egl_.destroy_context) ||
      !LoadSymbol(lib_egl_, "eglMakeCurrent", &egl_.make_current) ||
      !LoadSymbol(lib_egl_, "eglGetError", &egl_.get_error)) {
    return false;
  }
  // Optional entry points: loaded when present.
  (void)LoadSymbol(lib_egl_, "eglCreatePbufferSurface", &egl_.create_pbuffer_surface);
  (void)LoadSymbol(lib_egl_, "eglDestroySurface", &egl_.destroy_surface);

  if (egl_.get_proc_address != nullptr) {
    egl_.get_platform_display = FunctionFromProc<PfnEglGetPlatformDisplayExt>(
        egl_.get_proc_address("eglGetPlatformDisplayEXT"));
    if (egl_.get_platform_display == nullptr) {
      egl_.get_platform_display = FunctionFromDataPointer<PfnEglGetPlatformDisplayExt>(
          dlsym(lib_egl_, "eglGetPlatformDisplayEXT"));
    }
  }
  return true;
}

bool EglGpuContext::ConnectDisplay()
{
  const char* client_extensions =
      egl_.query_string != nullptr ? egl_.query_string(kEglNoDisplay, kEglExtensions) : nullptr;

  // 1) Surfaceless platform: no display connection at all (Mesa, NVIDIA).
  if (egl_.get_platform_display != nullptr &&
      HasExtension(client_extensions, "EGL_MESA_platform_surfaceless")) {
    EglDisplay display = egl_.get_platform_display(kEglPlatformSurfacelessMesa, nullptr, nullptr);
    if (display != kEglNoDisplay && TryDisplay(display)) {
      return true;
    }
  }

  // 2) GBM device from a DRM render node: fully headless, no session needed.
  if (egl_.get_platform_display != nullptr &&
      (HasExtension(client_extensions, "EGL_KHR_platform_gbm") ||
       HasExtension(client_extensions, "EGL_MESA_platform_gbm"))) {
    if (TryGbmDisplay()) {
      return true;
    }
  }

  // 3) Default display: an X11 or Wayland session (uses the environment).
  if (egl_.get_display != nullptr) {
    EglDisplay display = egl_.get_display(nullptr);
    if (display != kEglNoDisplay && TryDisplay(display)) {
      return true;
    }
  }

  return false;
}

bool EglGpuContext::TryDisplay(EglDisplay display)
{
  Eglint major = 0;
  Eglint minor = 0;
  if (egl_.initialize(display, &major, &minor) != static_cast<EglBoolean>(1)) {
    return false;
  }
  display_ = display;
  egl_version_major_ = major;
  egl_version_minor_ = minor;
  if (!CreateContextAndSurface()) {
    display_ = kEglNoDisplay;
    egl_.terminate(display);
    return false;
  }
  return true;
}

bool EglGpuContext::TryGbmDisplay()
{
  lib_gbm_ = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
  if (lib_gbm_ == nullptr) {
    return false;
  }
  void* (*gbm_create_device)(int) = nullptr;
  if (!LoadSymbol(lib_gbm_, "gbm_create_device", &gbm_create_device) ||
      !LoadSymbol(lib_gbm_, "gbm_device_destroy", &gbm_device_destroy_)) {
    return false;
  }

  // Prefer render nodes (no master privileges needed); fall back to card nodes.
  const char* candidates[] = {"/dev/dri/renderD128",
                              "/dev/dri/renderD129",
                              "/dev/dri/renderD130",
                              "/dev/dri/renderD131",
                              "/dev/dri/card0",
                              "/dev/dri/card1"};
  for (const char* path : candidates) {
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    void* device = gbm_create_device(fd);
    if (device == nullptr) {
      ::close(fd);
      continue;
    }
    EglDisplay display = egl_.get_platform_display(kEglPlatformGbmKhr, device, nullptr);
    if (display != kEglNoDisplay && TryDisplay(display)) {
      gbm_device_ = device;
      gbm_fd_ = fd;
      return true;
    }
    gbm_device_destroy_(device);
    ::close(fd);
  }
  return false;
}

bool EglGpuContext::ChooseConfig(EglConfig* config) const
{
  const Eglint base_attribs[] = {kEglSurfaceType,
                                 kEglPBufferBit,
                                 kEglRenderableType,
                                 kEglOpenGlBit,
                                 kEglColorBufferType,
                                 kEglRgbBuffer,
                                 kEglAlphaSize,
                                 8,
                                 kEglNone};
  const Eglint no_alpha_attribs[] = {kEglSurfaceType,
                                     kEglPBufferBit,
                                     kEglRenderableType,
                                     kEglOpenGlBit,
                                     kEglColorBufferType,
                                     kEglRgbBuffer,
                                     kEglNone};
  Eglint count = 0;
  if (egl_.choose_config(display_, base_attribs, config, 1, &count) == static_cast<EglBoolean>(1) &&
      count > 0) {
    return true;
  }
  count = 0;
  if (egl_.choose_config(display_, no_alpha_attribs, config, 1, &count) ==
          static_cast<EglBoolean>(1) &&
      count > 0) {
    return true;
  }
  return false;
}

bool EglGpuContext::CreateContextAndSurface()
{
  // Desktop GL 3.3 core requires EGL_KHR_create_context (or EGL 1.5, where it
  // is core); a 1.4 driver without the extension cannot give us the context we
  // need and must not be mistaken for a working backend.
  const char* display_extensions = egl_.query_string(display_, kEglExtensions);
  const bool has_create_context = egl_version_major_ > 1 || egl_version_minor_ >= 5 ||
                                  HasExtension(display_extensions, "EGL_KHR_create_context");
  if (!has_create_context) {
    return false;
  }

  EglConfig config = nullptr;
  if (!ChooseConfig(&config)) {
    return false;
  }
  if (egl_.bind_api(kEglOpenGlApi) != static_cast<EglBoolean>(1)) {
    return false;
  }

  const Eglint context_attribs[] = {kEglContextMajorVersion,
                                    3,
                                    kEglContextMinorVersion,
                                    3,
                                    kEglContextOpenGlProfileMask,
                                    kEglContextOpenGlCoreProfileBit,
                                    kEglNone};
  EglContext context = egl_.create_context(display_, config, kEglNoContext, context_attribs);
  if (context == kEglNoContext) {
    return false;
  }

  // Prefer a minimal pbuffer so the context is current on every implementation;
  // fall back to a surfaceless context when pbuffers are unavailable.
  EglSurface surface = kEglNoSurface;
  if (egl_.create_pbuffer_surface != nullptr) {
    const Eglint pbuffer_attribs[] = {kEglWidth, 1, kEglHeight, 1, kEglNone};
    surface = egl_.create_pbuffer_surface(display_, config, pbuffer_attribs);
  }
  if (surface != kEglNoSurface) {
    if (egl_.make_current(display_, surface, surface, context) == static_cast<EglBoolean>(1)) {
      context_ = context;
      surface_ = surface;
      return true;
    }
    if (egl_.destroy_surface != nullptr) {
      (void)egl_.destroy_surface(display_, surface);
    }
    egl_.destroy_context(display_, context);
    return false;
  }

  const char* client_extensions =
      egl_.query_string != nullptr ? egl_.query_string(kEglNoDisplay, kEglExtensions) : nullptr;
  if (HasExtension(client_extensions, "EGL_KHR_surfaceless_context") &&
      egl_.make_current(display_, kEglNoSurface, kEglNoSurface, context) ==
          static_cast<EglBoolean>(1)) {
    context_ = context;
    surface_ = kEglNoSurface;
    return true;
  }
  egl_.destroy_context(display_, context);
  return false;
}

bool EglGpuContext::LoadGlApi()
{
  return gl_.Load(egl_.get_proc_address, lib_gl_);
}

unsigned int EglGpuContext::CompileShader(unsigned int type, const char* source) const
{
  const unsigned int shader = gl_.create_shader(type);
  if (shader == 0) {
    return 0;
  }
  const int length = static_cast<int>(std::strlen(source));
  const char* sources[1] = {source};
  gl_.shader_source(shader, 1, sources, &length);
  gl_.compile_shader(shader);

  int status = 0;
  gl_.get_shader_iv(shader, kGlCompileStatus, &status);
  if (status == 0) {
    int log_length = 0;
    gl_.get_shader_iv(shader, kGlInfoLogLength, &log_length);
    std::vector<char> log(static_cast<std::size_t>(std::max(1, log_length)) + 1, '\0');
    gl_.get_shader_info_log(shader, static_cast<int>(log.size()), nullptr, log.data());
    NEKO_LOG_WARNING(std::string("GPU shader compilation failed: ") + log.data());
    gl_.delete_shader(shader);
    return 0;
  }
  return shader;
}

bool EglGpuContext::CreateCompositionPipeline()
{
  const unsigned int vertex_shader = CompileShader(kGlVertexShader, kVertexShaderSource);
  if (vertex_shader == 0) {
    return false;
  }
  const unsigned int fragment_shader = CompileShader(kGlFragmentShader, kFragmentShaderSource);
  if (fragment_shader == 0) {
    gl_.delete_shader(vertex_shader);
    return false;
  }

  const unsigned int program = gl_.create_program();
  if (program == 0) {
    gl_.delete_shader(vertex_shader);
    gl_.delete_shader(fragment_shader);
    return false;
  }
  gl_.attach_shader(program, vertex_shader);
  gl_.attach_shader(program, fragment_shader);
  gl_.link_program(program);
  gl_.delete_shader(vertex_shader);
  gl_.delete_shader(fragment_shader);

  int status = 0;
  gl_.get_program_iv(program, kGlLinkStatus, &status);
  if (status == 0) {
    int log_length = 0;
    gl_.get_program_iv(program, kGlInfoLogLength, &log_length);
    std::vector<char> log(static_cast<std::size_t>(std::max(1, log_length)) + 1, '\0');
    gl_.get_program_info_log(program, static_cast<int>(log.size()), nullptr, log.data());
    NEKO_LOG_WARNING(std::string("GPU shader link failed: ") + log.data());
    gl_.delete_program(program);
    return false;
  }

  program_ = program;
  u_dst_ = gl_.get_uniform_location(program_, "uDst");
  u_src_ = gl_.get_uniform_location(program_, "uSrc");
  u_offset_ = gl_.get_uniform_location(program_, "uOffset");
  u_layer_size_ = gl_.get_uniform_location(program_, "uLayerSize");
  u_opacity_ = gl_.get_uniform_location(program_, "uOpacity");
  u_copy_ = gl_.get_uniform_location(program_, "uCopy");
  if (u_dst_ < 0 || u_src_ < 0 || u_offset_ < 0 || u_layer_size_ < 0 || u_opacity_ < 0 ||
      u_copy_ < 0) {
    NEKO_LOG_WARNING("GPU composition shader is missing an expected uniform");
    return false;
  }

  // The vertex shader synthesizes the full-screen triangle from gl_VertexID;
  // core profile still requires a vertex array object to be bound.
  gl_.gen_vertex_arrays(1, &vertex_array_);
  return vertex_array_ != 0;
}

bool EglGpuContext::CreateSurfaceTargets()
{
  DestroySurfaceTargets();
  if (surface_width_ <= 0 || surface_height_ <= 0) {
    return true;
  }
  if (max_texture_size_ > 0 &&
      (surface_width_ > max_texture_size_ || surface_height_ > max_texture_size_)) {
    return false;
  }
  gl_.gen_textures(2, surface_textures_);
  gl_.gen_framebuffers(2, surface_framebuffers_);
  for (int i = 0; i < 2; ++i) {
    if (surface_textures_[i] == 0 || surface_framebuffers_[i] == 0) {
      DestroySurfaceTargets();
      return false;
    }
    gl_.bind_texture(kGlTexture2d, surface_textures_[i]);
    gl_.tex_parameter_i(kGlTexture2d, kGlTextureMinFilter, static_cast<int>(kGlNearest));
    gl_.tex_parameter_i(kGlTexture2d, kGlTextureMagFilter, static_cast<int>(kGlNearest));
    gl_.tex_parameter_i(kGlTexture2d, kGlTextureWrapS, static_cast<int>(kGlClampToEdge));
    gl_.tex_parameter_i(kGlTexture2d, kGlTextureWrapT, static_cast<int>(kGlClampToEdge));
    gl_.tex_image_2d(kGlTexture2d,
                     0,
                     static_cast<int>(kGlRgba8),
                     surface_width_,
                     surface_height_,
                     0,
                     kGlRgba,
                     kGlUnsignedByte,
                     nullptr);
    gl_.bind_framebuffer(kGlFramebuffer, surface_framebuffers_[i]);
    gl_.framebuffer_texture_2d(
        kGlFramebuffer, kGlColorAttachment0, kGlTexture2d, surface_textures_[i], 0);
    if (gl_.check_framebuffer_status(kGlFramebuffer) != kGlFramebufferComplete) {
      DestroySurfaceTargets();
      return false;
    }
  }
  surface_valid_ = true;
  return true;
}

void EglGpuContext::DestroySurfaceTargets()
{
  if (context_ != kEglNoContext) {
    if (surface_framebuffers_[0] != 0 || surface_framebuffers_[1] != 0) {
      gl_.delete_framebuffers(2, surface_framebuffers_);
    }
    if (surface_textures_[0] != 0 || surface_textures_[1] != 0) {
      gl_.delete_textures(2, surface_textures_);
    }
  }
  surface_framebuffers_[0] = 0;
  surface_framebuffers_[1] = 0;
  surface_textures_[0] = 0;
  surface_textures_[1] = 0;
  surface_valid_ = false;
}

void EglGpuContext::DestroyGlResources()
{
  if (context_ == kEglNoContext) {
    return;
  }
  DestroySurfaceTargets();
  for (auto& [handle, texture] : textures_) {
    (void)handle;
    if (texture.id != 0) {
      gl_.delete_textures(1, &texture.id);
    }
  }
  textures_.clear();
  if (program_ != 0) {
    gl_.delete_program(program_);
    program_ = 0;
  }
  if (vertex_array_ != 0) {
    gl_.delete_vertex_arrays(1, &vertex_array_);
    vertex_array_ = 0;
  }
}

void EglGpuContext::Destroy()
{
  // GL resources must be released while the context is still current; only
  // then is it safe to release the context itself.
  DestroyGlResources();
  if (context_ != kEglNoContext && display_ != kEglNoDisplay) {
    if (egl_.make_current != nullptr) {
      (void)egl_.make_current(display_, kEglNoSurface, kEglNoSurface, kEglNoContext);
    }
  }

  if (display_ != kEglNoDisplay) {
    if (surface_ != kEglNoSurface && egl_.destroy_surface != nullptr) {
      (void)egl_.destroy_surface(display_, surface_);
    }
    if (context_ != kEglNoContext) {
      (void)egl_.destroy_context(display_, context_);
    }
    (void)egl_.terminate(display_);
  }
  context_ = kEglNoContext;
  surface_ = kEglNoSurface;
  display_ = kEglNoDisplay;

  if (gbm_device_ != nullptr && gbm_device_destroy_ != nullptr) {
    gbm_device_destroy_(gbm_device_);
    gbm_device_ = nullptr;
  }
  if (gbm_fd_ >= 0) {
    ::close(gbm_fd_);
    gbm_fd_ = -1;
  }
  if (lib_gbm_ != nullptr) {
    dlclose(lib_gbm_);
    lib_gbm_ = nullptr;
  }
  if (lib_gl_ != nullptr) {
    dlclose(lib_gl_);
    lib_gl_ = nullptr;
  }
  if (lib_egl_ != nullptr) {
    dlclose(lib_egl_);
    lib_egl_ = nullptr;
  }
}

EglGpuContext::Texture* EglGpuContext::FindTexture(std::uintptr_t handle)
{
  const auto it = textures_.find(handle);
  return it == textures_.end() ? nullptr : &it->second;
}

void EglGpuContext::LogGlError(const char* where) const
{
  if (gl_.get_error == nullptr) {
    return;
  }
  const unsigned int error = gl_.get_error();
  if (error != 0) {
    NEKO_LOG_WARNING_F("GPU backend GL error 0x{:x} in {}", error, where);
  }
}

base::Status EglGpuContext::ResizeSurface(int width, int height)
{
  surface_width_ = std::max(0, width);
  surface_height_ = std::max(0, height);
  frame_presented_ = false;
  if (!CreateSurfaceTargets()) {
    return base::Err(base::Error::InvalidArgument(
        "GPU surface size is not supported by this device (texture limit exceeded)"));
  }
  return {};
}

base::Status EglGpuContext::CreateTexture(int width, int height, std::uintptr_t* handle)
{
  if (handle == nullptr) {
    return base::Err(base::Error::InvalidArgument("texture handle output must not be null"));
  }
  *handle = 0;
  if (width <= 0 || height <= 0) {
    return base::Err(base::Error::InvalidArgument("texture dimensions must be positive"));
  }
  if (max_texture_size_ > 0 && (width > max_texture_size_ || height > max_texture_size_)) {
    return base::Err(base::Error::InvalidArgument("texture exceeds max texture size"));
  }

  unsigned int id = 0;
  gl_.gen_textures(1, &id);
  if (id == 0) {
    return base::Err(base::Error::Unknown("GPU could not allocate a texture"));
  }
  gl_.bind_texture(kGlTexture2d, id);
  gl_.tex_parameter_i(kGlTexture2d, kGlTextureMinFilter, static_cast<int>(kGlNearest));
  gl_.tex_parameter_i(kGlTexture2d, kGlTextureMagFilter, static_cast<int>(kGlNearest));
  gl_.tex_parameter_i(kGlTexture2d, kGlTextureWrapS, static_cast<int>(kGlClampToEdge));
  gl_.tex_parameter_i(kGlTexture2d, kGlTextureWrapT, static_cast<int>(kGlClampToEdge));
  // Zero-initialize explicitly: glTexImage2D with a null data pointer leaves
  // texels undefined, and undefined layer pixels would make uploads of only
  // part of a texture (and the tests around them) nondeterministic.
  const std::vector<std::uint8_t> zeros(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
  gl_.tex_image_2d(kGlTexture2d,
                   0,
                   static_cast<int>(kGlRgba8),
                   width,
                   height,
                   0,
                   kGlRgba,
                   kGlUnsignedByte,
                   zeros.data());

  const std::uintptr_t handle_value = next_handle_++;
  if (handle_value == 0) {
    // Wrapped: restart the counter (practically unreachable).
    next_handle_ = 2;
  }
  textures_[handle_value] = Texture{id, width, height};
  *handle = handle_value;
  return {};
}

base::Status EglGpuContext::UploadTexture(
    std::uintptr_t handle, const std::uint8_t* pixels, int stride, int x, int y, int w, int h)
{
  Texture* texture = FindTexture(handle);
  if (texture == nullptr) {
    return base::Err(base::Error::InvalidArgument("unknown texture handle"));
  }
  if (pixels == nullptr) {
    return base::Err(base::Error::InvalidArgument("pixels must not be null"));
  }
  if (stride < 0 || stride % 4 != 0) {
    return base::Err(base::Error::InvalidArgument("stride must be a non-negative multiple of 4"));
  }

  // Same clipping rules as the recording context: partial overlap is legal,
  // fully outside is a no-op.
  const int src_x0 = std::max(0, -x);
  const int src_y0 = std::max(0, -y);
  const int dst_x0 = std::max(0, x);
  const int dst_y0 = std::max(0, y);
  const int copy_w = std::min(w - src_x0, texture->width - dst_x0);
  const int copy_h = std::min(h - src_y0, texture->height - dst_y0);
  if (copy_w <= 0 || copy_h <= 0) {
    return {};
  }

  gl_.bind_texture(kGlTexture2d, texture->id);
  gl_.pixel_store_i(kGlUnpackRowLength, stride / 4);
  gl_.pixel_store_i(kGlUnpackAlignment, 1);
  const std::uint8_t* source = pixels +
                               static_cast<std::size_t>(src_y0) * static_cast<std::size_t>(stride) +
                               static_cast<std::size_t>(src_x0) * 4;
  gl_.tex_sub_image_2d(
      kGlTexture2d, 0, dst_x0, dst_y0, copy_w, copy_h, kGlRgba, kGlUnsignedByte, source);
  gl_.pixel_store_i(kGlUnpackRowLength, 0);
  gl_.pixel_store_i(kGlUnpackAlignment, 4);
  return {};
}

void EglGpuContext::DestroyTexture(std::uintptr_t handle)
{
  const auto it = textures_.find(handle);
  if (it == textures_.end()) {
    return;
  }
  if (it->second.id != 0) {
    gl_.delete_textures(1, &it->second.id);
  }
  textures_.erase(it);
}

void EglGpuContext::BeginFrame(std::uint32_t rgba)
{
  clear_rgba_ = rgba;
  pending_draws_.clear();
  frame_presented_ = false;
}

void EglGpuContext::Draw(const GpuDrawCall& call)
{
  pending_draws_.push_back(call);
}

base::Status EglGpuContext::EndFrame()
{
  if (!surface_valid_) {
    return base::Err(base::Error::Unknown("GPU surface is not sized; call ResizeSurface first"));
  }

  // Seed the frame with the background color.  The clear conversion round
  // trips exactly (k/255 -> k), matching Surface::Clear.
  gl_.bind_framebuffer(kGlFramebuffer, surface_framebuffers_[0]);
  gl_.clear_color(static_cast<float>((clear_rgba_ >> 24) & 0xFFu) / 255.0f,
                  static_cast<float>((clear_rgba_ >> 16) & 0xFFu) / 255.0f,
                  static_cast<float>((clear_rgba_ >> 8) & 0xFFu) / 255.0f,
                  static_cast<float>(clear_rgba_ & 0xFFu) / 255.0f);
  gl_.clear(kGlColorBufferBit);

  int target = 0;
  for (const GpuDrawCall& call : pending_draws_) {
    const Texture* texture = FindTexture(call.texture);
    if (texture == nullptr || texture->id == 0 || texture->width <= 0 || texture->height <= 0) {
      continue;
    }
    const int next_target = 1 - target;
    gl_.bind_framebuffer(kGlFramebuffer, surface_framebuffers_[next_target]);
    gl_.use_program(program_);
    // GL_TEXTURE0 carries uDst; GL_TEXTURE1 carries uSrc.
    gl_.active_texture(kGlTexture0);
    gl_.bind_texture(kGlTexture2d, surface_textures_[target]);
    gl_.active_texture(kGlTexture1);
    gl_.bind_texture(kGlTexture2d, texture->id);
    gl_.uniform_1_i(u_dst_, 0);
    gl_.uniform_1_i(u_src_, 1);
    gl_.uniform_2_i(u_offset_, static_cast<int>(call.x), static_cast<int>(call.y));
    gl_.uniform_2_i(u_layer_size_, texture->width, texture->height);
    gl_.uniform_1_f(u_opacity_, call.opacity);
    gl_.uniform_1_i(u_copy_, call.blend == GpuBlendMode::kCopy ? 1 : 0);
    gl_.viewport(0, 0, surface_width_, surface_height_);
    gl_.bind_vertex_array(vertex_array_);
    gl_.draw_arrays(kGlTriangles, 0, 3);
    target = next_target;
  }
  LogGlError("EglGpuContext::EndFrame");
  presented_target_ = target;
  frame_presented_ = true;
  return {};
}

base::Status EglGpuContext::Readback(std::vector<std::uint8_t>* out_rgba8888)
{
  if (out_rgba8888 == nullptr) {
    return base::Err(base::Error::InvalidArgument("output buffer must not be null"));
  }
  if (!frame_presented_ || !surface_valid_) {
    return base::Err(base::Error::InvalidArgument("no frame has been presented"));
  }
  out_rgba8888->assign(
      static_cast<std::size_t>(surface_width_) * static_cast<std::size_t>(surface_height_) * 4, 0);
  gl_.bind_framebuffer(kGlFramebuffer, surface_framebuffers_[presented_target_]);
  gl_.pixel_store_i(kGlPackAlignment, 1);
  gl_.read_pixels(
      0, 0, surface_width_, surface_height_, kGlRgba, kGlUnsignedByte, out_rgba8888->data());
  gl_.pixel_store_i(kGlPackAlignment, 4);
  return {};
}

} // namespace

std::unique_ptr<GpuContext> CreateEglGpuContext()
{
  auto context = std::make_unique<EglGpuContext>();
  if (!context->Initialize()) {
    return nullptr;
  }
  return context;
}

} // namespace neko::compositor

#else // !defined(__linux__)

namespace neko::compositor {

std::unique_ptr<GpuContext> CreateEglGpuContext()
{
  return nullptr;
}

} // namespace neko::compositor

#endif // defined(__linux__)

namespace neko::compositor {

std::unique_ptr<GpuContext> CreateBestGpuContext()
{
#if defined(__linux__)
  return CreateEglGpuContext();
#else
  // Vulkan / Metal / Direct3D backends are not implemented; reporting no
  // device here is the honest answer that keeps the software fallback.
  return nullptr;
#endif
}

} // namespace neko::compositor
