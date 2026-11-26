# SkyLine（简体中文汉化版）

SkyLine 是一款基于 CommonLibSSE-NG 开发的 Skyrim SKSE 插件。

它会在 Skyrim 游戏窗口出现前显示启动画面和进度条，并在游戏启动过程中显示当前加载阶段及详细状态。启用实验性选项后，也可以在游戏内加载画面中显示进度条。

## 配置说明

配置文件位于 `SKSE/Plugins/SkyLine.ini`。放在同一目录下的 `SkyLine_Custom.ini` 中的同名配置项会覆盖默认配置。

| 配置项 | 说明 |
|---|---|
| `bEnabled` | 启用或禁用插件 |
| `bSplash` | 设为 0 时跳过启动窗口 |
| `iSplashWidth` | 启动画面宽度 |
| `sSplashImage` | 自定义启动图片路径，也可以指定一个随机选图的文件夹 |
| `bBarOnly` | 设为 1 时只显示进度条 |
| `bShowDetail` | 设为 0 时隐藏显示详细加载信息的第二行 |
| `iFontSize`、`sTextColor` | 字体大小和文字颜色 |
| `sFont` | 已安装的字体名称；中文界面建议使用“Microsoft YaHei UI” |
| `bTextShadow` | 设为 0 时关闭文字阴影 |
| `bDpiScaling` | 设为 0 时忽略 Windows 显示缩放 |
| `iBarHeight`、`iBarLength`、`sBarColor` | 进度条粗细、长度和颜色 |
| `sPosition`、`iOffsetX`、`iOffsetY` | 进度条位置及偏移 |
| `bLoadingScreens` | 设为 1 时也在游戏内加载画面显示进度条（实验性功能） |
| `iLingerSeconds` | 游戏加载完成后，完成提示继续显示的秒数 |
| `bLog` | 启用或禁用日志 |

上一次启动的耗时统计以及启动画面的位置会保存在 `SKSE/Plugins/SkyLine.yaml` 中。

## 运行要求

- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

## 下载编译版本

每次向 `main` 分支推送插件、配置或工作流相关修改时，GitHub Actions 都会自动编译。也可以在仓库的 **Actions** 页面手动运行 **Build SkyLine** 工作流。

编译完成后，在对应工作流运行记录底部的 **Artifacts** 中下载 `SkyLine-<commit>` 压缩包。解压后将其中的 `SKSE` 文件夹合并到 Skyrim 游戏目录即可。

压缩包内结构：

```
SKSE/
└── Plugins/
    ├── SkyLine.dll
    ├── SkyLine.ini
    └── SkyLineSplash.jpg
```

## 本地编译

需要 CMake 3.21 或更高版本、Visual Studio 2022 C++ 工具链，以及安装了 CommonLibSSE-NG 和 spdlog 依赖的 vcpkg。

使用静态运行库的 `x64-windows-static` triplet 编译：

```powershell
cmake -S plugin -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

## 致谢

- **Bethesda Game Studios**：Skyrim 及默认启动画面中的游戏主题图片
- **ianpatt、behippo 和 SKSE 团队**：[SKSE64](https://skse.silverlock.org/)
- **meh321**：[Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) 及其贡献者
- [spdlog](https://github.com/gabime/spdlog)
- **ahzaab**：[Skyrim Load Progress](https://github.com/ahzaab/sky-load-progress)（[Clean Loading Transitions](https://www.nexusmods.com/skyrimspecialedition/mods/190193)），游戏内加载队列计数算法的来源
