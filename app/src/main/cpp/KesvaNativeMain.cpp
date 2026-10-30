#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>
#include <bx/platform.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>

#include <string>
#include <memory>
#include <mutex>

// ============ 与 Kotlin 严格一致的权限位 ============
enum PermBits {
    PERM_READ_EXTERNAL   = 1 << 0,
    PERM_WRITE_EXTERNAL  = 1 << 1,
    PERM_MANAGE_EXTERNAL = 1 << 2,
};

// ============ 全局原子状态 ============
static SDL_AtomicInt g_permMask     = {0};
static SDL_AtomicInt g_paused       = {0};
static SDL_AtomicInt g_running      = {1};
static SDL_AtomicInt g_logDirReady  = {0};
static std::string g_logDirPath;

// ============ spdlog Android Logcat Sink（修复悬垂指针） ============
template<typename Mutex>
class android_log_sink : public spdlog::sinks::base_sink<Mutex>
{
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);

        android_LogPriority prio = ANDROID_LOG_INFO;
        switch (msg.level) {
            case spdlog::level::trace:    prio = ANDROID_LOG_VERBOSE; break;
            case spdlog::level::debug:    prio = ANDROID_LOG_DEBUG;   break;
            case spdlog::level::info:     prio = ANDROID_LOG_INFO;    break;
            case spdlog::level::warn:     prio = ANDROID_LOG_WARN;    break;
            case spdlog::level::err:      prio = ANDROID_LOG_ERROR;   break;
            case spdlog::level::critical: prio = ANDROID_LOG_FATAL;   break;
            default: break;
        }
        // 直接使用缓冲区，不创建临时字符串，彻底杜绝悬垂指针
        __android_log_print(prio, "Kesva3D", "%.*s",
            static_cast<int>(formatted.size()), formatted.data());
    }

    void flush_() override {}
};

using android_log_sink_mt = android_log_sink<std::mutex>;

// ============ 初始化 spdlog 双写 ============
static bool InitSpdlog(const std::string& logDir)
{
    try {
        std::string logFilePath = logDir + "/kesva3d.log";

        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath, true);
        auto androidSink = std::make_shared<android_log_sink_mt>();

        std::vector<spdlog::sink_ptr> sinks { fileSink, androidSink };
        auto logger = std::make_shared<spdlog::logger>("kesva3d", sinks.begin(), sinks.end());

        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);

        return true;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "Kesva3D",
            "spdlog init failed: %s", e.what());
        return false;
    }
}

