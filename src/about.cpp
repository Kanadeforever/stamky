/*
 * stamky / about.cpp
 *
 * 模块职责：构造并显示“关于”对话框。这里故意只承载产品说明、版本展示和上游致谢，
 * 不参与主窗口、缓存或配置状态管理，避免一个纯展示入口意外改变运行时状态。
 *
 * Win32 约束：MessageBoxW 是同步模态调用，会在当前线程内部运行系统自己的消息循环；
 * 因此传入的 owner 必须是仍然有效的顶层窗口句柄。显示期间对象不跨调用保存 HWND，
 * 也不把对话框句柄缓存到全局状态。所有文本均使用宽字符 API，避免系统代码页影响中文。
 *
 * 维护注意：Stacky / Stahky 是本项目明确保留的上游项目名称与致谢，不属于 stamky
 * 正式改名时应替换的旧品牌字符串。版本号在发行阶段应与 CMake、资源文件、manifest、
 * 版本说明同步核对。
 */

#include "about.h"
#include "lang.h"

namespace sm {
/* STAMKY_CN_DETAIL
 * 关于窗口是纯展示路径，不持有长期资源；版本与上游 Stacky/Stahky 致谢在这里保持可见。
 * 上游名称属于来源/致谢，不参与本项目历史开发名 -> stamky 的品牌迁移。
 */
void show_about(HWND owner){
    // 版本号与 版本.txt / stamky.rc / app.manifest 保持同步。
    // 关于正文纳入多语言：整段作为翻译键（键 = 中文原文，换行用字面 \n 转义，
    // 加载时自动转真实换行）；版本号/作者/网址随键保留，翻译文件只译说明文字。
    MessageBoxW(owner,
        tr(L"stamky  v0.9.3\\n\\n"
           L"by Luminous 20260816\\n\\n"
           L"轻量级 Windows 任务栏分组启动器：点击即出、低内存常驻。\\n"
           L"以开源项目 Stacky 为灵感，借鉴了Stahky的部分设计、按个人需求重构的现代版本。\\n\\n"
           L"原生 C++/Win32 实现，无第三方运行时依赖。\\n\\n"
           L"致谢：\\n\\n"
           L"stacky    https://github.com/pawelt/stacky\\n\\n"
           L"stahky    https://github.com/joedf/stahky\\n").c_str(),
        tr(L"关于 stamky").c_str(),MB_OK|MB_ICONINFORMATION);
}
}
