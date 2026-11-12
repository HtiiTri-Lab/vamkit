# vamkit：给编程 agent 的说明（中文入口）

你多半是因为有人想把这个 mod 装进自己的 VaM、调好或者修好才来的。这一页讲怎么拉、怎么用，以及必须守的几条规则；
完整版是英文的 [AGENTS.md](AGENTS.md)，其余指南、知识文档和代码注释也都是英文。和用户说话请用用户的语言。

## 这个仓库是做什么用的

它是一块地基，不是一件围了栅栏的成品。你和写它的人一样有能力：凭这里的架构、源码、实测、指南和工具，应该把用户这台机器上
能做到的都做到，包括本项目没实现的东西（别的超分或插帧、更高的倍数、给非 RTX 显卡的路径追踪降噪或神经渲染、别的 VaM 版本、别的头显）。
文档里的“没采用”“试过放弃”“不要”和每个缺省值，记录的是在一台机器上、为一个人做过的选择和踩过的坑，用来省你的弯路，
不是禁令；代码里写死的上限只是“工作停在这里”。

真正的限制只有三条：

- **法律**：从 VaM 文件生成的东西、各厂商的二进制和 SDK，不进 git；在用户自己的机器上可以生成、下载、构建。
- **同意**：VaM 文件夹、设置和选择都是用户的。会改 VaM 文件夹的命令先打印计划，用户点头后才加 `--yes`。
- **结果不能错**：替换 VaM 自身工作的部分必须给出和 VaM 一样的结果；做不到就要大声说出来并修好，不能当作通过。

## 怎么拉、怎么用

**先装进一份独立、纯净的 VaM。** 动手之前先把这一点告诉用户：另开一个文件夹放一份全新的 VaM，单独装 BepInEx，只放测试要用的
内容（[docs/agent/open-items.md](docs/agent/open-items.md) 的 “A clean VaM to test on”）。这个套件运行时会替换 VaM 的文件，
会改变场景的载入、渲染和物理，而且只在一台电脑上跑过。用户平时玩的那份安装，要等套件在这台机器上确认没问题、并且用户明确
同意之后再装；在那之前，先帮用户备份无法重来的东西。

```
git clone <本仓库> && cd vamkit
python tools/vamkit doctor --vam "<用户的 VaM 文件夹>"         # 看现状、缺什么；不改任何东西
python tools/vamkit versions --vam "<VaM>"                    # 这台机器和开发这套东西的那台机器哪里不一样
python tools/vamkit fetch --accept-licenses                   # 用户同意各厂商许可条款之后再跑
python tools/vamkit bepinex --vam "<VaM>"                     # VaM 里没有 BepInEx 5 时装它：先看计划，再加 --yes
python tools/vamkit build
python tools/vamkit test
python tools/vamkit install --vam "<VaM>"                     # 只打印计划：给用户看过，再加 --yes
python tools/vamkit verify --vam "<VaM>"                      # 每项检查 ok / LESS / FAIL
python tools/vamkit fetch --lab --accept-licenses && python tools/vamkit build --lab     # 模拟头显（测试工具）
python tools/lab/session/first_run.py --vam "<VaM>"           # 原版、mod 桌面、mod VR 各跑一遍
```

- `doctor` 会逐项说缺什么、该装什么。VaM 文件夹也可以写进 `local/vam.txt`（一行）或环境变量 `VAMKIT_VAM`。
- 首次运行检查不需要头显，也不需要人守在电脑前：它在后台启动 VaM，核对每个部分是否按要求在跑，退出码 0 = 全部照常，
  2 = 能用但打了折扣（逐条列出少了什么、原因、补法，这些该由你去补），1 = 有检查失败。