// ============ JNI 接口 ============
extern "C" JNIEXPORT void JNICALL
Java_org_errens_kesva3d_MainActivity_nativeSetLogDir(JNIEnv* env, jobject, jstring path)
{
    const char* str = env->GetStringUTFChars(path, nullptr);
    if (str) {
        g_logDirPath = str;
        env->ReleaseStringUTFChars(path, str);
        SDL_SetAtomicInt(&g_logDirReady, 1);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_errens_kesva3d_MainActivity_nativeSetPermissionState(JNIEnv* env, jobject, jint mask)
{
    SDL_SetAtomicInt(&g_permMask, mask);
    spdlog::info("Permission state updated: 0x{:x}", mask);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_errens_kesva3d_MainActivity_nativeOnPause(JNIEnv* env, jobject)
{
    SDL_SetAtomicInt(&g_paused, 1);
    spdlog::info("nativeOnPause: render paused");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_errens_kesva3d_MainActivity_nativeOnResume(JNIEnv* env, jobject)
{
    SDL_SetAtomicInt(&g_paused, 0);
    spdlog::info("nativeOnResume: render resumed");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

// ============ 等待日志路径 ============
static bool WaitLogDir(Uint32 timeoutMs = 2000)
{
    const Uint64 deadline = SDL_GetTicks() + timeoutMs;
    while (!SDL_GetAtomicInt(&g_logDirReady)) {
        if (SDL_GetTicks() > deadline) return false;
        SDL_Delay(10);
    }
    return true;
}

// ============ 权限等待 ============
static bool WaitPermissions(int required, Uint32 timeoutMs)
{
    const Uint64 deadline = SDL_GetTicks() + timeoutMs;
    while ((SDL_GetAtomicInt(&g_permMask) & required) != required) {
        if (SDL_GetTicks() > deadline || !SDL_GetAtomicInt(&g_running)) {
            return false;
        }
        SDL_Delay(10);
    }
    return true;
}

// ============ 获取 ANativeWindow ============
static ANativeWindow* GetNativeWindow(SDL_Window* window)
{
    for (int retry = 0; retry < 20; ++retry) {
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        void* nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
        if (nwh) {
            int w = ANativeWindow_getWidth(static_cast<ANativeWindow*>(nwh));
            int h = ANativeWindow_getHeight(static_cast<ANativeWindow*>(nwh));
            if (w > 0 && h > 0) {
                spdlog::info("Got valid ANativeWindow: {}x{}", w, h);
                return static_cast<ANativeWindow*>(nwh);
            }
        }
        SDL_Delay(10);
    }
    spdlog::error("Failed to get valid ANativeWindow");
    return nullptr;
}

// ============ bgfx 初始化：先强制 GLES 保底跑通，修正参数适配 Android ============
static bool InitBgfx(uint32_t w, uint32_t h, void* nwh)
{
    spdlog::info("Start bgfx init, resolution: {}x{}", w, h);

    // 每次初始化前都重新设置平台数据，避免前一次失败残留状态
    bgfx::PlatformData pd{};
    pd.ndt = nullptr;
    pd.nwh = nwh;
    bgfx::setPlatformData(pd);

    bgfx::Init init{};
    init.resolution.width  = w;
    init.resolution.height = h;
    init.resolution.reset  = BGFX_RESET_NONE; // 先去掉 VSYNC，排除驱动兼容问题
    init.resolution.format = bgfx::TextureFormat::RGBA8; // Android 原生窗口标准格式
    init.type = bgfx::RendererType::OpenGLES; // 先强制 GLES，保底跑通

    spdlog::info("Try bgfx renderer: OpenGLES");
    if (bgfx::init(init)) {
        spdlog::info("bgfx init success: OpenGLES");
        // 初始化成功后再开启 VSYNC
        bgfx::reset(w, h, BGFX_RESET_VSYNC);
        return true;
    }

    spdlog::error("bgfx OpenGLES init failed");
    return false;
}

// ============ SDL 标准入口 ============
extern "C" int SDL_main(int argc, char** argv)
{
    __android_log_print(ANDROID_LOG_INFO, "Kesva3D", "=== SDL_main entry ===");

    if (WaitLogDir()) {
        InitSpdlog(g_logDirPath);
        spdlog::info("=== Kesva3D engine start ===");
        spdlog::info("Log directory: {}", g_logDirPath);
    } else {
        __android_log_print(ANDROID_LOG_WARN, "Kesva3D",
            "Log dir wait timeout, use logcat only");
    }

    // 1. SDL 初始化
    spdlog::info("SDL_Init start");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        spdlog::error("SDL_Init failed: {}", SDL_GetError());
        return 1;
    }
    spdlog::info("SDL_Init success");

    // 2. 创建窗口
    spdlog::info("Create SDL window start");
    SDL_Window* window = SDL_CreateWindow(
        "Kesva3D", 1280, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        spdlog::error("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    spdlog::info("SDL_CreateWindow success");

    // 3. 获取窗口尺寸
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    spdlog::info("Window pixel size: {}x{}", w, h);

    // 4. 获取原生窗口
    ANativeWindow* nwh = GetNativeWindow(window);
    if (!nwh) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // 5. bgfx 初始化
    if (!InitBgfx((uint32_t)w, (uint32_t)h, nwh)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // 6. 视图配置
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, (uint16_t)w, (uint16_t)h);
    spdlog::info("View setup done, enter render loop");

    bool assetsStarted = false;

    // 主渲染循环
    while (SDL_GetAtomicInt(&g_running)) {
        // 事件处理
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    spdlog::info("Quit event received, exit loop");
                    SDL_SetAtomicInt(&g_running, 0);
                    break;

                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    SDL_GetWindowSizeInPixels(window, &w, &h);
                    spdlog::info("Window resized: {}x{}", w, h);
                    bgfx::reset((uint32_t)w, (uint32_t)h, BGFX_RESET_VSYNC);
                    bgfx::setViewRect(0, 0, 0, (uint16_t)w, (uint16_t)h);
                    break;

                default: break;
            }
        }

        // 暂停状态
        if (SDL_GetAtomicInt(&g_paused)) {
            SDL_Delay(16);
            continue;
        }

        // 资源加载触发
        if (!assetsStarted && SDL_GetAtomicInt(&g_permMask) != 0) {
            assetsStarted = true;
            if (WaitPermissions(PERM_READ_EXTERNAL | PERM_MANAGE_EXTERNAL, 3000)) {
                spdlog::info("Storage ready, load assets here");
            } else {
                spdlog::warn("Storage not granted, run without external assets");
            }
        }

        // 提交渲染
        bgfx::touch(0);
        bgfx::frame();
    }

    // 安全退出
    spdlog::info("Engine shutdown start");
    bgfx::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    spdlog::info("Engine shutdown complete");
    return 0;
}
