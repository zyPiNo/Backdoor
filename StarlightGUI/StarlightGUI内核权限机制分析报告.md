# StarlightGUI 内核级权限实现机制 — 深度架构分析

> **项目版本**: v4.0.0 (Free)  |  **驱动版本**: Sirius 5.1.1  |  **许可证**: GPL 3.0  
> **作者**: Stars  |  **技术栈**: C++/WinRT + WinUI3 + 自定义内核驱动 Sirius.sys

---

## 一、总体架构概览

StarlightGUI 是一个 **反Rootkit（ARK）工具**，其内核级权限通过 **"用户态 GUI + 内核驱动"** 双层架构实现。核心设计思想是让一个合法的内核驱动（Sirius.sys）充当"代理"，GUI 层通过 IOCTL 向驱动发送命令，驱动在内核态（Ring 0）执行原本不可能从用户态完成的操作。

```
┌─────────────────────────────────────────────────────────────────────┐
│                      用户态 (Ring 3)                                 │
│  ┌──────────────┐   ┌─────────────────┐   ┌──────────────────────┐ │
│  │  WinUI3 GUI   │──▶│  KernelInstance  │──▶│    Elevator.h        │ │
│  │  (XAML Pages) │   │  (IOCTL 封装层)  │   │  (TrustedInstaller   │ │
│  └──────────────┘   └────────┬────────┘   │    提权)              │ │
│                              │             └──────────────────────┘ │
│                              │ DeviceIoControl()                    │
│                              ▼                                      │
│                     ┌─────────────────┐                             │
│                     │  DriverUtils     │                             │
│                     │  (SCM 驱动管理)   │                             │
│                     └────────┬────────┘                             │
│                              │ OpenSCManager / CreateService         │
├──────────────────────────────┼──────────────────────────────────────┤
│                      内核态 (Ring 0)                                 │
│                              ▼                                      │
│                     ┌─────────────────┐                             │
│                     │   Sirius.sys     │                             │
│                     │   (内核驱动)      │                             │
│                     │                 │                             │
│                     │  • 进程/线程操作  │                             │
│                     │  • 内存直接读写   │                             │
│                     │  • 内核回调管理   │                             │
│                     │  • SSDT/IDT/GDT  │                             │
│                     │  • DSE/PG 控制   │                             │
│                     │  • Hypervisor    │                             │
│                     └─────────────────┘                             │
└─────────────────────────────────────────────────────────────────────┘
```

**关键依赖**:
- Sirius.sys: 预编译内核驱动 (.sys)，源码不在本仓库
- Capstone: x86/x64 反汇编引擎
- nlohmann/json: JSON 解析（配置存储）
- NVML: NVIDIA GPU 监控

---

## 二、权限获取机制详解

### 2.1 内核驱动加载 — 核心入口

**文件**: `Utils/DriverUtils.cpp` → `DriverUtils::LoadKernelDriver()`

这是整个系统能够获得内核级能力的关键。通过 Windows SCM (Service Control Manager) 创建一个 `SERVICE_KERNEL_DRIVER` 类型的服务来加载驱动。

```
关键函数: DriverUtils::LoadKernelDriver(LPCWSTR kernelPath)

调用链:
  OpenSCManagerW(SC_MANAGER_ALL_ACCESS)
    └─ OpenServiceW("Sirius for StarlightGUI")
       ├─ [已存在] → QueryServiceStatus → StartServiceW (仅当STOPPED时)
       └─ [不存在] → CreateServiceW(
                        SERVICE_KERNEL_DRIVER,    // ★ 关键: 内核驱动类型
                        SERVICE_DEMAND_START,      // ★ 按需启动
                        kernelPath                 // ★ Sirius.sys 路径
                      )
                    → StartServiceW
```

**代码路径 (DriverUtils.cpp:6-62)**:
```cpp
hService = CreateServiceW(
    hSCM,
    L"Sirius for StarlightGUI",     // 服务名
    L"Sirius for StarlightGUI",     // 显示名
    SERVICE_ALL_ACCESS,
    SERVICE_KERNEL_DRIVER,           // ★ 内核驱动服务类型
    SERVICE_DEMAND_START,            // ★ 手动启动
    SERVICE_ERROR_IGNORE,
    kernelPath,                      // driver .sys 路径
    NULL, NULL, NULL, NULL, NULL
);
StartServiceW(hService, 0, nullptr);  // 加载到内核空间
```

