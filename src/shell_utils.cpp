/*
 * stamky / shell_utils.cpp
 *
 * Windows Shell/COM 交互集中层：文件/文件夹选择、ShellLink 创建与校验、旧快捷方式迁移、
 * ShellExecute 启动、属性页、原生 IContextMenu2/3 右键菜单以及窗口 AppUserModelID。把这些
 * 高重入/高所有权复杂度接口集中在一个模块，避免各窗口重复实现略有差异的 COM 释放规则。
 *
 * ShellLink 写入采用“临时 .lnk -> IPersistFile::Save -> 重新加载回读关键字段 -> 原子替换”策略。
 * 只有 target/arguments/AppUserModelID 等关键身份回读一致，临时链接才替换正式文件；这样用户
 * 不会拿到一个 COM 调用表面成功、实际内容错误的任务栏入口。
 *
 * 原生上下文菜单：IContextMenu2/3 在 TrackPopupMenuEx 期间需要 owner 窗口转发 WM_INITMENUPOPUP、
 * WM_DRAWITEM、WM_MEASUREITEM、WM_MENUCHAR。活动接口指针只在同步菜单调用期间有效，退出后立即
 * 清空全局转发状态并 Release，且禁止两个 Shell 上下文菜单重叠进入。
 *
 * 迁移：只识别旧 StackyModern/stackymorden 可执行目标并重建为 stamky；Stacky/Stahky 上游
 * 名称与普通用户文件不参与替换。任务栏已经固定的外部副本无法可靠原地迁移，需要用户重建/重固定。
 */

#include "shell_utils.h"
#include "lang.h"
#include <shlwapi.h>
#include <propkey.h>
#include <propsys.h>

namespace sm {
namespace {
thread_local IContextMenu2* g_contextMenu2 = nullptr;
thread_local IContextMenu3* g_contextMenu3 = nullptr;

bool is_context_forward_message(UINT msg) {
    return msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR;
}
}

/* STAMKY_CN_DETAIL
 * IFileOpenDialog 属于 COM 对象，SetOptions/SetFileTypes/Show/GetResults 每一步都可能独立失败。
 * 筛选器的 COMDLG_FILTERSPEC 指针只在 Show 调用期间需要保持有效，所以 backing wstring/vector 生命周期
 * 必须覆盖该调用。用户取消属于正常空结果，不与配置 HRESULT 失败混为一谈。
 */
std::vector<std::wstring> pick_files(HWND owner, const std::vector<std::wstring>& exts) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;

    DWORD opts = 0;
    if(FAILED(dlg->GetOptions(&opts))||
       FAILED(dlg->SetOptions(opts|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_ALLOWMULTISELECT|FOS_NODEREFERENCELINKS))){
        dlg->Release();
        return out;
    }
    // FOS_NODEREFERENCELINKS is critical: selecting xxx.lnk must return the .lnk itself,
    // not silently resolve it to the target executable.
    // 过滤器随设置的可启动格式动态生成；exts 为空时回退内置默认全集。
    const std::vector<std::wstring>& list = exts.empty() ? default_launch_extensions() : exts;
    std::wstring patterns;
    for (const auto& e : list) {
        if (!patterns.empty()) patterns += L";";
        patterns += L"*." + e;
    }
    COMDLG_FILTERSPEC filters[] = {
        {L"可启动文件", patterns.c_str()},
        {L"所有文件", L"*.*"}
    };
    if(FAILED(dlg->SetFileTypes(2,filters))||FAILED(dlg->SetFileTypeIndex(1))){dlg->Release();return out;}

    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr)) && arr) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &item)) && item) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                        out.emplace_back(p);
                        CoTaskMemFree(p);
                    }
                    item->Release();
                }
            }
            arr->Release();
        }
    }
    dlg->Release();
    return out;
}

