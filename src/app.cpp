/*
 * stamky / app.cpp
 *
 * 应用层总控模块。隐藏 Host 窗口是进程内 Win32 消息路由中心：负责托盘图标、
 * 单实例转发、TaskbarCreated 恢复、运行菜单 owner-draw 回调、Shell 上下文菜单消息转发，
 * 以及 Manager/Settings 顶层窗口的创建与生命周期协调。
 *
 * 重要生命周期：RuntimeCache 使用文件映射，RuntimeMenu 内的 Runtime*View 只在一次菜单
 * 构建/显示期间有效。重建 runtime.bin 前必须先确认没有活动 TrackPopupMenuEx 嵌套消息循环，
 * 再关闭映射、原子替换文件并重新 open；绝不能让 DrawEntry 或菜单回调引用已解除映射的内存。
 *
 * 消息重入：TrackPopupMenu/MessageBox/Shell 对话框都会运行嵌套消息循环。任何从这些路径
 * 间接进入的“重建/再次显示”请求都要遵守 RuntimeMenu 的 showing_ 状态机，而不能直接释放
 * 活动菜单资源。WM_COPYDATA 收到的内容先复制并验证，再 PostMessage 延后执行，避免在发送方
 * 的同步调用栈中执行高层逻辑造成重入。
 *
 * 兼容迁移：kLegacyHostClass/kLegacyHostTitle 仅用于发现 UI-06 以前的旧实例；新建窗口、
 * AppUserModelID 与其他正式身份一律使用 stamky。不要为了“清理字符串”删除这些兼容探针。
 *
 * 前台激活：任务栏组入口由临时第二实例启动后转发 SHOW。发送方必须 best-effort 把 Windows
 * 前台激活资格授予常驻 Host 进程；隐藏 Host 本身保持普通 TOOLWINDOW（不使用 NOACTIVATE），
 * RuntimeMenu 才能在 TrackPopupMenuEx 前切到前台并获得原生方向键/Enter/Esc 菜单导航。
 */

#include "app.h"
#include "about.h"
#include "lang.h"
#include "icon_cache.h"
#include "shell_utils.h"
#include <psapi.h>
#include <sstream>
#include <new>
#include <limits>

namespace sm {
namespace {
constexpr wchar_t kHostClass[]=L"stamky.HostWindow.v2";
constexpr wchar_t kHostTitle[]=L"stamky.Host";
constexpr wchar_t kLegacyHostClass[]=L"StackyModern.HostWindow.v2";
constexpr wchar_t kLegacyHostTitle[]=L"StackyModern.Host";
constexpr ULONG_PTR kCopyDataCommand=1;
constexpr DWORD kMaxCopyDataBytes=64u*1024u;
constexpr UINT WM_TRAY=WM_APP+10;
constexpr UINT WM_EXECUTE_COMMAND=WM_APP+11;
enum{ID_TRAY_MANAGE=40001,ID_TRAY_SETTINGS,ID_TRAY_REBUILD,ID_TRAY_STATUS,ID_TRAY_ABOUT,ID_TRAY_EXIT};
}

/* STAMKY_CN_DETAIL
 * 单实例转发只把命令发给已经找到的 Host 窗口。WM_COPYDATA 是同步跨进程消息：发送端缓冲只需在
 * SendMessageTimeoutW 调用期间有效；接收端必须在返回前复制 payload，不能保存 lpData 指针。
 * 超时可避免旧实例卡死时新进程永久挂住。
 */
bool App::forward_to_existing(const std::wstring& command){
    if((command.size()+1)>(kMaxCopyDataBytes/sizeof(wchar_t)))return false;
    HWND h=nullptr;
    for(int i=0;i<50&&!h;++i){
        h=FindWindowW(kHostClass,kHostTitle);
        if(!h)h=FindWindowW(kLegacyHostClass,kLegacyHostTitle);
        if(!h)Sleep(10);
    }
    if(!h)return false;

    // 任务栏“组快捷方式”通常会启动一个很快退出的第二实例，再由它把 SHOW 命令
    // 转发给已经常驻的 Host。此时 Windows 的前台激活资格属于新实例；如果不把资格
    // 显式转交给 Host 所在进程，第一实例虽然能弹出 TrackPopupMenuEx，键盘焦点却可能
    // 仍停在 Explorer/任务栏，表现为方向键完全不能导航菜单。
    //
    // AllowSetForegroundWindow 只授予一次前台切换资格，不强行抢焦点；调用方当前没有
    // 资格时它可能失败，因此这里只做 best-effort。后续 WM_COPYDATA 仍照常发送，鼠标
    // 路径不会因为激活授权失败而失效。目标 PID 来自刚刚找到的可信 Host HWND。
    DWORD targetPid=0;
    GetWindowThreadProcessId(h,&targetPid);
    if(targetPid)AllowSetForegroundWindow(targetPid);

    COPYDATASTRUCT cds{};
    cds.dwData=kCopyDataCommand;
    cds.cbData=static_cast<DWORD>((command.size()+1)*sizeof(wchar_t));
    cds.lpData=const_cast<wchar_t*>(command.c_str());
    DWORD_PTR ignored=0;
    return SendMessageTimeoutW(h,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&cds),SMTO_ABORTIFHUNG,1000,&ignored)!=0;
}