**安全考量**:
- 加载驱动需要管理员权限（SeLoadDriverPrivilege）
- DSE (Driver Signature Enforcement) 会阻止未签名驱动加载
- 项目通过 `DisableDSE()` 和 Premium 版的 Hypervisor 绕过 DSE

### 2.2 TrustedInstaller 提权 — 用户态最高权限

**文件**: `Utils/Elevator.h` → `CreateProcessElevated()`

这是一个经典的 Windows 提权链，利用 SYSTEM 账户到 TrustedInstaller 的 Token 传递，使 GUI 进程获得 **Windows 最高等级的用户态权限**。

```
完整提权流程 (CreateProcessElevated):

第1阶段: 获取关键特权
  AdjustTokenPrivileges(SE_DEBUG_NAME)  ── 允许调试/打开任意进程
  AdjustTokenPrivileges(SE_TCB_NAME)    ── 允许充当操作系统的一部分

第2阶段: 窃取 SYSTEM Token
  FindProcessId("winlogon.exe")         ── 找到以 SYSTEM 运行的winlogon
  OpenProcessToken(winlogon, TOKEN_DUPLICATE)
  DuplicateTokenEx() × 2               ── 获得 Primary + Impersonation Token
  ImpersonateLoggedOnUser()             ── 线程模拟 SYSTEM
  SetThreadToken(NULL, hToken)          ── 强制设置线程 Token

第3阶段: 启动 TrustedInstaller 服务
  OpenSCManagerW(SC_MANAGER_ALL_ACCESS)  ── 以 SYSTEM 身份打开SCM
  OpenServiceW("TrustedInstaller")       ── 打开TI服务
  StartServiceW()                        ── 启动 TrustedInstaller 服务
  [降级路径] CreateProcessAsUserW()     ── 直接以SYSTEM创建TI进程

第4阶段: 复制 TrustedInstaller Token
  FindProcessId("TrustedInstaller.exe")  ── 等待TI进程出现
  OpenProcessToken(PROCESS_QUERY_INFO)   ── 打开TI进程Token
  DuplicateTokenEx(, TokenPrimary)       ── 复制为Primary Token

第5阶段: 增强Token (可选)
  EnableAllPrivileges()                  ── 启用Token中全部特权
    遍历 TokenPrivileges.PrivilegeCount
    每个 SE_PRIVILEGE_ENABLED = true

第6阶段: SessionID 修正 + 进程创建
  SetTokenInformation(TokenSessionId)    ── 对齐会话ID
  RevertToSelf()                         ── 恢复自身身份
  CreateProcessWithTokenW(hTIToken, LOGON_WITH_PROFILE)
  [降级路径] CreateProcessAsUserW()     ── 某些环境下WTS API不可用
```

**EnableAllPrivileges 实现 (Elevator.h:11-35)**:
```cpp
// 获取Token中所有特权
GetTokenInformation(hToken, TokenPrivileges, ...)
// 遍历并启用每一个
for (DWORD i = 0; i < pTokenPrivileges->PrivilegeCount; i++) {
    pTokenPrivileges->Privileges[i].Attributes |= SE_PRIVILEGE_ENABLED;
}
// 应用修改
AdjustTokenPrivileges(hToken, FALSE, pTokenPrivileges, ...)
```

### 2.3 启动流程 — App.xaml.cpp

**文件**: `App.xaml.cpp` → `App::OnLaunched()`

```
启动流程:
  OnLaunched()
    │
    ├─ HasSwitch("--trustedinstaller-relaunch")?
    │   ├─ [是] → 已经是TI重启动，继续正常启动
    │   └─ [否] → elevated_run == true?
    │              └─ CreateProcessElevated(自身, "--trustedinstaller-relaunch")
    │                 → Exit() // 当前实例退出，TI权限实例接管
    │
    ├─ InitializeDriverBeforeWindow()
    │   ├─ 确定 Sirius.sys 路径: <AppDir>\Assets\Sirius.sys
    │   ├─ DriverUtils::LoadKernelDriver(siriusPath)
    │   └─ [失败] → MessageBox 错误 + Exit()
    │
    └─ 创建 MainWindow → 正常启动
```

