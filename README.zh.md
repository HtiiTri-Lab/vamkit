# vamkit

Virt-A-Mate（VaM）1.x 的大幅画质与运行效率优化，Windows + NVIDIA RTX 显卡：游戏内可开关的路径追踪、DLSS 超分和光线重建（RR）、
DLSS 神经降噪、VR 和桌面的 DLSS 插帧，新的皮肤、头发、衣物着色，以及让 VaM 自己的头发、衣物、人物更新更省时间的运行优化。

[English](README.md)

**状态：开发中。** 仓库已经能自己取第三方依赖、从源码构建全部组件、跑离线测试，并且可逆地装进 VaM 目录
（`vamkit fetch`、`build`、`test`、`install`、`verify`、`uninstall`），并在一份不含内容的 VaM 副本上通过了桌面模式的首次运行检查
（`tools/lab/session`）。这份构建还没有在 VR 下、也没有用真实场景跑过。见 [CHANGELOG.md](CHANGELOG.md)。

## 包含什么

| 部分 | 效果 |
|---|---|
| 路径追踪（游戏内开关） | 人物、衣物、房间、头发全部路径追踪，由 DLSS RR 重建 |
| 路径追踪关 | VaM 自己的光照和阴影，加上新的皮肤、头发、衣物着色 |
| 超分 | 路径追踪关时 DLSS SR 的 K / L / M，开时 RR；每个模型各有自己的渲染比例 |
| 降噪 | 输出上的 DLSS 神经降噪，游戏内可调 |
| 插帧 | DLSS 插帧 2–4×；VR 走 OFXR 层，桌面走我们自己的呈现器 |
| 皮肤 | 屏幕空间次表面散射和薄处透光（耳朵、手指），开不开路径追踪都有 |
| 运行优化 | 头发和衣物物理合并成少量 GPU 计算、包裹衣物合批、人物的 CPU 热点改原生、更快的 Mono 回收器、头发开销预算；结果和 VaM 自己的代码相同 |
| 启动器 | 选 **增强画面 mod** 或 **原版**，VR 或桌面。原版不加载本项目的任何东西（也不加载任何 BepInEx 插件） |

## 怎么用

只发源码。在你自己的电脑上、对着你自己的 VaM 构建，最好交给编程 agent 来做（Claude Code、Codex、Cursor 等会读
[AGENTS.md](AGENTS.md)）。一部分着色器在构建时从你的 VaM 安装里的着色器生成，只留在你的电脑上。

需要：

- 你自己的正版 VaM 1.x，并装好 BepInEx 5
- Windows 10 或 11，NVIDIA RTX 显卡（路径追踪要 DXR 1.1，推荐 RTX 50）和较新的驱动
- Visual Studio 2022 Build Tools（C++ 和 Windows SDK）、CMake、Python 3.11 以上

```
python tools/vamkit doctor --vam "D:/VaM"
python tools/vamkit fetch --accept-licenses
python tools/vamkit build
python tools/vamkit test
python tools/vamkit install          # 先打印计划；加 --yes 才执行
```

## 为你的 agent 而写

安装、调优、扩展这个项目，最好的方式是交给编程 agent。这个仓库既是写给你的，也是写给它的：

- [docs/agent](docs/agent)：怎么安装、改一个组件、适配别的 VaM 版本、查问题的原因。
- [docs/knowledge](docs/knowledge)：每项优化的原理和这么做的理由、实测数据、试过又放弃的做法、还没解决的问题。
- [docs/protocol](docs/protocol)：我们怎么测试、怎么判断性能、怎么改一份安装而不把它弄坏。
- [tools/lab](tools/lab)：我们自己用的检测和测量工具。
- 每个组件目录里有一份 README：文件、构建、设置、测试，以及改动不能破坏什么。

拉下这个仓库，得到的不只是一份构建，还有一份调优指南和可以接着做的地基。

## 注意

- 非官方，和 MeshedVR、NVIDIA 没有关系。
- mod 运行期间，启动器把 VaM 的 `VaM_Data/Plugins/openvr_api.dll` 换成 OpenComposite，并在 `VaM.exe` 旁边放一个
  `d3d11.dll`；VaM 退出时换回原来的文件。手动换回用 `launch.py --restore`。
- 仓库里没有 NVIDIA 的运行库和 SDK。`vamkit fetch` 在你同意 NVIDIA 的条款后从 NVIDIA 下载。

## 许可

我们的代码是 MIT。OpenComposite 是 GPL-3.0，OFXR Bridge 是 LGPL-3.0，沿用上游。见 [THIRD_PARTY.md](THIRD_PARTY.md)。