/* STAMKY_CN_DETAIL
 * Host 主生命周期顺序：加载权威 Model/Settings -> 必要时构建并打开 runtime cache -> 创建隐藏 Host ->
 * 托盘 -> 处理初始命令 -> GetMessage 循环。Manager/Settings 的 IsDialogMessage 预处理发生在 Dispatch 前，
 * 以保留键盘导航；GetMessage 返回 -1 是 API 失败而不是 WM_QUIT，必须单独记录。
 */
int App::run(const std::wstring& initialCommand){
    ensure_directories();
    reset_startup_log();
    startup_log(L"Host 启动：进入 App::run");
    settings_.load();
    lang().ensure_default_files();
    lang().load(settings_.language);
    startup_log(L"设置与语言加载完成");

    if(!model_.load()){
        startup_log(L"致命：groups.tsv 无法完整、安全解析；未改写原文件");
        MessageBoxW(nullptr,tr(L"分组数据无法安全读取。程序已停止启动，并且不会覆盖现有 groups.tsv。\n\n请检查 data\\groups.tsv 和 data\\启动诊断.log。").c_str(),tr(L"stamky - 启动失败").c_str(),MB_OK|MB_ICONERROR);
        return 4;
    }
    startup_log(L"分组数据读取完成");
    const size_t migrated=migrate_legacy_group_shortcuts(model_);
    if(migrated)startup_log(L"已迁移旧项目命名的组快捷方式："+std::to_wstring(migrated)+L" 个");

    if(!cache_.open()){
        startup_log(L"runtime.bin 不存在/版本旧/不可读，开始构建");
        if(!builder_.build(model_)||!cache_.open()){
            startup_log(L"致命：runtime.bin 构建或映射失败");
            MessageBoxW(nullptr,tr(L"运行缓存无法构建或读取。\n\n请检查磁盘可写空间以及 data\\启动诊断.log。").c_str(),tr(L"stamky - 启动失败").c_str(),MB_OK|MB_ICONERROR);
            return 5;
        }
        startup_log(L"runtime.bin 构建并校验完成");
    }
    menu_=std::make_unique<RuntimeMenu>(cache_,settings_);
    if(!create_host()){
        startup_log(L"致命：隐藏 Host 窗口创建失败");
        MessageBoxW(nullptr,tr(L"stamky 无法创建后台 Host 窗口。\n\n请把 data\\启动诊断.log 发回用于定位。").c_str(),tr(L"stamky - 启动失败").c_str(),MB_OK|MB_ICONERROR);
        return 2;
    }
    startup_log(L"隐藏 Host 窗口创建成功");
    taskbarCreatedMsg_=RegisterWindowMessageW(L"TaskbarCreated");
    add_tray();
    if(!initialCommand.empty())execute_command(initialCommand);
    else if(model_.groups.size()==1&&model_.groups[0].items.empty())open_manager();

    startup_log(L"进入消息循环");
    MSG msg{};
    int gm=0;
    while((gm=GetMessageW(&msg,nullptr,0,0))>0){
        if(manager_&&manager_->pretranslate(msg))continue;
        if(settingsWindow_&&settingsWindow_->pretranslate(msg))continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if(gm<0){startup_log(L"GetMessageW 失败："+win32_error_text(GetLastError()));remove_tray();return 8;}
    remove_tray();
    return static_cast<int>(msg.wParam);
}

bool App::create_host(){WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=WndProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kHostClass;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);if(!RegisterClassExW(&wc)){DWORD e=GetLastError();if(e!=ERROR_CLASS_ALREADY_EXISTS){startup_log(L"RegisterClassExW(Host) 失败："+win32_error_text(e));return false;}}
    // Host 从不 ShowWindow，WS_EX_TOOLWINDOW 已足够保证它不进入任务栏。这里不能再加
    // WS_EX_NOACTIVATE：RuntimeMenu::show_once 在 TrackPopupMenuEx 前需要把 Host 所在线程
    // 可靠切到前台，原生菜单才能接收 ↑/↓/←/→/Enter/Esc。NOACTIVATE 对一个永远隐藏
    // 的窗口没有用户可见收益，却会破坏任务栏快捷方式转发后的键盘菜单语义。
    HWND created=CreateWindowExW(WS_EX_TOOLWINDOW,kHostClass,kHostTitle,WS_POPUP,0,0,1,1,nullptr,nullptr,wc.hInstance,this);if(!created){startup_log(L"CreateWindowExW(Host) 失败："+win32_error_text(GetLastError()));return false;}hwnd_=created;return true;}