**关键设计**:
- GUI 可以选择以 TrustedInstaller 权限运行（`elevated_run` 配置项）
- 驱动在窗口创建**之前**加载，确保所有页面可用
- `--trustedinstaller-relaunch` 参数防止无限递归提权

---

## 三、系统调用方式与通信机制

### 3.1 IOCTL — 用户态↔内核态的主要桥梁

StarlightGUI **不使用** syscall hook 或漏洞利用。所有内核操作都是通过 **DeviceIoControl API** 向 Sirius.sys 发送标准 IOCTL 请求完成的。

**设备打开 (KernelInstance.cpp:1350-1361)**:
```cpp
HANDLE device = CreateFile(
    L"\\\\.\\Sirius",                       // 驱动设备符号链接
    GENERIC_READ | GENERIC_WRITE,           // 读写权限
    0, NULL, OPEN_EXISTING, 0, NULL
);
```

**IOCTL 四大操作域**:

| IOCTL 域 | 控制码 (CTL_CODE) | 功能分类 |
|---|---|---|
| **进程操作** | `IOCTL_SIRIUS_SET_PROCESS_INFORMATION` (0x000) | 终止/隐藏/挂起/注入/Token修改/PPL设置 |
| **进程查询** | `IOCTL_SIRIUS_QUERY_PROCESS_INFORMATION` (0x001) | 枚举进程/线程/模块/句柄/内核回调表 |
| **线程操作** | `IOCTL_SIRIUS_SET_THREAD_INFORMATION` (0x002) | 终止/挂起/恢复/PreviousMode/APC注入 |
| **线程查询** | `IOCTL_SIRIUS_QUERY_THREAD_INFORMATION` (0x003) | 线程基本信息/完整信息 |
| **系统操作** | `IOCTL_SIRIUS_SET_SYSTEM_INFORMATION` (0x100) | **内存读写/BSOD/加载驱动/移除回调/DSE/PG/LKD** |
| **系统查询** | `IOCTL_SIRIUS_QUERY_SYSTEM_INFORMATION` (0x101) | **SSDT/SSSDT/IDT/GDT/回调/MiniFilter/PiDDB/HAL表** |
| **文件操作** | `IOCTL_SIRIUS_SET_FILE_INFORMATION` (0x00E) | 强制删除/复制/重命名 |
| **文件查询** | `IOCTL_SIRIUS_QUERY_FILE_INFORMATION` (0x00F) | NTFS MFT遍历/原始数据读取 |
| **监控** | IOCTL_SET_MONITOR/GET_LOG 系列 (0x600+) | 进程/线程/镜像/注册表/文件行为监控 |
| **虚拟化** | IOCTL_METAVERSE_* 系列 (0x900+) | Hypervisor 初始化/退出/检查 (Premium) |
| **诊断** | IOCTL_SIRIUS_GET_ERROR_CODE/DETAIL (0x700+) | 错误码/详细信息查询 |

### 3.2 通信协议设计

所有 IOCTL 使用 `METHOD_BUFFERED` 缓冲模式，保证安全的数据交换。每个请求都有统一的结构体封装：

```cpp
// 典型请求结构 (以进程操作为例)
typedef struct _SI_PROCESS_INFORMATION {
    ULONG ProcessInformation;   // 操作类型枚举
    ULONG PID;                  // 目标进程ID
    PVOID Buffer;               // 输入/输出缓冲区
    ULONG Argument;             // 额外参数
} SI_PROCESS_INFORMATION;

// 调用方式
SI_PROCESS_INFORMATION request = { operation, pid, buffer, argument };
DeviceIoControl(driverDevice, IOCTL_SIRIUS_SET_PROCESS_INFORMATION,
    &request, sizeof(SI_PROCESS_INFORMATION),
    0, 0, 0, NULL);
```

### 3.3 两阶段枚举模式

所有枚举操作（进程列表、SSDT、回调等）都采用相同的"两阶段"模式，保证效率：

```
阶段1 — 查询数量:
  enumeration.Count = 0
  enumeration.Buffer = NULL
  DeviceIoControl(...)  → 驱动返回 item 数量到 Count

阶段2 — 获取数据:
  enumeration.BufferSize = Count * itemSize
  enumeration.Buffer = HeapAlloc(...)
  DeviceIoControl(...)  → 驱动写入实际数据

代码实现 (KernelInstance.cpp:47-76):
  QuerySystemEnumeration() → 封装此模式
```

