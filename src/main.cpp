/*
 * stamky / main.cpp
 *
 * WinMain/进程入口：建立 Per-Monitor-V2 DPI、Common Controls、COM STA、单实例互斥体，解析
 * --show/--settings 等启动命令，然后进入 App::run。初始化顺序很重要：使用 Shell/COM 对话框前
 * 必须先成功 CoInitializeEx；退出时只有本进程成功初始化的 COM 才对应 CoUninitialize。
 *
 * 单实例：新 stamky mutex 是正式身份；旧 StackyModern mutex 仅作迁移期探针。第二实例不直接
 * 操作第一实例的内存，而通过 Host 窗口 + WM_COPYDATA 发送有限长度命令。首次实例尚在启动时，
 * 转发采用短暂重试查找 Host；失败会明确返回而不是同时启动两个常驻 Host。
 *
 * DPI 兼容：优先动态解析 SetProcessDpiAwarenessContext，避免静态导入把“可尝试旧 Windows
 * 自行移植”变成不必要的加载期硬依赖。正式发行仍只测试 Windows 10+。
 */

#include "app.h"
#include <shellapi.h>

/* STAMKY_CN_DETAIL
 * 进程入口只做“全局一次性设施”初始化：启动日志、目录、DPI awareness、Common Controls、COM、语言、
 * 命令行和单实例仲裁。PerMonitorV2 使用动态 API，在现代系统启用；初始化失败会记录明确原因。
 * 单实例采用新 stamky mutex，旧 StackyModern mutex 仅用于迁移期探测，绝不再创建旧身份对象。
 */
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    using SetDpiCtx=BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if(auto p=reinterpret_cast<SetDpiCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext")))
        p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX ic{sizeof(ic),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES|ICC_BAR_CLASSES};
    if(!InitCommonControlsEx(&ic)){
        MessageBoxW(nullptr,L"stamky 无法初始化 Windows 公共控件。",L"stamky - 启动失败",MB_OK|MB_ICONERROR);
        return 5;
    }
    const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if(FAILED(co)){
        MessageBoxW(nullptr,L"stamky 无法初始化 Shell/COM 单线程单元，文件选择器和快捷方式功能无法安全工作。",L"stamky - 启动失败",MB_OK|MB_ICONERROR);
        return 6;
    }

    int argc=0;
    LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::wstring command;
    if(argv){
        for(int i=1;i<argc;++i){
            const std::wstring a=argv[i];
            if(a==L"--show"&&i+1<argc){
                const std::wstring id=argv[++i];
                POINT pt{};
                command=L"SHOW\t"+id;
                if(GetCursorPos(&pt))command+=L"\t"+std::to_wstring(pt.x)+L"\t"+std::to_wstring(pt.y);
            }else if(a==L"--manage") command=L"MANAGE";
            else if(a==L"--settings") command=L"SETTINGS";
            else if(a==L"--host") command=L"HOST";
        }
        LocalFree(argv);
    }

    // 仅用于“旧版本仍在运行”这一升级边界：不创建旧命名对象，只探测它是否存在。
    // 新进程的正式单实例对象始终是 stamky.Host.v2。
    HANDLE legacy=OpenMutexW(SYNCHRONIZE,FALSE,L"Local\\StackyModern.Host.v2");
    if(legacy){
        const bool ok=sm::App::forward_to_existing(command.empty()?L"MANAGE":command);
        CloseHandle(legacy);
        CoUninitialize();
        return ok?0:3;
    }

    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\stamky.Host.v2");
    if(!mutex){
        MessageBoxW(nullptr,L"stamky 无法创建单实例同步对象。",L"stamky - 启动失败",MB_OK|MB_ICONERROR);
        CoUninitialize();
        return 7;
    }
    const DWORD mutexError=GetLastError();
    if(mutexError==ERROR_ALREADY_EXISTS){
        const bool ok=sm::App::forward_to_existing(command.empty()?L"MANAGE":command);
        CloseHandle(mutex);
        CoUninitialize();
        return ok?0:3;
    }

    sm::App app;
    const int rc=app.run(command);
    CloseHandle(mutex);
    CoUninitialize();
    return rc;
}