LRESULT CALLBACK App::WndProc(HWND h,UINT m,WPARAM w,LPARAM l){auto*self=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);if(self)self->hwnd_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}return self?self->proc(h,m,w,l):DefWindowProcW(h,m,w,l);}

void App::add_tray(){
    nid_={};
    nid_.cbSize=sizeof(nid_);
    nid_.hWnd=hwnd_;
    nid_.uID=1;
    nid_.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
    nid_.uCallbackMessage=WM_TRAY;
    nid_.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_STAMKY));
    if(!nid_.hIcon){startup_log(L"警告：托盘图标资源加载失败");nid_.hWnd=nullptr;return;}
    wcscpy_s(nid_.szTip,L"stamky");
    if(!Shell_NotifyIconW(NIM_ADD,&nid_)){startup_log(L"警告：托盘图标添加失败");nid_.hWnd=nullptr;return;}
    nid_.uVersion=NOTIFYICON_VERSION_4;
    if(!Shell_NotifyIconW(NIM_SETVERSION,&nid_))startup_log(L"警告：托盘图标未能切换到 NOTIFYICON_VERSION_4");
}
void App::remove_tray(){
    if(nid_.hWnd)Shell_NotifyIconW(NIM_DELETE,&nid_);
    nid_.hWnd=nullptr;
}
void App::show_tray_menu(std::optional<POINT> anchor){
    POINT p{};
    if(anchor)p=*anchor;
    else if(!GetCursorPos(&p)){startup_log(L"GetCursorPos(Tray) 失败："+win32_error_text(GetLastError()));return;}
    HMENU m=CreatePopupMenu();
    if(!m){startup_log(L"CreatePopupMenu(Tray) 失败："+win32_error_text(GetLastError()));return;}
    const bool built=
        AppendMenuW(m,MF_STRING,ID_TRAY_MANAGE,tr(L"打开内容管理").c_str())&&
        AppendMenuW(m,MF_STRING,ID_TRAY_SETTINGS,tr(L"设置").c_str())&&
        AppendMenuW(m,MF_STRING,ID_TRAY_REBUILD,tr(L"重建图标与运行缓存").c_str())&&
        AppendMenuW(m,MF_STRING,ID_TRAY_STATUS,tr(L"性能状态").c_str())&&
        AppendMenuW(m,MF_SEPARATOR,0,nullptr)&&
        AppendMenuW(m,MF_STRING,ID_TRAY_ABOUT,tr(L"关于").c_str())&&
        AppendMenuW(m,MF_STRING,ID_TRAY_EXIT,tr(L"退出").c_str());
    if(!built){startup_log(L"AppendMenuW(Tray) 失败："+win32_error_text(GetLastError()));DestroyMenu(m);return;}
    SetForegroundWindow(hwnd_);
    const int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd_,nullptr);
    DestroyMenu(m);
    PostMessageW(hwnd_,WM_NULL,0,0);
    if(cmd)PostMessageW(hwnd_,WM_COMMAND,static_cast<WPARAM>(cmd),0);
}

void App::open_manager(){if(!manager_)manager_=std::make_unique<ManagerWindow>(model_,settings_,[this]{return rebuild_runtime();},[this]{open_settings();},[this](const std::wstring&id){if(menu_)menu_->show(hwnd_,id,MenuAnchorMode::Cursor);});manager_->show(hwnd_);}
void App::open_settings(){if(!settingsWindow_)settingsWindow_=std::make_unique<SettingsWindow>(settings_);settingsWindow_->show(hwnd_);}
/* STAMKY_CN_DETAIL
 * rebuild_runtime() 不能在 RuntimeMenu 的 TrackPopupMenuEx 嵌套循环仍活动时直接关闭 mmap：菜单中的
 * Runtime*View/DrawEntry 可能仍引用旧映射。活动时只延迟重建；安全点先 close 旧映射，再构建新文件、
 * reopen/parse，失败则明确报错，不让半有效 cache 被继续使用。
 */