### 3.4 用户态直接 NT API 调用

**文件**: `KernelInstance.cpp:1009-1089` + `Utils/NTBase.h`

除 IOCTL 外，代码还通过 `GetProcAddress` 从 `ntdll.dll` 动态加载 NT 原生 API，用于**对象目录浏览**（不受驱动影响的操作）:

```cpp
// 动态加载全部 NT API
HMODULE hModule = GetModuleHandleW(L"ntdll.dll");
NtOpenDirectoryObject  = GetProcAddress(hModule, "NtOpenDirectoryObject");
NtOpenSymbolicLinkObject = GetProcAddress(hModule, "NtOpenSymbolicLinkObject");
NtOpenEvent            = GetProcAddress(hModule, "NtOpenEvent");
// ... 共18个 NtOpen*/NtQuery* 函数

// 使用
HANDLE hDir;
NtOpenDirectoryObject(&hDir, DIRECTORY_QUERY, &objAttr);
NtQueryDirectoryObject(hDir, buffer, bufferSize, FALSE, FALSE, &context, &returnLength);
```

**支持的 NT 对象类型**: Directory, SymbolicLink, Event, Mutant, Semaphore, Section, Timer, Device, Session, CpuPartition, Job, IoCompletion, Partition

---

## 四、核心内核能力及其代码路径

### 4.1 进程操作

| 功能 | KernelInstance 方法 | IOCTL → 驱动操作 | 关键代码行 |
|---|---|---|---|
| **终止进程** | `SiTerminateProcess(pid)` | `ProcessSetInformation::Terminate, arg=0` | 140-143 |
| **强制终止** | `SiTerminateProcessEx(pid)` | `ProcessSetInformation::Terminate, arg=2` (内存方式) | 146-149 |
| **隐藏进程** | `SiHideProcess(pid)` | `ProcessSetInformation::Hide` (BSOD警告) | 164-168 |
| **挂起/恢复** | `SiSuspendProcess/SiResumeProcess` | `ProcessSetInformation::Suspend/Resume` | 152-162 |
| **DLL注入** | `InjectDLLToProcess(pid, dllPath)` | `ProcessSetInformation::InjectDll` (CreateRemoteThread/APC/ManualMap) | 184-191 |
| **Token替换** | `ModifyProcessToken(srcPID, tgtPID)` | `ProcessSetInformation::Token` | 193-197 |
| **设置PPL保护** | `SetPPL(pid, level)` | `ProcessSetInformation::Protection` | 170-175 |
| **标记关键进程** | `SetCriticalProcess(pid)` | `ProcessSetInformation::Critical` | 177-182 |

**DLL 注入方法 (SI_INJECT_DLL)**:
- Method=0: CreateRemoteThread (经典远程线程注入)
- Method=1: QueueUserAPC (APC注入)
- Method=2: Manual Map (手动映射 — 最隐蔽)

### 4.2 内存直接读写

**文件**: `KernelInstance.cpp:1289-1342`

```
读取路径 (ReadMemory):
  KernelInstance::ReadMemory(data, address, size)
    → 构造 SI_MEMORY { address, size, Data[1] }
    → SiQuerySystemInformation(SystemGetInformation::ReadMemory, ...)
      → DeviceIoControl(IOCTL_SIRIUS_QUERY_SYSTEM_INFORMATION)
        → 驱动使用 MmCopyVirtualMemory 或直接解引用内核地址

写入路径 (WriteMemory):
  KernelInstance::WriteMemory(address, data, size)
    → 构造 SI_MEMORY { address, size, Data[] }
    → SiSetSystemInformation(SystemSetInformation::WriteMemory, ...)
      → DeviceIoControl(IOCTL_SIRIUS_SET_SYSTEM_INFORMATION)
        → 驱动直接写入任意内核/用户态地址
```

GUI 集成 (DisasmPage.xaml.cpp):
- **读取模式**: 读取内存 → 可选Hex视图或Capstone反汇编
- **写入模式**: 写入指定地址 → 直接修改内存数据
- Capstone 引擎: `cs_open(CS_ARCH_X86, CS_MODE_64)` → `cs_disasm()` → 格式化输出