- 每一步的细节、什么时候该问用户什么：[docs/agent/install.md](docs/agent/install.md)。
- `versions` 列出这台机器上这套东西依赖的每一样的版本（Windows 和它的图形 DLL、显卡和驱动、OpenXR 运行时、VaM 的 Unity
  和 BepInEx、运行时文件、构建工具），和开发它的那一台逐行对比。不一样的每一行都是还没人走过的地方：在用户碰上之前就去看
  （[docs/agent/reference-machine.md](docs/agent/reference-machine.md)）。只差一行就可能出事：一个 Windows 修复之前的
  `d3d11on12.dll` 就足以让画面在第一帧超分时卡死。

装完之后，把它在这台机器上调对：显卡、头显和它的运行时、VaM 的版本。改动前后用 `tools/lab/bench/bench.py` 量
（[docs/benchmarks/README.md](docs/benchmarks/README.md)），原因按 [docs/agent/troubleshoot.md](docs/agent/troubleshoot.md) 去找，
最后用用户听得懂的话说清：得到了什么、在这台机器上哪些打了折扣、为什么。

用户平时双击 VaM 文件夹里的 `vamkit-launcher.bat` 启动；启动器和游戏内菜单有中文和英文
（[components/shared/lang](components/shared/lang/README.md)，再加一种语言只要一张表）。

## 硬规则（摘要，全文见 AGENTS.md）

1. 从 VaM 生成的任何东西不进 git（`out/`、DXBC、`.bin`、`.dll`、VaM 的 IL 或计算核的清单等）。
2. NVIDIA、Intel、AMD 的二进制和 SDK 不进 git；它们由 `vamkit fetch` 取到 `third_party/`（已忽略）。
3. 原版必须是原版：`launch.py --variant vanilla` 用 VaM 自己的文件启动、不加载 BepInEx、不带我们的环境变量。这是用户的退路。
4. 动用户的 VaM 文件夹之前先问；替换任何文件前把原件留在 `<VaM>/vamkit/original/`；卸载后文件夹要和安装前一样。
5. 菜单、启动器、渲染器三处对同一个设置的理解要一致（路径追踪开关、超分模型、各自的渲染比例）。

## 去哪里读

| 你要做的事 | 读 |
|---|---|
| 在新机器上安装 | `docs/agent/install.md`、`docs/agent/runtimes.md` |
| 按显卡、CPU、头显调参 | `docs/knowledge/frame-budget.md`、`docs/knowledge/settings-and-menu.md`、`docs/protocol/performance.md`、`docs/benchmarks/README.md` |
| 缩短或排查场景 / 图片载入 | `docs/knowledge/loading.md`、`docs/knowledge/gc.md` |
| 改或扩展一个组件 | 它的 `README.md`、`docs/agent/modify.md` 和它点名的知识文档 |
| 做本项目没有的东西 | `docs/agent/modify.md` 的 “Building what is not here” |
| 找问题的原因 | `docs/agent/troubleshoot.md` |
| 跑在别的 VaM 版本上 | `docs/agent/adapt-vam-version.md` |
| 这台机器和开发它的那台哪里不同、不同之处怎么办 | `docs/agent/reference-machine.md`（`python tools/vamkit versions`） |
| 这台机器上留给你定的事 | `docs/agent/open-items.md` |
| 怎么测试、怎么量 | `docs/protocol/testing.md`、`docs/protocol/performance.md`、`tools/lab/README.md` |
| 动用户的 VaM 文件夹 | `docs/protocol/changes.md` |

## 工作方式

- 先从代码把原因讲清楚，再开游戏确认；一次只开一个 VaM。
- 改完说明验证了什么、没验证什么（例如：编译通过，没在 VR 里跑）。
- “能用但少了东西”不算通过：缺运行库、插件自己让开、用了替代方案，工具都会标 LESS 并给出原因和补法，要告诉用户并补上。
- 改过着色器后，第一帧路径追踪要等驱动编译管线（一次约 26 秒）。
- 运行优化类插件必须给出 VaM 自己的结果；每个都有校验模式，改过它们或遇到没见过的 VaM 版本时要跑。
