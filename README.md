# stamky v0.9.3

> 原生 C++20 / Win32 的轻量级 Windows 任务栏分组启动器。目标是“点击即出、低内存常驻、列表式管理、可固定多个分组到任务栏”。

**当前发布状态：源码审核发行候选（Source-audited Release Candidate）。** 2026-08-16 已完成全代码库静态/源码审核并实际修复审核中确认的问题；本打包环境不是 Windows，无法执行 Windows SDK/MSVC 编译，因此 **v0.9.3 的 x86/x64 Compile/Link 与实机回归仍必须由 GitHub Actions 或 Windows 本机确认**。本仓库不会以“完全没有任何 bug”作为验证结论。

## 中文说明

### 项目定位

stamky 是独立重写的 Win32 项目，不是 Stacky 或 Stahky 源码的简单改名/复制。设计上保留并致谢：

- **Stacky** — https://github.com/pawelt/stacky
- **Stahky** — https://github.com/joedf/stahky

项目正式名称为 **stamky**。`StackyModern / stackymorden` 仅在少量“识别旧版本并迁移”的兼容代码中保留，不再作为新窗口类、EXE、资源、AppUserModelID、Mutex 或可见品牌使用。

### 使用

1. 运行 `stamky.exe`，Host 在托盘常驻。
2. 双击托盘图标或托盘菜单进入“内容管理”。
3. 新建/选择分组，添加程序、快捷方式、文件、文件夹、URL、自定义项目或子组；也可以扫描指定文件夹的可启动文件。
4. 右侧列表当前顺序就是 Runtime 菜单顺序，可拖动或使用 `Alt+↑ / Alt+↓` 调整。
5. 点击“创建组快捷方式”，程序在 `GroupShortCuts\` 创建 `.lnk`。
6. 先双击该 `.lnk` 验证，再固定到任务栏。
7. 日常点击固定分组入口即可弹出原生 owner-draw HMENU；左键启动，支持的项目右键进入 Windows Shell 原生上下文菜单。

首次从旧 `StackyModern / stackymorden` 版本启动时，程序会尝试迁移 **程序目录 `GroupShortCuts\` 内、且目标仍指向旧 EXE 的组快捷方式**。已经被复制到别处或固定到任务栏的 Shell 副本无法可靠原地重写，建议删除旧固定项，重新生成并固定 stamky 快捷方式。

### 数据目录

stamky 是便携式程序，运行数据默认位于 EXE 同目录：

```text
data\groups.tsv          # 用户内容的权威配置
data\settings.ini       # 全局设置
data\lang\*.ini        # 语言文件
data\启动诊断.log       # 启动/关键失败诊断
cache\runtime.bin        # 可重建运行缓存（STAMKY5 / v5）
cache\icons\...          # 可重建图标缓存
GroupShortCuts\*.lnk     # 组快捷方式
```

`runtime.bin` 和 `cache\icons` 都是派生缓存；`groups.tsv` 才是内容权威来源。

### 构建

本地一键脚本：

```text
构建_x86_Release.bat
构建_x64_Release.bat
构建_全部_Release.bat
```

手工 CMake：

```bat
cmake -S . -B build\x86 -G "Visual Studio 18 2026" -A Win32
cmake --build build\x86 --config Release --parallel

cmake -S . -B build\x64 -G "Visual Studio 18 2026" -A x64
cmake --build build\x64 --config Release --parallel
```

CMake 使用 C++20；项目语法最低要求 CMake 3.25，但 `Visual Studio 18 2026` generator 本身要求 **CMake 4.2+**。MSVC 默认开启 `/W4 /permissive- /EHsc /utf-8 /O2 /Gw /Gy`，链接使用 `/OPT:REF /OPT:ICF /MANIFEST:NO`。自定义 manifest 已由 `stamky.rc` 作为唯一 `RT_MANIFEST #1` 嵌入。

GitHub Actions 使用 `windows-2025-vs2026` 并分别生成 x86/x64 artifact；workflow 位于 `.github/workflows/build.yml`。

### Windows 支持范围

**官方支持/测试目标：Windows 10+。** CMake 目前定义 `_WIN32_WINNT=0x0A00`，manifest 也以现代 Windows 能力为默认环境。

程序本身并非在架构上“硬性只能运行 Windows 10”。有能力的用户可以自行修改项目目标宏/manifest、为较新 Win32 API 增加运行时回退，并使用适合旧系统的 SDK/MSVC/MinGW 工具链尝试 Windows 7 / 8.1 兼容。但这属于 **非官方兼容移植**：本项目不提供该平台的 CI、实机测试或兼容保证，也不能仅通过降低 `_WIN32_WINNT` 就推定可用。

### 当前验证边界

已完成并通过：

- 27 个 C++/头文件 UTF-8/NUL/括号/注释/字符串结构扫描；
- CMake 源文件集合与磁盘文件一致性；
- 默认 cn/en 语言键重复与 `tr()` 覆盖检查；
- 旧品牌残留白名单检查；
- 功能冻结后两批中文注释阶段“剥掉新增模块级/函数级注释即可字节级回到冻结提交”的检查；
- release metadata / workflow / 文档的一致性静态检查。

仍需外部 Windows 环境确认：

- Visual Studio 18 2026 + MSVC 的 **x86 Release** Configure/Compile/RC/Link；
- Visual Studio 18 2026 + MSVC 的 **x64 Release** Configure/Compile/RC/Link；
- Windows 10/11 托盘、单实例、任务栏固定、Runtime Shell 菜单、mixed-DPI、Explorer 重启、设置/管理器以及 100 次菜单压力回归。

详见 [`docs/测试说明.md`](docs/测试说明.md)、[`docs/发行前检查清单.md`](docs/发行前检查清单.md) 与 [`docs/GitHub发行步骤.md`](docs/GitHub发行步骤.md)。

---

## English

### What is stamky?

stamky is a native C++20/Win32 taskbar group launcher focused on instant popup latency, a small resident footprint, and list-based group management. It is an independent rewrite rather than a source rename of Stacky or Stahky.

Credits and inspiration are intentionally retained:

- **Stacky** — https://github.com/pawelt/stacky
- **Stahky** — https://github.com/joedf/stahky

The official project name is **stamky**. Remaining `StackyModern / stackymorden` literals are migration-only probes for legacy instances, shortcuts, or language data.

### Build

```bat
cmake -S . -B build\x86 -G "Visual Studio 18 2026" -A Win32
cmake --build build\x86 --config Release --parallel

cmake -S . -B build\x64 -G "Visual Studio 18 2026" -A x64
cmake --build build\x64 --config Release --parallel
```

Convenience scripts are provided as `构建_x86_Release.bat`, `构建_x64_Release.bat`, and `构建_全部_Release.bat`.

### Supported Windows versions

The official build/test target is **Windows 10+**. The program is not conceptually hard-wired to Windows 10, and experienced users may backport API calls, adjust target macros/manifest settings, and use a suitable toolchain to experiment with Windows 7/8.1. Such builds are **unofficial, untested, and unsupported** by this project.

### Validation status

Completed: full source/static audit, translation coverage, cache/data-format boundary review, legacy-brand whitelist review, and release-file consistency checks.

Pending: Windows + MSVC x86/x64 compile/link and real-machine regression testing.

See `docs/静态检查报告.md`, `docs/测试说明.md`, and `docs/完整接档说明.md` for the complete technical handoff.
