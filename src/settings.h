/*
 * stamky / settings.h
 *
 * Settings 保存用户可调整但不属于内容 Model 的全局选项。尺寸字段始终是逻辑单位，最终由各窗口
 * 按当前 DPI 转成物理像素。launchExtensions 的内部规范是“小写、无前导点、已去重”；
 * normalize_launch_extension 是所有入口的唯一规范化函数。
 */

#pragma once
#include "common.h"

namespace sm {
constexpr size_t kMaxLaunchExtensions = 512;
// 统一规范化启动扩展名：返回小写、不含前导点的 token；非法输入返回空串。
std::wstring normalize_launch_extension(std::wstring token);

struct Settings {
    // Runtime menu visual scale. All values are logical units and are converted per-monitor.
    int menuFontPoints = 10;
    int iconLogicalSize = 28;
    bool columnSeparator = true;
    // 界面语言代码（en/cn/ja…），对应 data\lang\<code>.ini；默认中文 cn。
    std::wstring language = L"cn";

    // 可启动文件扩展名（小写、不含点）。作用于：文件选择器过滤、扫描导入过滤、
    // 主菜单文件夹展开过滤。用户在设置中可自行增删，作为“快捷文件”启动。
    std::vector<std::wstring> launchExtensions = default_launch_extensions();

    void load();
    bool save() const;

    // ext 可带点或不带点；内部统一转为小写无点后匹配。
    bool is_launchable_ext(const std::wstring& ext) const;
    // 设置界面的文本互转：逗号/分号/空白分隔，解析时去点、小写、去重；全部无效时回退默认。
    std::wstring extensions_text() const;
    void set_extensions_text(const std::wstring& text);
};
}