### 4.3 内核回调管理

**文件**: `KernelInstance.cpp:783-781` (枚举), `1256-1265` (移除)

```
系统支持 19 种回调类型 (CallbackType 枚举):
  CreateProcess, CreateThread, LoadImage, Object, Registry,
  PowerSetting, PlugPlay, Shutdown, LastChanceShutdown,
  FileSystemChange, BugCheck, BugCheckReason, ExCallback,
  LogonSessionTerminated, LogonSessionTerminatedEx,
  DbgPrint, IoPriority, Coalescing, ImageVerification, Nmi

枚举: SiEnumCallbacks(type)
  → SystemGetInformation::Callback, arg=CallbackType
  → 驱动读取 PspCreateProcessNotifyRoutine 等内部数组

移除: RemoveCallback(entry)
  → SI_REMOVE_CALLBACK { Type, Address, Address2 }
  → SystemSetInformation::RemoveCallback
  → 驱动将回调地址数组中对应项置零
```

### 4.4 系统安全机制操控

**文件**: `KernelInstance.cpp` (多种方法) + `UtilityPage.xaml.cpp` (UI触发)

| 操作 | KernelInstance 方法 | IOCTL 参数 | 说明 |
|---|---|---|---|
| **启用DSE** | `EnableDSE(false)` | `SystemSetInformation::DSEState, TRUE, arg=0` | 重新开启驱动签名强制 |
| **禁用DSE** | `DisableDSE(false)` | `SystemSetInformation::DSEState, FALSE, arg=0` | 允许加载未签名驱动 |
| **禁用DSE(虚拟化)** | `DisableDSE(true)` | `SystemSetInformation::DSEState, FALSE, arg=1` | ★ Premium: Hypervisor级DSE绕过 |
| **禁用PatchGuard** | `DisablePatchGuard(false)` | `SystemSetInformation::DisablePatchGuard, arg=0` | ★ Premium: 内核补丁保护 |
| **禁用PG(虚拟化)** | `DisablePatchGuard(true)` | `SystemSetInformation::DisablePatchGuard, arg=1` | ★ Premium: Hypervisor级PG绕过 |
| **启用LKD** | `EnableLKD()` | `SystemSetInformation::LKDState` | 动态启用本地内核调试 |
| **阻止进程创建** | `DisableCreateProcess()` | `SystemSetInformation::CreateProcessState, FALSE` | 全局阻止新进程 |
| **阻止文件创建** | `DisableCreateFile()` | `SystemSetInformation::CreateFileState, FALSE` | 全局阻止文件创建 |
| **触发BSOD** | `BlueScreen()` | `SystemSetInformation::TriggerBugCheck` | 手动触发蓝屏 |

### 4.5 内核枚举能力

```
系统级枚举 (SystemGetInformation):
  Process        — 枚举全部进程 (含隐藏进程)
  Thread         — 枚举全部线程
  Module         — 枚举全部内核模块/驱动
  Handle         — 枚举全部句柄
  IOTimer        — 枚举全部IO定时器
  DPCTimer       — 枚举全部DPC定时器
  Minifilter     — 枚举全部文件系统微过滤器
  ObjectType     — 枚举全部对象类型
  Resource       — 枚举全部ERESOURCE
  Job            — 枚举全部Job对象
  SSDT           — 系统服务描述符表(ntoskrnl.exe)
  ShadowSSDT     — Shadow SSDT (win32k.sys)
  FilterSSDT     — 过滤驱动SSDT
  IDT            — 中断描述符表 (Interrupt Descriptor Table)
  GDT            — 全局描述符表 (Global Descriptor Table)
  Callback       — 19种内核回调
  MADT_Entries   — ACPI MADT表项
  HalDispatchTable / HalPrivateDispatchTable
  HalIommuDispatchTable / HalAcpiDispatchTable
  HalSubComponents / SeCiCallbacks
  PiDDBCacheTable       — 驱动签名缓存表
  MmUnloadedDrivers     — 已卸载驱动记录
  Firmware               — ACPI/UEFI固件表信息
```

### 4.6 Hypervisor 虚拟化 (Premium)

**文件**: `SiriusIO.h:210-213`, `KernelInstance.cpp:861-892`