std::optional<std::wstring> pick_folder(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
    DWORD opts = 0;
    if(FAILED(dlg->GetOptions(&opts))||FAILED(dlg->SetOptions(opts|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST))){
        dlg->Release();
        return std::nullopt;
    }
    std::optional<std::wstring> out;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                out = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

/* STAMKY_CN_DETAIL
 * 快捷方式是任务栏身份的一部分，不能“IPersistFile::Save 返回成功就算完成”。这里先写临时 .lnk，
 * 设置 Target/Arguments/Description/AppUserModelID 后保存，再重新 Load 并回读关键字段做一致性校验，
 * 全部匹配才原子替换正式 .lnk。这样能尽早发现属性存储失败或异常 ShellLink 实现。
 */
bool create_app_shortcut(const std::wstring& linkPath, const std::wstring& arguments, const std::wstring& description, const std::wstring& appUserModelId) {
    const auto root=exe_dir();
    if(root.empty()||linkPath.empty())return false;
    std::wstring exeBuf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exeBuf.data(), static_cast<DWORD>(exeBuf.size()));
    if (!n || n >= exeBuf.size()) return false;
    exeBuf.resize(n);

    const std::filesystem::path finalPath(linkPath);
    if(finalPath.parent_path().empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec) return false;
    const auto tempPath = finalPath.parent_path() /
        (finalPath.stem().wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L".lnk");
    DeleteFileW(tempPath.c_str());

    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) || !sl) return false;
    bool ok = SUCCEEDED(sl->SetPath(exeBuf.c_str())) &&
              SUCCEEDED(sl->SetArguments(arguments.c_str())) &&
              SUCCEEDED(sl->SetWorkingDirectory(root.c_str())) &&
              SUCCEEDED(sl->SetDescription(description.c_str())) &&
              SUCCEEDED(sl->SetIconLocation(exeBuf.c_str(), 0));

    if (ok && !appUserModelId.empty()) {
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&store))) && store) {
            PROPVARIANT pv{};
            pv.vt = VT_LPWSTR;
            pv.pwszVal = const_cast<LPWSTR>(appUserModelId.c_str());
            ok = SUCCEEDED(store->SetValue(PKEY_AppUserModel_ID, pv)) && SUCCEEDED(store->Commit());
            store->Release();
        } else ok = false;
    }

    IPersistFile* pf = nullptr;
    if (ok && SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) && pf) {
        ok = SUCCEEDED(pf->Save(tempPath.c_str(), TRUE));
        pf->Release();
    } else ok = false;
    sl->Release();
    if (!ok) { DeleteFileW(tempPath.c_str()); return false; }

    IShellLinkW* verify = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&verify))) || !verify) {
        DeleteFileW(tempPath.c_str()); return false;
    }
    IPersistFile* vpf = nullptr;
    bool valid = false;
    if (SUCCEEDED(verify->QueryInterface(IID_PPV_ARGS(&vpf))) && vpf && SUCCEEDED(vpf->Load(tempPath.c_str(), STGM_READ))) {
        wchar_t target[32768]{};
        WIN32_FIND_DATAW fd{};
        wchar_t args[4096]{};
        valid = SUCCEEDED(verify->GetPath(target, static_cast<int>(_countof(target)), &fd, SLGP_RAWPATH)) &&
                SUCCEEDED(verify->GetArguments(args, static_cast<int>(_countof(args)))) &&
                _wcsicmp(target, exeBuf.c_str()) == 0 && arguments == args;
        if (valid && !appUserModelId.empty()) {
            IPropertyStore* store = nullptr;
            valid = SUCCEEDED(verify->QueryInterface(IID_PPV_ARGS(&store))) && store;
            if (valid) {
                PROPVARIANT pv{};
                if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv))) {
                    valid = pv.vt == VT_LPWSTR && pv.pwszVal && appUserModelId == pv.pwszVal;
                    PropVariantClear(&pv);
                } else valid = false;
                store->Release();
            }
        }
    }
    if (vpf) vpf->Release();
    verify->Release();
    if (!valid) { DeleteFileW(tempPath.c_str()); return false; }
    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

/* STAMKY_CN_DETAIL
 * 组快捷方式文件名来自用户组名，但真正身份由稳定 Group GUID 和 stamky.Group.<GUID> AUMID 决定。
 * 同名组通过安全后缀避免覆盖；旧 StackyModern/stackymorden 目标只作为迁移输入识别，任何新生成链接
 * 都必须指向 stamky.exe，避免品牌迁移后继续制造旧身份。
 */
