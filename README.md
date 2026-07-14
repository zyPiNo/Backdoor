# 一个基于Metasploit-framework内核级后门木马

Windows 服务/应用程序双模式，结合内核驱动实现 Ring 0 级进程控制。

## 架构概览

```
main.exe
├─ 应用模式 (--console)    手动运行，带控制台输出
└─ 服务模式 (SCM 管理)     作为 Windows 服务持久运行

两种模式执行完全相同的初始化流程:
  Phase 1    权限检测
  Phase 2    ElevateToKernelLevel — 13 项内核特权 + NULL DACL
  Phase 2.5  提取嵌入的 Sirius.sys → 加载内核驱动
  TI Phase   TrustedInstaller 提权（失败不阻塞）
  Kill Phase StartBackgroundKiller — 后台进程查杀线程
  Core       RunBusinessCore — 保活主循环，维护嵌入二进制程序
```

## 功能特性

### 内核级权限提升
- 13 项 Windows 特权（SeTcbPrivilege, SeDebugPrivilege, SeLoadDriverPrivilege 等）
- NULL DACL 安全描述符覆盖
- TrustedInstaller 提权链（winlogon → SYSTEM → TI Token）

### 内核驱动 (Sirius.sys)
- 编译时嵌入 exe 资源，运行时自动释放加载
- IOCTL 通信接口：进程终止、线程挂起/恢复等
- 终止策略：普通(0) → 线程(1) → 内存(2)，逐步升级

### 进程查杀
- 后台线程每 3 秒扫描进程列表
- 模糊匹配目标进程名（大小写不敏感）
- 匹中即以内核级权限强制终止

### 嵌入二进制程序
- XOR 加密的 payload 随主程序分发
- Master 保活循环：子进程退出立即重启
- TrustedInstaller 权限传递给子进程

### 字符串混淆
- 所有敏感字符串 XOR 0xD3 编译时加密
- 运行时 DecryptAllStrings 解密
- 反沙箱检测（文件特征 + 内存阈值）

### 反检测
- 字符串 XOR 混淆绕过静态特征扫描
- 反沙箱（Cuckoo Sandbox 特征检测）
- API 动态解析框架（ResolveAPI）

## 编译

### 依赖
- MinGW-w64 GCC
- Windows SDK（advapi32, wtsapi32, userenv）

### 编译前准备
- 通过msf生成二进制程序文件
- 将二进制程序文件编译为C语言数组并进行XOR运算后存入kPayload数组中
- 将XOR 解密密钥写入代码第314行

### 编译命令

```bash
gcc main.c -o main.exe \
    -mwindows \
    -ladvapi32 -lwtsapi32 -luserenv
```

需要先确保 `driver_data.h`（Sirius.sys 字节数组）和 `resource.h` 存在。

## 运行方式

### 应用模式
```cmd
main.exe --console
```

### 服务模式
由 SCM 自动启动，或手动安装服务后运行。

### 命令行参数

| 参数 | 说明 |
|------|------|
| `--console` | 显示控制台窗口，输出运行日志 |
| `--slave` | 以子进程模式运行（内部使用） |
| `--uninstall` | 卸载 Windows 服务并退出 |

### 后台查杀配置

在 `StartBackgroundKiller` 调用处修改目标进程关键词，例如：
```c
StartBackgroundKiller("Hips");  // 匹配所有含 "Hips" 的进程
```

支持逗号分隔多个目标：
```c
StartBackgroundKiller("Hips,notepad,calc");
```

## 项目文件

| 文件 | 说明 |
|------|------|
| `main.c` | 主程序源码（单文件） |
| `driver_data.h` | Sirius.sys 编译时嵌入的字节数组 |
| `resource.h` | 资源定义头文件 |
| `Assets/Sirius.sys` | 内核驱动原始文件 |
| `StarlightGUI/` | Sirius.sys 原始项目源码 |

## 安全声明

本程序仅供安全研究和教育用途。在未经授权的系统上使用可能违反法律法规。
使用内核驱动操作存在蓝屏风险，请在生产环境外充分测试。