```
IOCTL_METAVERSE_CHECK_SUPPORT (0x902)
  → 检查CPU是否支持虚拟化 (VMX/SVM)

IOCTL_METAVERSE_INITIALIZE (0x900)
  → 初始化 Hypervisor (VT-x/AMD-V)
  → 注册表: HKLM\SOFTWARE\Sirius\AutoVirtualization=1 自动启动

IOCTL_METAVERSE_EXIT (0x901)
  → 退出虚拟化模式
  → 注释: "必须关闭，否则会自动关闭"

Premium独有:
  - DSE 虚拟化绕过 (不修改内核数据，通过 Hypervisor EPT 做内存隐藏)
  - PatchGuard 虚拟化绕过
  - 加载驱动时绕过 DSE
```

---

## 五、模块调用关系与数据流

```
┌──────────────────────────────────────────────────────────────────┐
│                        App.xaml.cpp                               │
│                        (启动入口)                                  │
│  OnLaunched()                                                     │
│    ├── CreateProcessElevated() ← Elevator.h (提权至TrustedInstaller)
│    ├── InitializeDriverBeforeWindow()                             │
│    │     └── DriverUtils::LoadKernelDriver() ← DriverUtils.cpp    │
│    │           └── SCM API → 内核加载 Sirius.sys                  │
│    └── MainWindow()                                               │
└──────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│                      MainWindow.xaml.cpp                           │
│                      (主窗口 + 导航)                                │
│  NavigationView → 各页面路由:                                      │
│    HomePage / TaskPage / KernelModulePage / FilePage              │
│    WindowPage / UtilityPage / MonitorPage / DisasmPage            │
│    SettingsPage / HelpPage                                        │
└──────────────────────────────────────────────────────────────────┘
                              │
         ┌────────────────────┼────────────────────┐
         ▼                    ▼                     ▼
┌──────────────┐    ┌──────────────┐     ┌──────────────┐
│  TaskPage    │    │ MonitorPage  │     │ UtilityPage  │
│  (进程管理)   │    │  (ARK监控)    │     │  (系统工具)   │
│              │    │              │     │              │
│ SiTerminate  │    │ SSDT/GDT/IDT │     │ Enable/Disable│
│ SiSuspend    │    │ Callbacks    │     │   DSE         │
│ InjectDLL    │    │ MiniFilter   │     │   PatchGuard  │
│ Token修改    │    │ PiDDB/HAL    │     │   LKD         │
│ SetPPL       │    │ Object Tree  │     │   Hypervisor  │
└──────┬───────┘    └──────┬───────┘     └──────┬────────┘
       │                   │                    │
       └───────────────────┼────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│                  KernelInstance (内核接口层)                        │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ 公共接口层                                                    │ │
│  │  SiTerminateProcess() / SiHideProcess() / InjectDLLToProcess()│ │
│  │  SiEnumProcesses() / SiEnumCallbacks() / SiEnumSSDT()        │ │
│  │  ReadMemory() / WriteMemory() / EnableDSE() / BlueScreen()   │ │
│  │  SiEnumObjectsByDirectory() ← 直接调用 ntdll.dll NT API      │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                           │                                       │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ 私有IOCTL层                                                  │ │
│  │  SiSetProcessInformation() / SiQueryProcessInformation()     │ │
│  │  SiSetSystemInformation() / SiQuerySystemInformation()      │ │
│  │  SiSetFileInformation() / SiQueryFileInformation()           │ │
│  │                                                              │ │
│  │  每个 = DeviceIoControl(driverDevice, IOCTL_xxx, ...)       │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                           │                                       │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ 设备获取层 GetDriverDevice()                                  │ │
│  │  1. 尝试 DriverUtils::LoadKernelDriver()                    │ │
│  │  2. CreateFile("\\\\.\\Sirius", ...)                        │ │
│  └─────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
                           │
                     DeviceIoControl()
                           ▼
┌──────────────────────────────────────────────────────────────────┐
│                    Sirius.sys (内核驱动)                           │
│                                                                   │
│  接收 IOCTL → 在内核上下文执行 → 返回结果                          │
│                                                                   │
│  可能使用的内核API:                                                │
│    MmCopyVirtualMemory()   — 跨进程内存读写                        │
│    PsLookupProcessByProcessId() — EPROCESS获取                    │
│    KeStackAttachProcess()  — 附加到目标进程                        │
│    ZwQuerySystemInformation() — 系统信息查询                       │
│    ExAcquireResourceSharedLite() — 同步锁                          │
│    PsSetCreateProcessNotifyRoutine() — 回调操作                    │
│    CmRegisterCallback()     — 注册表回调                            │
│    IoCreateDevice()         — 创建设备对象                         │
│    __readmsr() / __writemsr() — MSR操作 (Hypervisor)              │
│    __vmx_vmcall / __vmx_on   — VMX操作 (Hypervisor)               │
│    RtlInitUnicodeString()   — 字符串处理                           │
│    ExAllocatePoolWithTag()  — 内存分配                             │
└──────────────────────────────────────────────────────────────────┘
```