bool App::rebuild_runtime(){
    // TrackPopupMenuEx 会运行嵌套消息循环；活动菜单的 DrawEntry/位图以及 RuntimeCache
    // 视图都可能仍在被系统回调读取。此时绝不能解除映射或重建缓存。
    if(menu_&&menu_->is_showing()){
        startup_log(L"运行缓存重建被拒绝：当前仍有活动弹出菜单");
        return false;
    }
    // runtime.bin 在正常运行时由 RuntimeCache 内存映射。Windows 不保证可直接用
    // MoveFileEx(REPLACE_EXISTING) 覆盖仍被映射的目标文件，因此必须先解除映射。
    cache_.close();
    if(!builder_.build(model_)){
        startup_log(L"运行缓存构建失败：已尝试重新打开旧缓存");
        cache_.open();
        return false;
    }
    if(!cache_.open()){
        startup_log(L"运行缓存构建成功，但重新映射失败");
        return false;
    }
    if(menu_)menu_->clear_bitmaps();
    return true;
}
void App::execute_command(const std::wstring& c){
    if(c==L"MANAGE"){open_manager();return;}
    if(c==L"SETTINGS"){open_settings();return;}
    if(c==L"HOST") return;
    if(c.rfind(L"SHOW\t",0)==0){
        std::vector<std::wstring> parts;
        size_t start=0;
        while(start<=c.size()){
            size_t pos=c.find(L'\t',start);
            parts.push_back(c.substr(start,pos==std::wstring::npos?std::wstring::npos:pos-start));
            if(pos==std::wstring::npos) break;
            start=pos+1;
        }
        if(parts.size()>=2){
            std::optional<POINT> invocation;
            if(parts.size()>=4){
                try{
                    const long long x=std::stoll(parts[2]);
                    const long long y=std::stoll(parts[3]);
                    if(x>=std::numeric_limits<LONG>::min()&&x<=std::numeric_limits<LONG>::max()&&
                       y>=std::numeric_limits<LONG>::min()&&y<=std::numeric_limits<LONG>::max()){
                        POINT p{static_cast<LONG>(x),static_cast<LONG>(y)};
                        invocation=p;
                    }
                }catch(...){ }
            }
            if(menu_&&!menu_->show(hwnd_,parts[1],MenuAnchorMode::TaskbarAware,invocation))
                MessageBoxW(nullptr,tr(L"找不到该分组，可能快捷方式已经过期。").c_str(),tr(L"stamky").c_str(),MB_OK|MB_ICONWARNING);
        }
    }
}

void App::show_status(){PROCESS_MEMORY_COUNTERS_EX pm{};pm.cb=sizeof(pm);if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),sizeof(pm))){MessageBoxW(hwnd_,tr(L"无法读取当前进程的内存统计。").c_str(),tr(L"stamky - 性能状态").c_str(),MB_OK|MB_ICONWARNING);return;}std::wostringstream s;s.setf(std::ios::fixed);s.precision(1);s<<(pm.WorkingSetSize/1024.0/1024.0);std::wstring ws=s.str();s.str(L"");s<<(pm.PrivateUsage/1024.0/1024.0);std::wstring pc=s.str();s.str(L"");s.precision(2);s<<(menu_?menu_->last_prepare_ms():0.0);std::wstring ms=s.str();wchar_t buf[2048]{};swprintf_s(buf,tr(L"Working Set：%s MB\\nPrivate Commit：%s MB\\n最近一次菜单准备：%s ms\\n\\n管理窗口关闭后，其 ListView、图像列表和字体资源会销毁；Runtime 图标只保留小型 LRU。").c_str(),ws.c_str(),pc.c_str(),ms.c_str());MessageBoxW(hwnd_,buf,tr(L"stamky - 性能状态").c_str(),MB_OK|MB_ICONINFORMATION);}

