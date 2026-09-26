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

## 三之二、低延迟与稳定性深度修复（按机型适配）

- **OpenSL ES 降级后的空指针崩溃修复**：输入流在部分机型（如 Redmi K80）会从 AAudio 降级为 OpenSL ES；此时若仍调用 AAudio 专属的 `OboeExtensions::isMMapUsed()`，会在内部强转空指针导致应用崩溃。新增 `AudioEngine::isMMapUsedSafe()`，仅对 AAudio 流调用该函数，其余 API 直接返回 0。
- **OpenSL ES FAST path 校验与回退**：降级到 OpenSL ES 后校验是否真正拿到 FAST path（独占 + 低延迟）；若未拿到则回退 AAudio 共享模式，避免共享模式下输入调度不稳、周期性欠载产生的「滋滋」杂音。
- **RingBuffer 解耦（参考 NAM Sandwich 架构）**：新增 SPSC 无锁 `AudioRingBuffer`，输入先写入环形缓冲，DSP 按固定块大小从环形缓冲消费并跑完整插件链，输出同样经环形缓冲输出。将 HAL 的输入供应节拍与处理节拍解耦，吸收部分机型 10–13 ms 的 HAL 调度抖动；启动时预填充输出环到约 12 ms 目标水位，保证输出不下溢。
- **自适应缓冲 + 延迟上报**：根据 xrun 情况自动增长输出缓冲并即时更新 UI 延迟显示；延迟显示包含环形缓冲水位与输出缓冲，反映实际听感延迟。
- **噪声门增强**：加入**迟滞（+3 dB）+ 保持时间 + 慢启动 / 快释放包络**。慢启动包络可忽略输入流的 1–2 ms 瞬时尖刺（避免门被噪声持续顶开），保持时间决定止音后杂音的收尾长度。
- **软限幅器**：输出端加入软限幅（0.98 天花板 + 快攻慢放），抑制高增益 NAM 模型输出的瞬时削波爆音。
- **NAM 输入归一化**：加载 NAM 模型时启用输入归一化，把小信号提升到模型训练时的正确工作区，改善动态饱满度与信噪比。
- **NAM a2.0 支持**：内置模型与加载逻辑支持 NAM **a2.0**（SlimmableContainer）架构，兼容旧版 WaveNet 模型。

## 四、功能与体验改进

- **首次启动资产自动解压 + 进度界面**：新增 [EngineInitHelper.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/EngineInitHelper.kt)、[PluginAssetExtractor.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/PluginAssetExtractor.kt) 与 [PluginExtractScreen.kt](app/src/main/java/com/varcain/guitarrackcraft/ui/loading/PluginExtractScreen.kt)，首启解压插件/资产时展示实时进度条（`extracted / total` 回调）。
- **前台服务保活**：新增 [AudioForegroundService.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/AudioForegroundService.kt)，引擎启动后以前台服务保持音频回调，切后台 / 锁屏不被冻结。
- **横屏 / 平板适配**：Modgui 与 Rack 界面根据 `LocalConfiguration` 自适应布局，宽插件横屏显示更友好。
- **构建信息展示**：`BUILD_DATE` / `BUILD_TIME` / `BUILD_HOST` 注入 `BuildConfig`，方便追溯构建来源。

## 五、内置名曲预设 + 完整使用说明

- **内置名曲预设**：新增 [assets/presets/](app/src/main/assets/presets/) 目录，内置 **14 个经典音色预设**——10 个著名歌曲（Smoke on the Water、Sweet Child O' Mine、Back in Black、Enter Sandman、Purple Haze、Hotel California、Stairway to Heaven、Nothing Else Matters、Comfortably Numb、Beat It）+ 4 个当代吉他手典型音色（John Mayer - Gravity、Guthrie Govan - Wonderful Slippery Thing、Stevie Ray Vaughan - Texas Flood、Cory Wong - Funk）。预设统一使用英文名（无中文曲名），每个预设由 `GxAmplifier` + 失真/过载/压缩 + 调制/延迟/混响等插件组合手工调校。
- **首次启动自动导入**：[PresetManager.kt](app/src/main/java/com/varcain/guitarrackcraft/engine/PresetManager.kt) 新增 `importBundledPresets()`，App 首次启动时把内置预设复制到 `files/presets/`，通过 `SharedPreferences` 记录已导入文件，**不覆盖用户修改过的同名预设**；用户删除内置预设后不会在下次启动重新出现。
- **完整使用说明**：新增 [使用说明.md](使用说明.md)，包含应用简介、快速开始、主界面效果链、底部工具栏、插件浏览器、Modgui 参数调节、预设系统、内置名曲预设详解、音频设置、录音回放、语言切换与常见问题（FAQ）等 12 个章节。

## 五之二、新增功能：调音表与模型管理

- **调音表（Tuner）**：新增 [Tuner.cpp](app/src/main/cpp/engine/Tuner.cpp) / [TunerScreen.kt](app/src/main/java/com/varcain/guitarrackcraft/ui/tuner/TunerScreen.kt)，从 Rack 右上角菜单进入，实时检测音高并提示最接近的琴弦（参考音高 A4 = 440 Hz，适配任意乐器）。
- **模型管理（Model Manager）**：新增 [ModelManagerScreen.kt](app/src/main/java/com/varcain/guitarrackcraft/ui/models/ModelManagerScreen.kt)，集中管理 NAM / IR 文件：
  - **内置模型**：随包附带精选 NAM 模型与 IR 文件，首次启动后自动可用并标记为「内置」。
  - **一键导入**：支持导入 `.nam` / `.json` 模型（含 NAM a2.0 / SlimmableContainer 架构）与 `.wav` IR 文件。
  - **自动路由**：选择模型后点击「应用到效果链」，按文件类型自动路由到链中的 NAM 插件或 IR 加载插件。
  - **IR 加载开关**：可开关 IR 加载——整链采集（含箱体）的 NAM 模型叠加 IR 会造成双重箱体滤波、声音发闷，关闭开关即可避免；开关状态持久保存。

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