---

## 六、关键函数速查表

| 函数 | 文件:行 | 作用 | 权限级别 |
|---|---|---|---|
| `CreateProcessElevated()` | Elevator.h:37 | TrustedInstaller 提权链 | 用户态最高权限 |
| `EnableAllPrivileges()` | Elevator.h:11 | 启用Token全部特权 | 用户态 |
| `DriverUtils::LoadKernelDriver()` | DriverUtils.cpp:6 | SCM加载内核驱动 | 内核入口 |
| `KernelInstance::GetDriverDevice()` | KernelInstance.cpp:1351 | 获取设备句柄+自动加载驱动 | 桥接层 |
| `KernelInstance::ReadMemory()` | KernelInstance.cpp:1289 | 读取任意内核/用户态内存 | 内核 |
| `KernelInstance::WriteMemory()` | KernelInstance.cpp:1318 | 写入任意内核/用户态内存 | 内核 |
| `KernelInstance::SiHideProcess()` | KernelInstance.cpp:164 | 断链隐藏进程 | 内核 |
| `KernelInstance::InjectDLLToProcess()` | KernelInstance.cpp:184 | DLL注入 (3种方法) | 内核 |
| `KernelInstance::ModifyProcessToken()` | KernelInstance.cpp:193 | Token替换提权 | 内核 |
| `KernelInstance::SetPPL()` | KernelInstance.cpp:170 | 设置进程保护级别 | 内核 |
| `KernelInstance::SiEnumCallbacks()` | KernelInstance.cpp:783 | 枚举19种内核回调 | 内核 |
| `KernelInstance::RemoveCallback()` | KernelInstance.cpp:1256 | 移除内核回调 | 内核 |
| `KernelInstance::SiEnumSSDT()` | KernelInstance.cpp:497 | SSDT系统服务表枚举 | 内核 |
| `KernelInstance::SiEnumSSSDT()` | KernelInstance.cpp:524 | Shadow SSDT枚举 | 内核 |
| `KernelInstance::SiEnumIDT()` | KernelInstance.cpp:573 | 中断描述符表枚举 | 内核 |
| `KernelInstance::SiEnumGDT()` | KernelInstance.cpp:596 | 全局描述符表枚举 | 内核 |
| `KernelInstance::DisableDSE()` | KernelInstance.cpp:948 | 禁用驱动签名强制 | 内核 |
| `KernelInstance::DisablePatchGuard()` | KernelInstance.cpp:969 | 禁用内核补丁保护 (Premium) | 内核 |
| `KernelInstance::EnableLKD()` | KernelInstance.cpp:962 | 动态启用本地内核调试 | 内核 |
| `KernelInstance::EnableHypervisor()` | KernelInstance.cpp:861 | Hypervisor初始化 (Premium) | 内核 |
| `KernelInstance::BlueScreen()` | KernelInstance.cpp:981 | 手动触发BSOD | 内核 |
| `KernelInstance::SiDeleteFile()` | KernelInstance.cpp:783 | 内核级强制删除文件 | 内核 |
| `KernelInstance::SiEnumObjectsByDirectory()` | KernelInstance.cpp:1009 | NT对象目录浏览 | 用户态 |
| `DriverUtils::StopKernelDriver()` | DriverUtils.cpp:122 | 停止驱动服务 | 系统管理 |

---

## 七、安全边界分析

### 7.1 权限提升路径总结