bool create_group_shortcut(const Group& group, std::wstring* outPath) {
    ensure_directories();
    const auto root=exe_dir();
    if(root.empty())return false;
    std::wstring token = group.id;
    token.erase(std::remove_if(token.begin(), token.end(), [](wchar_t c){return c==L'{'||c==L'}';}), token.end());
    const std::wstring args = L"--show \"" + group.id + L"\"";
    const std::wstring appId = L"stamky.Group." + token;

    auto linkPath = root / L"GroupShortCuts" / (safe_filename(group.name) + L".lnk");
    if (path_exists(linkPath.wstring())) {
        IShellLinkW* existing = nullptr;
        bool same = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&existing))) && existing) {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(existing->QueryInterface(IID_PPV_ARGS(&pf))) && pf && SUCCEEDED(pf->Load(linkPath.c_str(), STGM_READ))) {
                wchar_t oldArgs[4096]{};
                same = SUCCEEDED(existing->GetArguments(oldArgs, static_cast<int>(_countof(oldArgs)))) && args == oldArgs;
            }
            if (pf) pf->Release();
            existing->Release();
        }
        if (!same) {
            std::wstring suffix=token.substr(0,std::min<size_t>(8,token.size()));
            linkPath=root/L"GroupShortCuts"/(safe_filename(group.name)+L"_"+safe_filename(suffix)+L".lnk");
        }
    }
    const bool ok = create_app_shortcut(linkPath.wstring(), args, tr(L"显示分组：") + group.name, appId);
    if (ok && outPath) *outPath = linkPath.wstring();
    return ok;
}

size_t migrate_legacy_group_shortcuts(const Model& model) {
    constexpr wchar_t kLegacyExe1[] = L"StackyModern.exe";
    constexpr wchar_t kLegacyExe2[] = L"stackymorden.exe";
    const auto root=exe_dir();
    if(root.empty())return 0;
    const auto dir=root/L"GroupShortCuts";
    std::error_code ec;
    if(!std::filesystem::exists(dir,ec)||ec)return 0;
    size_t migrated=0;
    for(std::filesystem::directory_iterator it(dir,std::filesystem::directory_options::skip_permission_denied,ec),end;!ec&&it!=end;it.increment(ec)){
        std::error_code statusEc;
        if(!it->is_regular_file(statusEc)||statusEc||lower_copy(it->path().extension().wstring())!=L".lnk")continue;
        IShellLinkW* link=nullptr;
        if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))||!link)continue;
        IPersistFile* pf=nullptr;
        bool legacy=false;
        std::wstring groupId;
        if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))&&pf&&SUCCEEDED(pf->Load(it->path().c_str(),STGM_READ))){
            wchar_t target[32768]{};WIN32_FIND_DATAW fd{};wchar_t args[4096]{};
            if(SUCCEEDED(link->GetPath(target,static_cast<int>(_countof(target)),&fd,SLGP_RAWPATH))&&
               SUCCEEDED(link->GetArguments(args,static_cast<int>(_countof(args))))){
                const std::wstring file=std::filesystem::path(target).filename().wstring();
                legacy=_wcsicmp(file.c_str(),kLegacyExe1)==0||_wcsicmp(file.c_str(),kLegacyExe2)==0;
                const std::wstring a=args;
                constexpr wchar_t prefix[]=L"--show \"";
                if(legacy&&a.rfind(prefix,0)==0&&a.size()>wcslen(prefix)&&a.back()==L'\"')groupId=a.substr(wcslen(prefix),a.size()-wcslen(prefix)-1);
            }
        }
        if(pf)pf->Release();link->Release();
        const Group* group=groupId.empty()?nullptr:model.find_group(groupId);
        if(!legacy||!group)continue;
        std::wstring token=group->id;
        token.erase(std::remove_if(token.begin(),token.end(),[](wchar_t c){return c==L'{'||c==L'}';}),token.end());
        const std::wstring appId=L"stamky.Group."+token;
        const std::wstring args=L"--show \""+group->id+L"\"";
        if(create_app_shortcut(it->path().wstring(),args,tr(L"显示分组：")+group->name,appId))++migrated;
    }
    return migrated;
}

void launch_path(const std::wstring& path) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

