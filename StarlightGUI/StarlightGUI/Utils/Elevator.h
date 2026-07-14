#pragma once
#include <windows.h>
#include <string>
#include <tlhelp32.h>
#include <vector>
#include <Utils/Utils.h>

using namespace winrt;
using namespace StarlightGUI::implementation;

inline bool EnableAllPrivileges(HANDLE hToken) {
    DWORD dwSize;
    if (!GetTokenInformation(hToken, TokenPrivileges, nullptr, 0, &dwSize)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }
    }

    std::vector<BYTE> buffer(dwSize);
    PTOKEN_PRIVILEGES pTokenPrivileges = reinterpret_cast<PTOKEN_PRIVILEGES>(buffer.data());

    if (!GetTokenInformation(hToken, TokenPrivileges, pTokenPrivileges, dwSize, &dwSize)) {
        return false;
    }

    for (DWORD i = 0; i < pTokenPrivileges->PrivilegeCount; i++) {
        pTokenPrivileges->Privileges[i].Attributes |= SE_PRIVILEGE_ENABLED;
    }

    if (!AdjustTokenPrivileges(hToken, FALSE, pTokenPrivileges, dwSize, nullptr, nullptr)) {
        return false;
    }

    return GetLastError() == ERROR_SUCCESS;
}

inline bool CreateProcessElevated(std::wstring processName, bool fullPrivileges, std::wstring extraArgs = L"") {

    if (!EnablePrivilege(SE_DEBUG_NAME)) {
        LOG_ERROR(L"Elevator", L"Failed to obtain ES_DEBUG_PRIVILEGE.");
        return false;
    }

    if (!EnablePrivilege(SE_TCB_NAME)) {
        LOG_ERROR(L"Elevator", L"Failed to obtain SE_TCB_PRIVILEGE.");
        return false;
    }

    HANDLE hSystemToken = nullptr;
    HANDLE hImpersonationToken = nullptr;
    HANDLE hTrustedInstallerProcessToken = nullptr;
    HANDLE hTrustedInstallerToken = nullptr;

    DWORD winlogonPid = FindProcessId(L"winlogon.exe");
    if (winlogonPid == 0) {
        LOG_ERROR(L"Elevator", L"Failed to find Winlogon.exe.");
        return false;
    }

    HANDLE hWinlogon = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winlogonPid);
    if (!hWinlogon) {
        LOG_ERROR(L"Elevator", L"Failed to open Winlogon.exe.");
        return false;
    }

    HANDLE hWinlogonToken = nullptr;
    if (!OpenProcessToken(hWinlogon, TOKEN_DUPLICATE | TOKEN_QUERY, &hWinlogonToken)) {
        CloseHandle(hWinlogon);
        LOG_ERROR(L"Elevator", L"Failed to get Winlogon.exe token.");
        return false;
    }
    CloseHandle(hWinlogon);

    if (!DuplicateTokenEx(hWinlogonToken, MAXIMUM_ALLOWED, nullptr,
        SecurityImpersonation, TokenPrimary, &hSystemToken)) {
        CloseHandle(hWinlogonToken);
        LOG_ERROR(L"Elevator", L"Failed to duplicate Winlogon.exe token, step 1.");
        return false;
    }

    if (!DuplicateTokenEx(hWinlogonToken, MAXIMUM_ALLOWED, nullptr,
        SecurityImpersonation, TokenImpersonation, &hImpersonationToken)) {
        CloseHandle(hWinlogonToken);
        CloseHandle(hSystemToken);
        LOG_ERROR(L"Elevator", L"Failed to duplicate Winlogon.exe token, step 2.");
        return false;
    }
    CloseHandle(hWinlogonToken);

    if (!ImpersonateLoggedOnUser(hImpersonationToken)) {
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"Failed to impersonate logged on SYSTEM.");
        return false;
    }

    if (!SetThreadToken(NULL, hImpersonationToken)) {
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"Failed to set thread token.");
        return false;
    }

    SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scManager) {
        RevertToSelf();
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"Failed to open SCManager.");
        return false;
    }

    SC_HANDLE service = OpenServiceW(scManager, L"TrustedInstaller", SERVICE_ALL_ACCESS);
    bool serviceStarted = false;

    if (service) {
        if (!StartServiceW(service, 0, nullptr)) {
            if (GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                LOG_ERROR(L"Elevator", L"Failed to start service, trying to start process directly...");
                std::wstring trustedInstallerPath = L"C:\\Windows\\servicing\\TrustedInstaller.exe";

                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi = { 0 };

                if (CreateProcessAsUserW(hSystemToken, trustedInstallerPath.c_str(),
                    nullptr, nullptr, nullptr, FALSE, 0,
                    nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    serviceStarted = true;
                }
                else {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    serviceStarted = false;
                }
            }
            else {
                serviceStarted = true;
            }
        }
        else {
            serviceStarted = true;
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scManager);

    if (!serviceStarted) {
        RevertToSelf();
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"All attempts to starting TrustedInstaller are failed!");
        return false;
    }


    DWORD tiPid = 0;
    for (int i = 0; i < 10; i++) {
        tiPid = FindProcessId(L"TrustedInstaller.exe");
        if (tiPid != 0) break;
        Sleep(500);
    }

    if (tiPid == 0) {
        RevertToSelf();
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"Failed to find TrustedInstaller.exe.");
        return false;
    }

    HANDLE hTiProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, tiPid);
    if (!hTiProcess) {
        RevertToSelf();
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"Failed to open TrustedInstaller.exe.");
        return false;
    }

    if (!OpenProcessToken(hTiProcess, TOKEN_DUPLICATE, &hTrustedInstallerProcessToken)) {
        CloseHandle(hTiProcess);
        RevertToSelf();
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"Failed to get TrustedInstaller.exe token.");
        return false;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa) };
    if (!DuplicateTokenEx(hTrustedInstallerProcessToken, TOKEN_ALL_ACCESS, &sa, SecurityImpersonation, TokenPrimary, &hTrustedInstallerToken)) {
        CloseHandle(hTiProcess);
        RevertToSelf();
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"Failed to duplicate TrustedInstaller.exe token.");
        return false;
    }
    CloseHandle(hTiProcess);

    if (fullPrivileges) {
        if (!EnableAllPrivileges(hTrustedInstallerToken)) {
            CloseHandle(hTrustedInstallerToken);
            RevertToSelf();
            CloseHandle(hSystemToken);
            CloseHandle(hImpersonationToken);
            LOG_ERROR(L"Elevator", L"Failed to enable all privileges.");
            return false;
        }
    }

    DWORD currentSessionId = WTSGetActiveConsoleSessionId();
    ProcessIdToSessionId(GetCurrentProcessId(), &currentSessionId);

    if (!SetTokenInformation(hTrustedInstallerToken, TokenSessionId, &currentSessionId, sizeof(currentSessionId))) {
        CloseHandle(hTrustedInstallerToken);
        RevertToSelf();
        CloseHandle(hSystemToken);
        CloseHandle(hImpersonationToken);
        LOG_ERROR(L"Elevator", L"Failed to set token information.");
        return false;
    }

    RevertToSelf();

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    std::wstring commandLine = L"\"" + processName + L"\"";
    if (!extraArgs.empty()) {
        commandLine += L" ";
        commandLine += extraArgs;
    }

    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOW;

    if (!CreateProcessWithTokenW(hTrustedInstallerToken,
        LOGON_WITH_PROFILE,
        processName.c_str(),
        commandLine.data(),
        0,
        nullptr,
        nullptr,
        &si,
        &pi)) {
        LOG_ERROR(L"Elevator", L"CreateProcessWithTokenW() failed, trying CreateProcessAsUserW()...");

        if (!CreateProcessAsUserW(hTrustedInstallerToken,
            processName.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(hTrustedInstallerToken);
            CloseHandle(hSystemToken);
            CloseHandle(hImpersonationToken);
            LOG_ERROR(L"Elevator", L"CreateProcessAsUserW() failed!");
            return false;
        }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hTrustedInstallerToken);
    CloseHandle(hSystemToken);
    CloseHandle(hImpersonationToken);

    return true;
}
