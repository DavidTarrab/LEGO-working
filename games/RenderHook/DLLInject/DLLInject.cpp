#include <Windows.h>
#include <cstdio>
#include <array>
#include <stdexcept>
#include <TlHelp32.h>
#include <iostream>
#include <tchar.h>
#include <comdef.h>
// 启用调试权限
bool EnableDebugPrivileges() {
    HANDLE hToken;
    LUID seDebugNameValue;
    TOKEN_PRIVILEGES tokenPriv;

    // 打开当前进程的访问令牌
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        std::cerr << "Failed to open process token\n";
        return false;
    }

    // 查找 SE_DEBUG_NAME 的 LUID
    if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &seDebugNameValue)) {
        std::cerr << "Failed to lookup privilege value\n";
        CloseHandle(hToken);
        return false;
    }

    // 设置权限结构
    tokenPriv.PrivilegeCount = 1;
    tokenPriv.Privileges[0].Luid = seDebugNameValue;
    tokenPriv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // 调整令牌的权限
    if (!AdjustTokenPrivileges(hToken, FALSE, &tokenPriv, sizeof(tokenPriv), NULL, NULL)) {
        std::cerr << "Failed to adjust token privileges\n";
        CloseHandle(hToken);
        return false;
    }

    // 关闭令牌句柄
    CloseHandle(hToken);
    return true;
}

DWORD GetProcessIdByName(const char* processName)
{
    DWORD dwProcID = 0;
    HANDLE hProcessSnap;
    PROCESSENTRY32 pe32;

    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (INVALID_HANDLE_VALUE == hProcessSnap)
        return(FALSE);

    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (!Process32First(hProcessSnap, &pe32))
    {
        CloseHandle(hProcessSnap);
        std::cout << "!!! Failed to gather information on system processes! \n";
        return(NULL);
    }
    do
    {
        _bstr_t b(pe32.szExeFile);
        if (!strcmp(processName, b))
        {
            std::cout << b << " : " << pe32.th32ProcessID << std::endl;
            dwProcID = pe32.th32ProcessID;

        }
        //cout << pe32.szExeFile << endl;
    } while (Process32Next(hProcessSnap, &pe32));

    CloseHandle(hProcessSnap);

    return dwProcID;
}

// DLL注入
bool InjectDLL(DWORD processID, const char* dllPath)
{
    // 获取游戏进程句柄
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processID);
    if (hProcess == NULL)
    {
        std::cerr << "Failed to open target process!\n";
        printf("OpenProcess() fail %d\n",GetLastError());
        return false;
    }
    
    // 在游戏进程的虚拟内存空间为DLL路径字符串分配内存
    void* pDllPath = VirtualAllocEx(hProcess, NULL, strlen(dllPath) + 1, MEM_COMMIT, PAGE_READWRITE);
    if (pDllPath == NULL)
    {
        std::cerr << "Failed to allocate memory in target process!\n";
        printf("VirtualAllocEx() fail %d\n",GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    // 将DLL路径字符串写入游戏进程的内存中
    if (!WriteProcessMemory(hProcess, pDllPath, (void*)dllPath,
        strlen(dllPath) + 1, NULL))
    {
        std::cerr << "Failed to write DLL path to target process memory!\n";
        VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // 获取LoadLibraryA函数地址--首先获取Kernel32模块句柄
    HMODULE hKernel32 = LoadLibraryA("kernel32.dll");
    if (hKernel32 == NULL)
    {
        std::cerr << "Failed to get Kernel32 module handle!\n";
        VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }
    
    // 获取LoadLibraryA函数地址
    FARPROC pLoadLibrary = GetProcAddress(hKernel32, "LoadLibraryA");
    if (pLoadLibrary == NULL)
    {
        std::cerr << "Failed to get LoadLibraryA function address!\n";
        VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // 在游戏进程中创建线程，执行LoadLibraryA来加载DLL
    HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)pLoadLibrary, pDllPath, 0, NULL);
    if (hRemoteThread == NULL)
    {
        std::cerr << "Failed to create remote thread in target process!\n";
        VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
        CloseHandle(hRemoteThread);
        CloseHandle(hProcess);
        return false;
    }

    ResumeThread(hRemoteThread);

    // 等待DLL注入完成
    WaitForSingleObject(hRemoteThread, INFINITE);

    // 释放分配的内存以及句柄
    VirtualFreeEx(hProcess, pDllPath, 0, MEM_RELEASE);
    CloseHandle(hRemoteThread);
    CloseHandle(hProcess);

    std::cout << "DLL injected successfully!\n";
    return true;
}

int main(int argc, char* argv[])
{
    // if (argc != 3)
    // {
    //     std::cerr << "Usage: " << argv[0] << " <game name> <hook.dll path>\n";
    //     return 1;
    // }

    const char* gameProcessName = "CivilizationVI_DX12.exe";
    const char* dllPath = "E:\\Program Files\\RenderHook\\RenderHookDLL\\x64\\Release\\RenderHookDLL.dll";

    // 启用调试权限
    if (!EnableDebugPrivileges()) {
        std::cerr << "Failed to enable debug privileges\n";
        return 1;
    }

    // 启动游戏
    DWORD processID = GetProcessIdByName(gameProcessName);
    if (processID == 0) {
        std::cerr << "Failed to find process " << gameProcessName << std::endl;
        return 1;
    }

    // 在游戏进程中注入DLL
    if (!InjectDLL(processID, dllPath))
    {
        std::cerr << "Failed to inject DLL!\n";
        return 1;
    }
    
    return 0;
}
