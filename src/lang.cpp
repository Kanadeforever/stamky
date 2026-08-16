/*
 * stamky / lang.cpp
 *
 * 简单、可用户编辑的 INI 式语言层。源代码中的中文原文就是稳定 key，data\lang\<code>.ini
 * 的 [UI] 段提供覆盖值；缺键始终回退到中文 key，因此第三方语言文件可以逐步翻译而不需要
 * 与程序版本完全锁步。首次释放 cn/en 默认文件只在目标不存在时原子创建，绝不覆盖用户修改。
 *
 * 编码/格式边界：语言文件限制体积并要求合法 UTF-8；语言代码只允许安全的字母数字、下划线
 * 和连字符，防止把配置值解释成任意相对路径。INI 为单行键值格式，字面“\n”在加载时同时
 * 对 key/value 还原为真实换行，使源码中的多行 tr() 与文件中的单行表示能稳定匹配。
 *
 * 品牌迁移：加载旧语言文件时可把 StackyModern/stackymorden 键和值迁移为 stamky；
 * Stacky/Stahky 的上游致谢不是旧品牌，必须原样保留。
 */

#include "lang.h"
#include <fstream>
#include <sstream>

namespace sm {
namespace {
std::filesystem::path lang_dir() { const auto dir=data_dir(); return dir.empty()?std::filesystem::path{}:dir/L"lang"; }

bool is_valid_language_code(const std::wstring& code) {
    if(code.empty()||code.size()>32)return false;
    return std::all_of(code.begin(),code.end(),[](wchar_t c){
        return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_';
    });
}

std::wstring normalize_language_code(std::wstring code) {
    if(!is_valid_language_code(code))return L"cn";
    return lower_copy(std::move(code));
}

void migrate_legacy_brand_text(std::wstring& text) {
    constexpr const wchar_t* legacy[] = {L"StackyModern", L"stackymorden"};
    for (const wchar_t* old : legacy) {
        size_t pos=0;
        const size_t len=wcslen(old);
        while ((pos=text.find(old,pos))!=std::wstring::npos) { text.replace(pos,len,L"stamky"); pos+=6; }
    }
}

void expand_ini_newlines(std::wstring& text) {
    size_t pos=0;
    while((pos=text.find(L"\\n",pos))!=std::wstring::npos){
        text.replace(pos,2,L"\n");
        ++pos;
    }
}

/* STAMKY_CN_DETAIL
 * 默认语言文件只负责首次释放，绝不能覆盖用户已经修改/翻译的 ini。实现先检查目标存在性，
 * 写同目录临时文件后再以“不覆盖”语义移动；若并发实例恰好先创建正式文件，本实例丢弃临时文件即可。
 */
void write_file_if_missing(const std::filesystem::path& p, const wchar_t* content) {
    std::error_code ec;
    const bool exists=std::filesystem::exists(p,ec);
    if(ec||exists)return;
    std::filesystem::path tmp=p;tmp+=L".tmp";
    std::ofstream f(tmp,std::ios::binary|std::ios::trunc);
    if(!f)return;
    const unsigned char bom[3]={0xEF,0xBB,0xBF};
    f.write(reinterpret_cast<const char*>(bom),3);
    const std::wstring source=content?content:L"";
    const auto u=wide_to_utf8(source);
    if(!source.empty()&&u.empty()){f.close();DeleteFileW(tmp.c_str());return;}
    if(!u.empty())f.write(u.data(),static_cast<std::streamsize>(u.size()));
    f.flush();
    const bool good=f.good();
    f.close();
    if(!good){DeleteFileW(tmp.c_str());return;}
    // 默认语言文件只允许“首次创建”，绝不覆盖用户已经修改/并发创建的版本。
    if(!MoveFileW(tmp.c_str(),p.c_str()))DeleteFileW(tmp.c_str());
}
}

Language& lang() {
    static Language instance;
    return instance;
}

void Language::ensure_default_files() {
    ensure_directories();
    const auto dir=lang_dir();
    if(dir.empty())return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if(ec)return;

    // cn.ini：键=中文原文，值=中文（可自行修改措辞）；en.ini：键=中文原文，值=英文。
    // 键清单与代码中 tr() 调用保持一致；漏掉的键自动回退中文原文，不影响显示。
    // 值内换行用字面 \n 表示（ini 键值必须单行），加载时自动转为真实换行。
    write_file_if_missing(dir / L"cn.ini",
        L"[UI]\r\n"
        L"stamky=stamky\r\n"
        L"打开内容管理=打开内容管理\r\n"
        L"设置=设置\r\n"
        L"重建图标与运行缓存=重建图标与运行缓存\r\n"
        L"性能状态=性能状态\r\n"
        L"关于=关于\r\n"
        L"退出=退出\r\n"
        L"stamky - 启动失败=stamky - 启动失败\r\n"
        L"stamky - 性能状态=stamky - 性能状态\r\n"
        L"stamky - 设置=stamky - 设置\r\n"
        L"stamky - 内容管理=stamky - 内容管理\r\n"
        L"stamky - 组快捷方式=stamky - 组快捷方式\r\n"
        L"stamky - 运行缓存更新失败=stamky - 运行缓存更新失败\r\n"
        L"外观与常规=外观与常规\r\n"
        L"菜单文字大小（pt）=菜单文字大小（pt）\r\n"
        L"菜单图标大小（DIP）=菜单图标大小（DIP）\r\n"
        L"可启动文件格式=可启动文件格式\r\n"
        L"编辑格式…=编辑格式…\r\n"
        L"界面语言=界面语言\r\n"
        L"多列之间显示分隔线=多列之间显示分隔线\r\n"
        L"菜单列数不再人为限制：程序会根据当前显示器工作区、文字和图标尺寸自动决定。极端超长列表仅在宽度也不足时启用原生滚动。=菜单列数不再人为限制：程序会根据当前显示器工作区、文字和图标尺寸自动决定。极端超长列表仅在宽度也不足时启用原生滚动。\r\n"
        L"保存=保存\r\n"
        L"取消=取消\r\n"
        L"确定=确定\r\n"
        L"保存设置失败。请检查 data 目录是否可写。=保存设置失败。请检查 data 目录是否可写。\r\n"
        L"每行一个格式名（如 lnk、jar）。点 + 新建（输入后回车）；选中后按钮变 -，点击或按 Delete 删除，支持 Ctrl/Shift 多选。=每行一个格式名（如 lnk、jar）。点 + 新建（输入后回车）；选中后按钮变 -，点击或按 Delete 删除，支持 Ctrl/Shift 多选。\r\n"
        L"分组=分组\r\n"
        L"项目=项目\r\n"
        L"名称=名称\r\n"
        L"类型=类型\r\n"
        L"路径 / 目标=路径 / 目标\r\n"
        L"来源 / 状态=来源 / 状态\r\n"
        L"搜索名称或路径=搜索名称或路径\r\n"
        L"＋ 新建=＋ 新建\r\n"
        L"重命名=重命名\r\n"
        L"删除=删除\r\n"
        L"预览=预览\r\n"
        L"＋ 添加...=＋ 添加...\r\n"
        L"扫描文件夹=扫描文件夹\r\n"
        L"编辑=编辑\r\n"
        L"创建组快捷方式=创建组快捷方式\r\n"
        L"（分组不存在）=（分组不存在）\r\n"
        L"内部=内部\r\n"
        L"手动=手动\r\n"
        L"扫描=扫描\r\n"
        L" · 缺失= · 缺失\r\n"
        L"没有分组=没有分组\r\n"
        L" 个项目= 个项目\r\n"
        L"  ·  当前显示 =  ·  当前显示 \r\n"
        L" 个= 个\r\n"
        L"     拖放：单文件→当前组；文件夹/多对象拖到左侧→新组；拖到右侧→加入当前组；双击启动；F2 编辑；Delete 删除；Alt+↑/↓ 调整顺序=     拖放：单文件→当前组；文件夹/多对象拖到左侧→新组；拖到右侧→加入当前组；双击启动；F2 编辑；Delete 删除；Alt+↑/↓ 调整顺序\r\n"
        L"保存配置失败。请检查 data 目录是否可写。=保存配置失败。请检查 data 目录是否可写。\r\n"
        L"添加程序 / 快捷方式 / 文件...=添加程序 / 快捷方式 / 文件...\r\n"
        L"添加文件夹...=添加文件夹...\r\n"
        L"自定义项目...=自定义项目...\r\n"
        L"添加现有分组为子组...=添加现有分组为子组...\r\n"
        L"添加现有分组为子组=添加现有分组为子组\r\n"
        L"没有可添加的分组=没有可添加的分组\r\n"
        L"扫描完成：新增 =扫描完成：新增 \r\n"
        L" 个项目。\\n\\n已有项目顺序保持不变；新项目追加到列表末尾。= 个项目。\\n\\n已有项目顺序保持不变；新项目追加到列表末尾。\r\n"
        L"扫描快捷方式=扫描快捷方式\r\n"
        L"扫描文件夹失败或在读取过程中中断。未导入任何项目。=扫描文件夹失败或在读取过程中中断。未导入任何项目。\r\n"
        L"没有可添加的分组（已排除当前组和会形成循环的分组）。=没有可添加的分组（已排除当前组和会形成循环的分组）。\r\n"
        L"删除选中的项目？\\n不会删除原始文件。=删除选中的项目？\\n不会删除原始文件。\r\n"
        L"删除当前项目？\\n不会删除原始文件。=删除当前项目？\\n不会删除原始文件。\r\n"
        L"子组项目的名称来自目标分组。请在左侧重命名目标分组。=子组项目的名称来自目标分组。请在左侧重命名目标分组。\r\n"
        L"图标已刷新，但运行缓存更新失败。=图标已刷新，但运行缓存更新失败。\r\n"
        L"没有从该项目取得可用图标，将继续使用通用图标。=没有从该项目取得可用图标，将继续使用通用图标。\r\n"
        L"搜索过滤时不调整真实顺序。请先清空搜索。=搜索过滤时不调整真实顺序。请先清空搜索。\r\n"
        L"新分组=新分组\r\n"
        L"至少保留一个分组。=至少保留一个分组。\r\n"
        L"删除当前分组？\\n\\n不会删除任何原始文件；指向这个分组的子组项目会显示为失效。=删除当前分组？\\n\\n不会删除任何原始文件；指向这个分组的子组项目会显示为失效。\r\n"
        L"组快捷方式已经创建并通过 ShellLink 回读校验：\\n\\n=组快捷方式已经创建并通过 ShellLink 回读校验：\\n\\n\r\n"
        L"\\n\\n请先双击确认能够弹出正确分组；确认后再固定到任务栏。旧版本生成的组快捷方式请删除后重新创建。=\\n\\n请先双击确认能够弹出正确分组；确认后再固定到任务栏。旧版本生成的组快捷方式请删除后重新创建。\r\n"
        L"创建或校验组快捷方式失败。\\n\\n程序不会保留未通过 ShellLink 回读校验的 .lnk。=创建或校验组快捷方式失败。\\n\\n程序不会保留未通过 ShellLink 回读校验的 .lnk。\r\n"
        L"启动\tEnter=启动\tEnter\r\n"
        L"编辑\tF2=编辑\tF2\r\n"
        L"打开链接=打开链接\r\n"
        L"打开所在位置=打开所在位置\r\n"
        L"刷新图标=刷新图标\r\n"
        L"Windows 属性=Windows 属性\r\n"
        L"删除\tDelete=删除\tDelete\r\n"
        L"预览菜单=预览菜单\r\n"
        L"重命名\tF2=重命名\tF2\r\n"
        L"新建分组=新建分组\r\n"
        L"删除分组=删除分组\r\n"
        L"添加项目=添加项目\r\n"
        L"项目属性=项目属性\r\n"
        L"显示名称=显示名称\r\n"
        L"路径 / URL=路径 / URL\r\n"
        L"浏览…=浏览…\r\n"
        L"启动参数（可选）=启动参数（可选）\r\n"
        L"工作目录（可选）=工作目录（可选）\r\n"
        L"自定义图标（可选）=自定义图标（可选）\r\n"
        L"清除=清除\r\n"
        L"名称和路径不能为空。=名称和路径不能为空。\r\n"
        L"图标与程序=图标与程序\r\n"
        L"所有文件=所有文件\r\n"
        L"（子组层级过深）=（子组层级过深）\r\n"
        L"（空）=（空）\r\n"
        L"（文件夹层级过深）=（文件夹层级过深）\r\n"
        L"（文件夹路径为空）=（文件夹路径为空）\r\n"
        L"（空文件夹）=（空文件夹）\r\n"
        L"（无法访问）=（无法访问）\r\n"
        L"（文件夹读取中断）=（文件夹读取中断）\r\n"
        L"（该文件夹内没有可启动文件）=（该文件夹内没有可启动文件）\r\n"
        L"（还有 =（还有 \r\n"
        L" 项…，点击打开文件夹）= 项…，点击打开文件夹）\r\n"
        L"（文件夹不可访问）=（文件夹不可访问）\r\n"
        L"程序=程序\r\n"
        L"文件夹=文件夹\r\n"
        L"网址=网址\r\n"
        L"文件=文件\r\n"
        L"子组=子组\r\n"
        L"常用=常用\r\n"
        L"显示分组：=显示分组：\r\n"
        L"未命名=未命名\r\n"
        L"stamky 无法创建后台 Host 窗口。\\n\\n请把 data\\启动诊断.log 发回用于定位。=stamky 无法创建后台 Host 窗口。\\n\\n请把 data\\启动诊断.log 发回用于定位。\r\n"
        L"分组数据无法安全读取。程序已停止启动，并且不会覆盖现有 groups.tsv。\\n\\n请检查 data\\groups.tsv 和 data\\启动诊断.log。=分组数据无法安全读取。程序已停止启动，并且不会覆盖现有 groups.tsv。\\n\\n请检查 data\\groups.tsv 和 data\\启动诊断.log。\r\n"
        L"运行缓存无法构建或读取。\\n\\n请检查磁盘可写空间以及 data\\启动诊断.log。=运行缓存无法构建或读取。\\n\\n请检查磁盘可写空间以及 data\\启动诊断.log。\r\n"
        L"无法读取当前进程的内存统计。=无法读取当前进程的内存统计。\r\n"
        L"找不到该分组，可能快捷方式已经过期。=找不到该分组，可能快捷方式已经过期。\r\n"
        L"缓存重建完成。=缓存重建完成。\r\n"
        L"缓存重建失败，请查看 data\\启动诊断.log。=缓存重建失败，请查看 data\\启动诊断.log。\r\n"
        L"Working Set：%s MB\\nPrivate Commit：%s MB\\n最近一次菜单准备：%s ms\\n\\n管理窗口关闭后，其 ListView、图像列表和字体资源会销毁；Runtime 图标只保留小型 LRU。=Working Set：%s MB\\nPrivate Commit：%s MB\\n最近一次菜单准备：%s ms\\n\\n管理窗口关闭后，其 ListView、图像列表和字体资源会销毁；Runtime 图标只保留小型 LRU。\r\n"
        L"内容已经保存，但运行缓存更新失败。\\n\\n为了避免出现“管理器里有项目、弹出菜单却是旧内容”的情况，请不要继续使用旧菜单；可从托盘选择“重建图标与运行缓存”，并查看 data\\启动诊断.log。=内容已经保存，但运行缓存更新失败。\\n\\n为了避免出现“管理器里有项目、弹出菜单却是旧内容”的情况，请不要继续使用旧菜单；可从托盘选择“重建图标与运行缓存”，并查看 data\\启动诊断.log。\r\n"
        L"关于 stamky=关于 stamky\r\n"
        L"stamky  v0.9.3\\n\\nby Luminous 20260816\\n\\n轻量级 Windows 任务栏分组启动器：点击即出、低内存常驻。\\n以开源项目 Stacky 为灵感，借鉴了Stahky的部分设计、按个人需求重构的现代版本。\\n\\n原生 C++/Win32 实现，无第三方运行时依赖。\\n\\n致谢：\\n\\nstacky    https://github.com/pawelt/stacky\\n\\nstahky    https://github.com/joedf/stahky\\n=stamky  v0.9.3\\n\\nby Luminous 20260816\\n\\n轻量级 Windows 任务栏分组启动器：点击即出、低内存常驻。\\n以开源项目 Stacky 为灵感，借鉴了Stahky的部分设计、按个人需求重构的现代版本。\\n\\n原生 C++/Win32 实现，无第三方运行时依赖。\\n\\n致谢：\\n\\nstacky    https://github.com/pawelt/stacky\\n\\nstahky    https://github.com/joedf/stahky\\n\r\n");

    write_file_if_missing(dir / L"en.ini",
        L"[UI]\r\n"
        L"stamky=stamky\r\n"
        L"打开内容管理=Open Content Manager\r\n"
        L"设置=Settings\r\n"
        L"重建图标与运行缓存=Rebuild Icons & Cache\r\n"
        L"性能状态=Performance\r\n"
        L"关于=About\r\n"
        L"退出=Exit\r\n"
        L"stamky - 启动失败=stamky - Startup Failed\r\n"
        L"stamky - 性能状态=stamky - Performance\r\n"
        L"stamky - 设置=stamky - Settings\r\n"
        L"stamky - 内容管理=stamky - Content Manager\r\n"
        L"stamky - 组快捷方式=stamky - Group Shortcut\r\n"
        L"stamky - 运行缓存更新失败=stamky - Cache Update Failed\r\n"
        L"外观与常规=Appearance\r\n"
        L"菜单文字大小（pt）=Menu font size (pt)\r\n"
        L"菜单图标大小（DIP）=Menu icon size (DIP)\r\n"
        L"可启动文件格式=Launchable File Formats\r\n"
        L"编辑格式…=Edit Formats...\r\n"
        L"界面语言=Language\r\n"
        L"多列之间显示分隔线=Show column separators\r\n"
        L"菜单列数不再人为限制：程序会根据当前显示器工作区、文字和图标尺寸自动决定。极端超长列表仅在宽度也不足时启用原生滚动。=Columns are automatic: based on the monitor work area, font and icon size. Native scrolling is used only when neither width nor height fits.\r\n"
        L"保存=Save\r\n"
        L"取消=Cancel\r\n"
        L"确定=OK\r\n"
        L"保存设置失败。请检查 data 目录是否可写。=Failed to save settings. Check that the data directory is writable.\r\n"
        L"每行一个格式名（如 lnk、jar）。点 + 新建（输入后回车）；选中后按钮变 -，点击或按 Delete 删除，支持 Ctrl/Shift 多选。=One format per line (e.g. lnk, jar). Click + to add (press Enter to confirm); when selected the button becomes - for removal; Delete key also removes; Ctrl/Shift multi-select supported.\r\n"
        L"分组=Groups\r\n"
        L"项目=Items\r\n"
        L"名称=Name\r\n"
        L"类型=Type\r\n"
        L"路径 / 目标=Path / Target\r\n"
        L"来源 / 状态=Source / Status\r\n"
        L"搜索名称或路径=Search name or path\r\n"
        L"＋ 新建=＋ New\r\n"
        L"重命名=Rename\r\n"
        L"删除=Delete\r\n"
        L"预览=Preview\r\n"
        L"＋ 添加...=＋ Add...\r\n"
        L"扫描文件夹=Scan Folder\r\n"
        L"编辑=Edit\r\n"
        L"创建组快捷方式=Create Group Shortcut\r\n"
        L"（分组不存在）=(missing group)\r\n"
        L"内部=internal\r\n"
        L"手动=manual\r\n"
        L"扫描=scan\r\n"
        L" · 缺失= · missing\r\n"
        L"没有分组=No groups\r\n"
        L" 个项目= items\r\n"
        L"  ·  当前显示 = · showing \r\n"
        L" 个=\r\n"
        L"     拖放：单文件→当前组；文件夹/多对象拖到左侧→新组；拖到右侧→加入当前组；双击启动；F2 编辑；Delete 删除；Alt+↑/↓ 调整顺序=     Drop: one file -> current group; folder/multiple items on left -> new group; right list -> add to current group; double-click launch; F2 edit; Delete remove; Alt+Up/Down reorder\r\n"
        L"保存配置失败。请检查 data 目录是否可写。=Failed to save config. Check that the data directory is writable.\r\n"
        L"添加程序 / 快捷方式 / 文件...=Add Program / Shortcut / File...\r\n"
        L"添加文件夹...=Add Folder...\r\n"
        L"自定义项目...=Custom Item...\r\n"
        L"添加现有分组为子组...=Add Existing Group as Subgroup...\r\n"
        L"添加现有分组为子组=Add Existing Group as Subgroup\r\n"
        L"没有可添加的分组=No groups available\r\n"
        L"扫描完成：新增 =Scan done: \r\n"
        L" 个项目。\\n\\n已有项目顺序保持不变；新项目追加到列表末尾。= item(s) added.\\n\\nExisting items keep their order; new items are appended.\r\n"
        L"扫描快捷方式=Scan Shortcuts\r\n"
        L"扫描文件夹失败或在读取过程中中断。未导入任何项目。=Folder scan failed or was interrupted while reading. No items were imported.\r\n"
        L"没有可添加的分组（已排除当前组和会形成循环的分组）。=No group available (current group and cycle-forming groups excluded).\r\n"
        L"删除选中的项目？\\n不会删除原始文件。=Delete selected items?\\nOriginal files are not removed.\r\n"
        L"删除当前项目？\\n不会删除原始文件。=Delete current item?\\nOriginal files are not removed.\r\n"
        L"子组项目的名称来自目标分组。请在左侧重命名目标分组。=The subgroup item name comes from the target group. Rename it in the left list.\r\n"
        L"图标已刷新，但运行缓存更新失败。=Icon refreshed, but the runtime cache update failed.\r\n"
        L"没有从该项目取得可用图标，将继续使用通用图标。=No icon was obtained from this item; a generic icon will be used.\r\n"
        L"搜索过滤时不调整真实顺序。请先清空搜索。=Cannot reorder while search filtering is active. Clear the search first.\r\n"
        L"新分组=New Group\r\n"
        L"至少保留一个分组。=Keep at least one group.\r\n"
        L"删除当前分组？\\n\\n不会删除任何原始文件；指向这个分组的子组项目会显示为失效。=Delete current group?\\n\\nNo original files are removed; subgroup items pointing to it will show as invalid.\r\n"
        L"组快捷方式已经创建并通过 ShellLink 回读校验：\\n\\n=Group shortcut created and verified via ShellLink re-read:\\n\\n\r\n"
        L"\\n\\n请先双击确认能够弹出正确分组；确认后再固定到任务栏。旧版本生成的组快捷方式请删除后重新创建。=\\n\\nDouble-click to confirm it opens the right group before pinning. Delete and recreate shortcuts generated by old versions.\r\n"
        L"创建或校验组快捷方式失败。\\n\\n程序不会保留未通过 ShellLink 回读校验的 .lnk。=Failed to create or verify the group shortcut.\\n\\nLinks that fail ShellLink re-read are not kept.\r\n"
        L"启动\tEnter=Launch\tEnter\r\n"
        L"编辑\tF2=Edit\tF2\r\n"
        L"打开链接=Open Link\r\n"
        L"打开所在位置=Open Location\r\n"
        L"刷新图标=Refresh Icon\r\n"
        L"Windows 属性=Windows Properties\r\n"
        L"删除\tDelete=Delete\tDelete\r\n"
        L"预览菜单=Preview Menu\r\n"
        L"重命名\tF2=Rename\tF2\r\n"
        L"新建分组=New Group\r\n"
        L"删除分组=Delete Group\r\n"
        L"添加项目=Add Item\r\n"
        L"项目属性=Item Properties\r\n"
        L"显示名称=Display Name\r\n"
        L"路径 / URL=Path / URL\r\n"
        L"浏览…=Browse...\r\n"
        L"启动参数（可选）=Arguments (optional)\r\n"
        L"工作目录（可选）=Working Directory (optional)\r\n"
        L"自定义图标（可选）=Custom Icon (optional)\r\n"
        L"清除=Clear\r\n"
        L"名称和路径不能为空。=Name and path cannot be empty.\r\n"
        L"图标与程序=Icons & Programs\r\n"
        L"所有文件=All Files\r\n"
        L"（子组层级过深）=(subgroup too deep)\r\n"
        L"（空）=(empty)\r\n"
        L"（文件夹层级过深）=(folder too deep)\r\n"
        L"（文件夹路径为空）=(empty folder path)\r\n"
        L"（空文件夹）=(empty folder)\r\n"
        L"（无法访问）=(inaccessible)\r\n"
        L"（文件夹读取中断）=(folder read interrupted)\r\n"
        L"（该文件夹内没有可启动文件）=(no launchable files)\r\n"
        L"（还有 =(... \r\n"
        L" 项…，点击打开文件夹）= more item(s)... click to open folder)\r\n"
        L"（文件夹不可访问）=(folder inaccessible)\r\n"
        L"程序=Program\r\n"
        L"文件夹=Folder\r\n"
        L"网址=URL\r\n"
        L"文件=File\r\n"
        L"子组=Subgroup\r\n"
        L"常用=Common\r\n"
        L"显示分组：=Show group: \r\n"
        L"未命名=Untitled\r\n"
        L"stamky 无法创建后台 Host 窗口。\\n\\n请把 data\\启动诊断.log 发回用于定位。=stamky could not create the background Host window.\\n\\nPlease send data\\启动诊断.log back for diagnosis.\r\n"
        L"分组数据无法安全读取。程序已停止启动，并且不会覆盖现有 groups.tsv。\\n\\n请检查 data\\groups.tsv 和 data\\启动诊断.log。=Group data could not be read safely. Startup has stopped and the existing groups.tsv will not be overwritten.\\n\\nCheck data\\groups.tsv and data\\启动诊断.log.\r\n"
        L"运行缓存无法构建或读取。\\n\\n请检查磁盘可写空间以及 data\\启动诊断.log。=The runtime cache could not be built or read.\\n\\nCheck available disk space, write access, and data\\启动诊断.log.\r\n"
        L"无法读取当前进程的内存统计。=Unable to read memory statistics for the current process.\r\n"
        L"找不到该分组，可能快捷方式已经过期。=Group not found. The shortcut may be outdated.\r\n"
        L"缓存重建完成。=Cache rebuilt.\r\n"
        L"缓存重建失败，请查看 data\\启动诊断.log。=Cache rebuild failed. See data\\启动诊断.log.\r\n"
        L"Working Set：%s MB\\nPrivate Commit：%s MB\\n最近一次菜单准备：%s ms\\n\\n管理窗口关闭后，其 ListView、图像列表和字体资源会销毁；Runtime 图标只保留小型 LRU。=Working Set: %s MB\\nPrivate Commit: %s MB\\nLast menu prepare: %s ms\\n\\nThe manager releases its ListView, image list and fonts when closed; Runtime keeps only a small LRU.\r\n"
        L"内容已经保存，但运行缓存更新失败。\\n\\n为了避免出现“管理器里有项目、弹出菜单却是旧内容”的情况，请不要继续使用旧菜单；可从托盘选择“重建图标与运行缓存”，并查看 data\\启动诊断.log。=Saved, but the runtime cache update failed.\\n\\nTo avoid “items in the manager but old menu contents”, stop using the old menu; pick tray “Rebuild Icons & Cache” and check data\\启动诊断.log.\r\n"
        L"关于 stamky=About stamky\r\n"
        L"stamky  v0.9.3\\n\\nby Luminous 20260816\\n\\n轻量级 Windows 任务栏分组启动器：点击即出、低内存常驻。\\n以开源项目 Stacky 为灵感，借鉴了Stahky的部分设计、按个人需求重构的现代版本。\\n\\n原生 C++/Win32 实现，无第三方运行时依赖。\\n\\n致谢：\\n\\nstacky    https://github.com/pawelt/stacky\\n\\nstahky    https://github.com/joedf/stahky\\n=stamky  v0.9.3\\n\\nby Luminous 20260816\\n\\nA lightweight Windows taskbar group launcher: instant popup, low memory footprint.\\nInspired by the open-source project Stacky, borrowing parts of Stahky's design, rebuilt to personal needs.\\n\\nNative C++/Win32, no third-party runtime dependencies.\\n\\nThanks:\\n\\nstacky    https://github.com/pawelt/stacky\\n\\nstahky    https://github.com/joedf/stahky\\n\r\n");
}

/* STAMKY_CN_DETAIL
 * 语言文件同样按不可信外部文本处理：语言代码先做文件名安全过滤，文件大小受限、读取必须完整、UTF-8
 * 必须严格合法。INI 的 key/value 都会把字面 \n 展开为真实换行；key 也必须展开，否则源码中的多行 tr()
 * 永远匹配不到磁盘中单行编码的键。旧品牌只在此处做兼容迁移，不再输出为正式 stamky 文案。
 */
void Language::load(const std::wstring& code) {
    code_=normalize_language_code(code);
    map_.clear();
    const auto dir=lang_dir();
    if(dir.empty())return;
    const auto file=dir/(code_+L".ini");
    // 翻译文件是 UTF-8 文本；限制体积并要求完整读取，避免损坏/异常文件导致巨额分配。
    constexpr std::uintmax_t kMaxLanguageBytes=16ull*1024ull*1024ull;
    std::error_code ec;
    const auto fileSize=std::filesystem::file_size(file,ec);
    if(ec||fileSize==0||fileSize>kMaxLanguageBytes||fileSize>static_cast<std::uintmax_t>(std::numeric_limits<size_t>::max()))return;
    std::ifstream f(file,std::ios::binary);
    if(!f)return;
    std::string bytes(static_cast<size_t>(fileSize),'\0');
    if(!f.read(bytes.data(),static_cast<std::streamsize>(bytes.size())))return;
    if(bytes.size()>=3&&static_cast<unsigned char>(bytes[0])==0xEF&&
       static_cast<unsigned char>(bytes[1])==0xBB&&static_cast<unsigned char>(bytes[2])==0xBF)bytes.erase(0,3);
    const std::wstring text=utf8_to_wide(bytes);
    if(text.empty()&&!bytes.empty())return;
    std::wistringstream ss(text);
    std::wstring line;
    bool inUi=false;
    while(std::getline(ss,line)){
        if(!line.empty()&&line.back()==L'\r')line.pop_back();
        if(line.empty()||line[0]==L'#')continue;
        if(line[0]==L'['){inUi=(line==L"[UI]");continue;}
        if(!inUi)continue;
        const size_t eq=line.find(L'=');
        if(eq==std::wstring::npos)continue;
        std::wstring key=line.substr(0,eq);
        std::wstring value=line.substr(eq+1);
        expand_ini_newlines(key);
        migrate_legacy_brand_text(key);
        migrate_legacy_brand_text(value);
        map_[std::move(key)]=std::move(value);
    }
}

/* STAMKY_CN_DETAIL
 * t() 查询前对源码 key 做与加载器完全相同的换行规范化，然后查 map；未命中时原样返回英文/源码字符串。
 * 这种 fallback 保证语言文件缺键不会显示空白，同时静态检查脚本负责确保内置 cn/en 的实际 tr() 覆盖为 0 缺失。
 */
std::wstring Language::t(const wchar_t* text) const {
    if (!text) return {};
    std::wstring key=text;
    expand_ini_newlines(key);
    auto it = map_.find(key);
    std::wstring out = (it != map_.end() && !it->second.empty()) ? it->second : text;
    // 兼容调用点直接写真实换行和历史代码使用字面 \n 两种形式；最终统一返回真实换行。
    expand_ini_newlines(out);
    return out;
}

std::vector<std::wstring> Language::available_languages() const {
    std::vector<std::wstring> out;
    const auto dir=lang_dir();
    if(dir.empty())return out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir,ec),end;
    while(!ec&&it!=end){
        const auto de=*it;
        std::error_code statusEc;
        if(de.is_regular_file(statusEc)&&!statusEc&&lower_copy(de.path().extension().wstring())==L".ini"){
            auto stem=de.path().stem().wstring();
            if(is_valid_language_code(stem)){
                stem=lower_copy(std::move(stem));
                if(std::find(out.begin(),out.end(),stem)==out.end())out.push_back(std::move(stem));
            }
        }
        it.increment(ec);
    }
    std::sort(out.begin(),out.end());
    return out;
}

}
