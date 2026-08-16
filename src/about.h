/*
 * stamky / about.h
 *
 * 对外只暴露一个同步的“关于”对话框入口。调用方负责提供 owner；本模块不持有 owner，
 * 不创建长期窗口对象，也不改变 App/Model/Settings。保持接口极小可以让关于信息与
 * 核心运行路径完全解耦。
 */

#pragma once
#include "common.h"

namespace sm {
// 关于对话框。显示内容集中定义在 about.cpp，便于单独维护与修改。
void show_about(HWND owner);
}
