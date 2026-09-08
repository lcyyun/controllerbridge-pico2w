# ControllerBridge Pico 2 W

[English](README.md)

面向私有仓库 `lcyyun/controllerbridge-pico2w` 的独立 Pico 2 W 实验性统一接收器固件。
本项目基于 DS5Dongle，并非从零重写。构建不依赖相邻仓库；Windows 管理器及固件模块、
发布包的打包约定由独立项目维护。

## 功能与边界

- 保留 DualSense / DualSense Edge Bluetooth Classic 和 NS2Pro BLE 输入路径。
- 一次激活一个手柄配置，自动选择并切换为对应 Sony 或 Nintendo 风格 USB 身份；
  初始管理身份不是游戏手柄。
- 保留输入、运动传感器、震动、DS5 音频/触觉和 Feature Report `0x7f` 管理代码。
  编译通过不等于硬件验证，也不保证所有手柄、主机或音频模式均兼容。
- 初始目标为 Pico 2 W。继承的 320 MHz 配置属于超频，需要逐板验证稳定性。
- NS2-only 是可选的独立镜像。Wake HID 不受支持，原有 `src/wake.cpp` 缺失；
  本仓库暂不提供 Pico W 发布配置。

## Windows 构建

在仓库根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-windows.ps1
# 可选的 NS2-only 固件，不是 unified：
powershell -ExecutionPolicy Bypass -File tools/build-windows.ps1 -Variant ns2pro
```

脚本可安装缺失工具并获取固定版本的 SDK。单独下载脚本运行时，会将私有仓库克隆到
`%USERPROFILE%\.controllerbridge-pico2w-build\controllerbridge-pico2w`；
Git 认证需自行配置。已有缓存源码不会被自动 reset 或 pull。

若使用已安装的依赖，将本机 C/C++ 编译器、CMake、Ninja、Git、Python 加入 `PATH`：

```powershell
.\tools\build-windows.ps1 -UseInstalledTools `
  -SdkPath C:\SDKs\pico-sdk `
  -ArmToolchainPath C:\Toolchains\arm-gnu-toolchain
```

此模式不安装工具、不修改 SDK 检出版本。默认 `unified` 配置为：

```text
ENABLE_NS2PRO=OFF
ENABLE_AUTO_PROFILE=ON
PICO_BOARD=pico2_w
PICO_W_BUILD=OFF
ENABLE_SERIAL=OFF
```

NS2-only 使用单独构建目录，设置 `ENABLE_NS2PRO=ON`、`ENABLE_AUTO_PROFILE=OFF`。
保留的 `standard` 是 DS5-only 开发配置，`debug` 是开启串口及详细日志的 unified；
两者都不是默认发布配置。

每个变体在 `build/<variant>/` 构建 `ds5-bridge` 目标，产生 `ds5-bridge.uf2`，
再复制到相应目录：

| 变体 | 输出 |
| --- | --- |
| `unified` | `artifacts/unified/controllerbridge-pico2w-unified.uf2` |
| `ns2pro` | `artifacts/ns2pro/ns2pro-bridge-pico2w.uf2` |

脚本打印 SHA-256，不刷写、不访问设备、不复制到桌面。
管理器的版本化模块文件名与兼容别名由打包流程处理。

## 固定依赖

保持 Pico SDK `2.2.0`、TinyUSB `0.20.0`、ARM GNU `14.2.rel1`。
SDK、TinyUSB、BTstack、cyw43-driver 的完整提交号见 [英文依赖表](README.md#dependency-pins)；
构建脚本和 CI 会校验这些提交及 ARM 版本。`lib/opus/`、`lib/WDL/` 保留导入快照，
不是 Git 子模块。SDK 所需 `pioasm`、`picotool` 会在构建目录内生成，首次配置可能需要联网。
Pico VS Code 的 SDK 覆盖默认关闭，不自动升级已有依赖。

## 目录、验证与许可证

- `src/`：DS5 主路径，`src/profiles/` 统一选择，`src/ns2/` BLE/HID。
- `lib/`：保留的 Opus/WDL 源码及许可证。
- `tools/`：构建脚本和可选 WebHID 调试工具。运行
  `node tools/serve-ns2-webhid-tuner.js`，访问
  `http://127.0.0.1:8787/tools/auto-debug-webui.html`。
- `.github/workflows/`：仅手动构建、复用构建和候选验证。unified 与 NS2-only 分开，
  不由 push、tag 或 release 事件自动发布，也不创建 GitHub Release。
- `SOURCE-SNAPSHOT.json`：保留原始导入哈希，不作为后续构建文件仍未修改或固件已验证的证明。
- [LICENSE](LICENSE)、[NOTICE.md](NOTICE.md)、[LICENSES/](LICENSES/)：
  保留 MIT 项目来源和 Apache-2.0 NS2 协议参考说明，以及
  [Opus COPYING](lib/opus/COPYING) 和 WDL 源码许可证。
  历史 NOTICE 提到的其他原仓库目标不在本仓库中；发布前仍需审核 SDK 依赖的再分发声明。