```
普通用户 → [管理员] → [SYSTEM] → [TrustedInstaller] → [内核驱动]
                          ↑                             ↑
                    Elevator.h                    DriverUtils
                  (Token传递链)                 (SCM驱动加载)

                  SE_DEBUG_NAME              SERVICE_KERNEL_DRIVER
                  SE_TCB_NAME                StartServiceW
                  winlogon.exe → SYSTEM
                  TrustedInstaller 服务
```

### 7.2 防御对抗能力

| 对抗目标 | 使用手段 | 风险等级 |
|---|---|---|
| **DSE (驱动签名强制)** | DisableDSE() / Premium虚拟化绕过 | ★★★ |
| **PatchGuard (内核补丁保护)** | DisablePatchGuard() 虚拟化绕过 | ★★★ (Premium) |
| **进程保护 (PPL)** | SetPPL() — 可设置也可覆盖 | ★★ |
| **内核回调检测** | RemoveCallback() — 移除安全软件回调 | ★★★ |
| **反Rootkit检测** | SiHideProcess/SiHideDriver — 断链隐藏 | ★★★ |
| **PiDDB缓存追踪** | RemovePiDDBCache() — 清除驱动加载记录 | ★★ (Premium) |
| **MmUnloadedDrivers** | RemoveFromMmUnloadedDrivers | ★★ |
| **NTFS 文件锁定** | SiDeleteFileEx() — 基于NTFS MFT的强制删除 | ★★ |

### 7.3 架构局限

1. **驱动依赖**: 所有内核操作依赖 Sirius.sys 成功加载。如果驱动加载失败（DSE拦截、杀软拦截），整个系统退化到纯用户态工具。
2. **Free版限制**: 虚拟化 (Hypervisor)、PiDDB清理、PatchGuard禁用等高级功能仅限 Premium 版。
3. **BSOD风险**: Hide/Suspend/Terminate 等操作直接修改内核数据结构，错误操作极易导致系统崩溃。

---

## 八、数据流向总图

```
┌─────────────────────────────────────────────────────────────┐
│                        用户交互                              │
│    XAML Page → Button_Click → ViewModel → KernelInstance    │
└────────────────────────┬────────────────────────────────────┘
                         │
     ┌───────────────────┼───────────────────┐
     ▼                   ▼                    ▼
┌─────────┐      ┌─────────────┐      ┌──────────────┐
│ 枚举操作 │      │  修改操作     │      │  系统控制操作  │
│ (两阶段) │      │  (单次IOCTL)  │      │  (单次IOCTL)  │
└────┬─────┘      └──────┬──────┘      └──────┬───────┘
     │                   │                    │
     ▼                   ▼                    ▼
┌──────────────────────────────────────────────────────────┐
│              SiQuerySystemInformation()                   │
│              SiSetSystemInformation()                     │
│              SiSetProcessInformation()                    │
│              SiSetFileInformation()                       │
│                                                          │
│  ┌────────────────────────────────────────────┐          │
│  │      DeviceIoControl(hDevice, IOCTL,       │          │
│  │        &request, sizeof(request),          │          │
│  │        NULL, 0, &returned, NULL)           │          │
│  └────────────────────────────────────────────┘          │
└────────────────────────┬─────────────────────────────────┘
                         │ METHOD_BUFFERED IRP
                         ▼
┌──────────────────────────────────────────────────────────┐
│                 Sirius.sys 内核驱动                       │
│                                                          │
│  IRP_MJ_DEVICE_CONTROL → DispatchIoControl               │
│    ↓                                                     │
│  switch(IOCTL):                                          │
│    case SET_PROCESS_INFO:  → 操作 EPROCESS               │
│    case SET_SYSTEM_INFO:   → 修改内核全局变量              │
│    case QUERY_SYSTEM_INFO: → 遍历内核数据结构             │
│    case METAVERSE_*:       → VMX 根模式操作               │
│    ...                                                    │
│    ↓                                                     │
│  Irp->IoStatus.Status = STATUS_SUCCESS                   │
│  IoCompleteRequest(Irp, IO_NO_INCREMENT)                 │
└──────────────────────────────────────────────────────────┘
```

---

*分析完成于 2026-07-14，基于 StarlightGUI v4.0.0 源代码。驱动内核源码（Sirius.sys 的源代码）不在本仓库中，上述内核实现部分为基于 IOCTL 接口定义和使用模式的合理推断。*