/* STAMKY_CN_DETAIL
 * 隐藏 Host 的窗口过程承担托盘、单实例 WM_COPYDATA、Runtime owner-draw 菜单以及任务栏重建通知。
 * 这是多个嵌套消息源汇合点：任何来自 WM_COPYDATA 的数据先复制再 PostMessage；Shell context menu
 * 的 IContextMenu2/3 消息只在其同步生命周期内转发；WM_MEASUREITEM/WM_DRAWITEM 交给 RuntimeMenu。
 */
LRESULT App::proc(HWND h,UINT m,WPARAM w,LPARAM l){
    LRESULT shellContextResult=0;
    if(handle_shell_context_menu_message(m,w,l,&shellContextResult)) return shellContextResult;
    if(taskbarCreatedMsg_&&m==taskbarCreatedMsg_){add_tray();return 0;}
    switch(m){
    case WM_MEASUREITEM:if(menu_&&menu_->handle_measure_item(reinterpret_cast<MEASUREITEMSTRUCT*>(l)))return TRUE;break;
    case WM_DRAWITEM:if(menu_&&menu_->handle_draw_item(reinterpret_cast<DRAWITEMSTRUCT*>(l)))return TRUE;break;
    case WM_INITMENUPOPUP:if(menu_&&menu_->handle_init_menu_popup(reinterpret_cast<HMENU>(w)))return 0;break;
    case WM_MENURBUTTONUP:if(menu_&&menu_->handle_menu_rbutton_up(static_cast<UINT>(w),reinterpret_cast<HMENU>(l),h))return 0;break;
    case WM_COPYDATA:{
        auto* cds=reinterpret_cast<COPYDATASTRUCT*>(l);
        if(!cds||cds->dwData!=kCopyDataCommand||!cds->lpData||cds->cbData<sizeof(wchar_t)||
           cds->cbData>kMaxCopyDataBytes||(cds->cbData%sizeof(wchar_t))!=0)return FALSE;
        const size_t chars=cds->cbData/sizeof(wchar_t);
        const auto* data=static_cast<const wchar_t*>(cds->lpData);
        if(!chars||data[chars-1]!=L'\0')return FALSE;
        auto* copy=new(std::nothrow) std::wstring(data,chars-1);
        if(!copy)return FALSE;
        if(!PostMessageW(h,WM_EXECUTE_COMMAND,0,reinterpret_cast<LPARAM>(copy))){delete copy;return FALSE;}
        return TRUE;
    }
    case WM_EXECUTE_COMMAND:{
        std::unique_ptr<std::wstring> command(reinterpret_cast<std::wstring*>(l));
        if(command)execute_command(*command);
        return 0;
    }
    case WM_TRAY:{
        const UINT notification=LOWORD(l);
        std::optional<POINT> anchor;
        // NOTIFYICON_VERSION_4 把托盘菜单锚点放在 wParam；旧语义下 HIWORD(lParam)==0，
        // 因而自动回退 GetCursorPos，不会把旧版的图标 ID 误当坐标。
        if(HIWORD(l)==nid_.uID){POINT p{GET_X_LPARAM(w),GET_Y_LPARAM(w)};anchor=p;}
        switch(notification){
        case WM_CONTEXTMENU:case WM_RBUTTONUP:show_tray_menu(anchor);break;
        case WM_LBUTTONDBLCLK:open_manager();break;
        }
        return 0;
    }
    case WM_COMMAND:switch(LOWORD(w)){case ID_TRAY_MANAGE:open_manager();break;case ID_TRAY_SETTINGS:open_settings();break;case ID_TRAY_REBUILD:{SetCursor(LoadCursorW(nullptr,IDC_WAIT));IconCache ic;for(auto&g:model_.groups)for(auto&it:g.items)if(it.type!=ItemType::Group)ic.ensure(it);bool ok=rebuild_runtime();SetCursor(LoadCursorW(nullptr,IDC_ARROW));MessageBoxW(hwnd_,tr(ok?L"缓存重建完成。":L"缓存重建失败，请查看 data\\启动诊断.log。").c_str(),tr(L"stamky").c_str(),MB_OK|(ok?MB_ICONINFORMATION:MB_ICONERROR));break;}case ID_TRAY_STATUS:show_status();break;case ID_TRAY_ABOUT:show_about(h);break;case ID_TRAY_EXIT:DestroyWindow(h);break;}return 0;
    case WM_DESTROY:remove_tray();PostQuitMessage(0);return 0;
    case WM_NCDESTROY:SetWindowLongPtrW(h,GWLP_USERDATA,0);if(hwnd_==h)hwnd_=nullptr;break;
    }
    return DefWindowProcW(h,m,w,l);
}
}