void launch_item(const Item& item) {
    if (item.type == ItemType::Group) return;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = item.path.c_str();
    sei.lpParameters = item.arguments.empty() ? nullptr : item.arguments.c_str();
    sei.lpDirectory = item.workingDirectory.empty() ? nullptr : item.workingDirectory.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

void open_containing_folder(const std::wstring& path) {
    if (path.empty()) return;
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(path), ec)) {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void show_file_properties(HWND owner, const std::wstring& path) {
    if (path.empty()) return;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.hwnd = owner;
    sei.lpVerb = L"properties";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

/* STAMKY_CN_DETAIL
 * Shell 原生右键菜单不是简单 TrackPopupMenu：扩展处理器会通过 owner HWND 收到 WM_INITMENUPOPUP、
 * WM_DRAWITEM、WM_MEASUREITEM、WM_MENUCHAR 等消息。因此 IContextMenu2/3 指针只在本次同步弹出期间
 * 暂存给全局转发器，TrackPopupMenuEx 返回后立即清空并 Release；禁止两个 Shell 菜单生命周期重叠。
 */
ShellContextMenuResult show_shell_context_menu(HWND owner, const std::wstring& path, POINT screenPoint) {
    if (g_contextMenu2 || g_contextMenu3) return ShellContextMenuResult::Failed;
    if (path.empty()) return ShellContextMenuResult::Failed;

    PIDLIST_ABSOLUTE absolute = nullptr;
    SFGAOF attrs = 0;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &absolute, 0, &attrs)) || !absolute) return ShellContextMenuResult::Failed;

    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(absolute, IID_PPV_ARGS(&parent), &child);
    if (FAILED(hr) || !parent || !child) {
        if (parent) parent->Release();
        CoTaskMemFree(absolute);
        return ShellContextMenuResult::Failed;
    }

    LPCITEMIDLIST items[1] = {child};
    IContextMenu* context = nullptr;
    hr = parent->GetUIObjectOf(owner, 1, items, IID_IContextMenu, nullptr, reinterpret_cast<void**>(&context));
    if (FAILED(hr) || !context) {
        parent->Release();
        CoTaskMemFree(absolute);
        return ShellContextMenuResult::Failed;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        context->Release();
        parent->Release();
        CoTaskMemFree(absolute);
        return ShellContextMenuResult::Failed;
    }

    constexpr UINT kFirst = 1;
    constexpr UINT kLast = 0x7FFF;
    hr = context->QueryContextMenu(menu, 0, kFirst, kLast, CMF_NORMAL | CMF_EXPLORE);
    if (FAILED(hr)) {
        DestroyMenu(menu);
        context->Release();
        parent->Release();
        CoTaskMemFree(absolute);
        return ShellContextMenuResult::Failed;
    }

    IContextMenu3* cm3 = nullptr;
    IContextMenu2* cm2 = nullptr;
    context->QueryInterface(IID_PPV_ARGS(&cm3));
    if (!cm3) context->QueryInterface(IID_PPV_ARGS(&cm2));
    g_contextMenu3 = cm3;
    g_contextMenu2 = cm2;

    SetForegroundWindow(owner);
    const UINT cmd = TrackPopupMenuEx(menu,
                                      TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RECURSE,
                                      screenPoint.x,
                                      screenPoint.y,
                                      owner,
                                      nullptr);

    g_contextMenu3 = nullptr;
    g_contextMenu2 = nullptr;

    ShellContextMenuResult result = ShellContextMenuResult::Dismissed;
    if (cmd >= kFirst) {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
        invoke.hwnd = owner;
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - kFirst);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - kFirst);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        result = SUCCEEDED(context->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke)))
                     ? ShellContextMenuResult::Invoked
                     : ShellContextMenuResult::Failed;
    }

    if (cm3) cm3->Release();
    if (cm2) cm2->Release();
    DestroyMenu(menu);
    context->Release();
    parent->Release();
    CoTaskMemFree(absolute);
    PostMessageW(owner, WM_NULL, 0, 0);
    return result;
}

bool handle_shell_context_menu_message(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* result) {
    if (!is_context_forward_message(msg) || (!g_contextMenu3 && !g_contextMenu2)) return false;

    LRESULT local = 0;
    HRESULT hr = E_FAIL;
    if (g_contextMenu3) {
        hr = g_contextMenu3->HandleMenuMsg2(msg, wParam, lParam, &local);
    } else if (g_contextMenu2) {
        hr = g_contextMenu2->HandleMenuMsg(msg, wParam, lParam);
    }
    if (FAILED(hr)) return false;
    if (result) *result = local;
    return true;
}

/* STAMKY_CN_DETAIL
 * AppUserModelID 通过 SHGetPropertyStoreForWindow 写入 PKEY_AppUserModel_ID。它影响任务栏分组/固定身份，
 * 所以新窗口只使用 stamky 命名空间；旧 AUMID 不作为输出回退。PROPVARIANT 由 InitPropVariantFromString
 * 初始化后必须 PropVariantClear，IPropertyStore 则由 ComPtr/显式 Release 生命周期管理。
 */
bool set_window_app_user_model_id(HWND hwnd, const std::wstring& appUserModelId) {
    if (!hwnd || appUserModelId.empty()) return false;
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store) return false;

    PROPVARIANT pv{};
    pv.vt = VT_LPWSTR;
    pv.pwszVal = const_cast<LPWSTR>(appUserModelId.c_str());
    const bool ok = SUCCEEDED(store->SetValue(PKEY_AppUserModel_ID, pv)) && SUCCEEDED(store->Commit());
    store->Release();
    return ok;
}

}
