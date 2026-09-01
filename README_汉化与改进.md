# Guitar RackCraft 汉化与改进说明

本仓库基于 [Varcain/GuitarRackCraft](https://github.com/Varcain/GuitarRackCraft) 的分支。在保持原有功能不变的前提下，完成了**完整简体中文本地化**、**音频引擎稳定性修复**，以及**构建 / 打包 / 体验层面的多项改进与优化**。

---

## 一、界面完整汉化 + 应用内语言切换

- **字符串全量本地化**：`values/strings.xml`（英文）与 `values-zh-rCN/strings.xml`（中文）**250 / 250 一一对应**，覆盖设置、Rack、插件浏览器、Modgui、加载页等全部界面文案。
- **应用内语言切换**：新增 [LanguageManager.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/LanguageManager.kt)，支持「跟随系统 / English / 简体中文」三种模式，设置页即可切换、即时生效。
  - 通过 `attachBaseContext` 包装 `Context` 实现资源重定向；
  - Android 13+ 集成 `LocaleManager`，低版本以 `Locale.updateConfiguration` 兜底。
- **错误消息本地化**：ViewModel 错误文案统一改为字符串资源解析（如 `R.string.rack_err_start_engine`），不再硬编码英文。
- **设备类型名称本地化**：`AudioDeviceInfo` 设备类型映射到本地化字符串。

## 二、插件说明汉化

- 新增 [plugin_metadata_zh.json](app/src/main/assets/plugin_metadata_zh.json)，收录约 **197 个插件的中文描述**（失真/过载、放大器、延迟、混响、EQ、调制、滤波、法兹、箱体模拟等全类别）。
- 插件浏览器 [PluginBrowserViewModel.kt](app/src/main/java/com/varcain/guitarrackcraft/ui/browser/PluginBrowserViewModel.kt) 按当前生效语言加载元数据：
  - 始终以英文 `plugin_metadata.json` 为基准，保证 `categories` / `authors` / `thumbnails` 等字段完整；
  - 中文模式下叠加中文描述，缺失项自动回退英文原文；
  - **插件分类与作者分组不受汉化影响。**
- **分类与作者组名本地化**：浏览器中的插件分类（失真 / 放大器 / 延迟 / 混响 / EQ / 压缩器 …）与作者组（未知 / Windows VST）名称通过字符串资源显示，中文模式下显示中文；品牌名（GxPlugins、Guitarix 等）保持原文。
- 播放对话框「Load WAV」按钮接入 `rack_load_wav` 字符串资源，中文模式下正确显示「载入 WAV」。

## 三、音频引擎稳定性修复

- [AudioEngine.cpp](app/src/main/cpp/engine/AudioEngine.cpp) 重构输入流打开逻辑，实现**三级回退**：
  1. 指定设备 + AAudio
  2. 指定设备 + 默认 API
  3. 丢弃失效的设备 ID，回退系统默认设备

  解决 SharedPreferences 中保存的输入设备（外接声卡 / 耳机麦克风）已不可用时，Oboe 打开输入流失败、引擎完全无法启动的问题。
- [RackViewModel.kt](app/src/main/java/com/varcain/guitarrackcraft/ui/rack/RackViewModel.kt) 启动失败时给出明确错误提示（如「音频引擎启动失败」），避免点击播放无任何反应。
- Rack 界面「引擎未运行」横幅文案本地化。

## 四、功能与体验改进

- **首次启动资产自动解压 + 进度界面**：新增 [EngineInitHelper.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/EngineInitHelper.kt)、[PluginAssetExtractor.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/PluginAssetExtractor.kt) 与 [PluginExtractScreen.kt](app/src/main/java/com/varcain/guitarrackcraft/ui/loading/PluginExtractScreen.kt)，首启解压插件/资产时展示实时进度条（`extracted / total` 回调）。
- **前台服务保活**：新增 [AudioForegroundService.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/AudioForegroundService.kt)，引擎启动后以前台服务保持音频回调，切后台 / 锁屏不被冻结。
- **横屏 / 平板适配**：Modgui 与 Rack 界面根据 `LocalConfiguration` 自适应布局，宽插件横屏显示更友好。
- **构建信息展示**：`BUILD_DATE` / `BUILD_TIME` / `BUILD_HOST` 注入 `BuildConfig`，方便追溯构建来源。

## 五、内置名曲预设 + 完整使用说明

- **内置名曲预设**：新增 [assets/presets/](app/src/main/assets/presets/) 目录，内置 **14 个经典音色预设**——10 个著名歌曲（Smoke on the Water、Sweet Child O' Mine、Back in Black、Enter Sandman、Purple Haze、Hotel California、Stairway to Heaven、Nothing Else Matters、Comfortably Numb、Beat It）+ 4 个当代吉他手典型音色（John Mayer - Gravity、Guthrie Govan - Wonderful Slippery Thing、Stevie Ray Vaughan - Texas Flood、Cory Wong - Funk）。预设统一使用英文名（无中文曲名），每个预设由 `GxAmplifier` + 失真/过载/压缩 + 调制/延迟/混响等插件组合手工调校。
- **首次启动自动导入**：[PresetManager.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/PresetManager.kt) 新增 `importBundledPresets()`，App 首次启动时把内置预设复制到 `files/presets/`，通过 `SharedPreferences` 记录已导入文件，**不覆盖用户修改过的同名预设**；用户删除内置预设后不会在下次启动重新出现。
- **完整使用说明**：新增 [使用说明.md](使用说明.md)，包含应用简介、快速开始、主界面效果链、底部工具栏、插件浏览器、Modgui 参数调节、预设系统、内置名曲预设详解、音频设置、录音回放、语言切换与常见问题（FAQ）等 12 个章节。

## 六、构建与基础设施

- **依赖升级**：AGP **8.7.3**、Kotlin **2.0.21**（启用官方 Compose 编译器插件，替代旧 `composeOptions` 写法）、Gradle **8.9**、`compileSdk / targetSdk 35`（默认）、`minSdk 26`。
- **LV2 引擎完整启用**：交叉编译 **lilv / serd / sord / sratom / zix** 静态库（Meson + NDK），`CMakeLists.txt` 检测到库后置 `HAVE_LV2=1`。本地构建不再停留在 stub 模式，LV2 插件可正常加载运行。
- **资产与插件补全**：将本地构建缺失的内容补齐至与官方 release 一致——
  - 1517 个 Wine DLL（FEX 运行 Windows VST）；
  - 227 个 gxplugins、21 个 brummer、6 个 Neural Amp Modeler 插件 `.so`；
  - 13 个 X11 / Mesa / GL 核心库（libGL、libglapi、libX11、libxcb 等）；
  - 约 303MB 音频与 UI 资产。
- **双变体发布**（`distribution` flavor）：
  - `full`：`targetSdk 28`，内置 `:vsthost_lib`（Wine + FEX，约 1GB），支持 Windows VST，侧载分发（F-Droid / 直接 APK）；
  - `playstore`：`targetSdk 35`，不依赖 VST host，通过 **Play Asset Delivery** 资产包（`gxplugins_pack` / `neural_pack` / `brummer_pack`）按需分发插件 `.so`。
- **release 构建加固**：R8 混淆 + 资源收缩（`isMinifyEnabled` / `isShrinkResources`），签名信息由 Gradle 属性注入。
- **API 兼容性修复**：修复 `minSdk 26` 设备上调用 API 28+ 方法（如 `Application.getProcessName`）导致的 `NoSuchMethodError`，低版本回退读取 `/proc/self/cmdline`。
- **APK 打包问题修复**：修复 Windows 下增量打包导致 `resources.arsc` / `res/` 丢失、安装报「解析软件包时出现问题 / PackageInfo is null」的问题（禁用增量打包后全量重建）。
- **代码质量**：修复 Lint 错误（如 `ProduceStateDoesNotAssignValue`，重构为 `LaunchedEffect` + `mutableStateOf`）、清理 debug 残留代码；`TokenManager` 使用 `EncryptedSharedPreferences`（AES-256-GCM）安全存储令牌。

## 七、构建方法

```powershell
# 完整版（内置全部插件与 Windows VST 栈，侧载）
.\gradlew.bat :app:assembleFullDebug

# Play Store 变体（资产包分发）
.\gradlew.bat :app:bundlePlaystoreDebug
```

APK 输出路径：

```
app\build\outputs\apk\full\debug\app-full-debug.apk
```

## 八、发布

- 本分支的 APK 已放置于 `releases/` 目录，可直接在 GitHub Releases 上传。
- 当前为 debug 签名；正式分发请使用 `RELEASE_STORE_FILE` 等 Gradle 属性配置 release 签名后重新打包。

## 授权

保持原项目 [GPL-3.0](LICENSE) 授权不变。
