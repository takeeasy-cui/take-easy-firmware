# 给下一个 AI 的接手命令（复制以下全部内容发给新 AI 即可）

---

你将接手 take-easy 墨水屏阅读器（D:\readpico-rc95，ESP32-S3+16MB flash+8MB PSRAM）的固件开发，同时继续《逆转裁判1》NDS→设备的完整移植。**不要从零开始、不要重新推导、不要重复开发**——之前所有成果、经验、踩坑都已在下面的档案里完整落档，直接引用。

## 第一步：按顺序读档案（读完再动手）

1. `D:\readpico-rc95\docs\HANDOFF.local.md` —— 固件任务账本（最新在上；顶部"双线状态速览"+长期规则+每批完整改动记录）
2. `D:\take-easy\docs\local\nds1-port\00-HANDOFF.md` —— 游戏移植总交接（§5 案件块边界已实证修正，勿用旧说法）
3. `D:\take-easy\docs\local\nds1-port\02-PIPELINE.md` —— 管线操作手册（工具命令可直接复制执行）
4. 系统长期记忆 MEMORY.md（自动载入，含构建环境与烧录铁律）

## 当前状态（2026-10-19 深夜）

**固件**：第十九批 0x3a67a0 已烧录真机，七项修复待用户真机回归反馈：①阅读数据面板导航栏恢复 ②备忘录整卡点击进输入+键盘界面整体重做（与天气编辑器同构）③天气输入框错位修复 ④键盘确定键先选词再保存 ⑤保存地点后首页立即显示（离线也显示）。输入法引擎（YanReader IME 移植，ime 分区 mmap 码表）已上线。

**游戏**：五案翻译案1-4 全部完成（约 15,500 行，trans_out\ 下 case*_cn.tsv.partN）；案5 未译（六分片已切好 trans_tasks\case5_a~f.tsv 各 1864 行，前三子代理被中止需重派）；DSL 仅案1 demo（case1_full2.txt 预检 PASS，demo 素材 9 项占位）；**案件块边界已实证修正**：案1=blk000/001 只有一场庭审（没有 Day2，原"案1 Day2 译文"实为案2 前段 blk002/003 的 1002 行）；案2=blk002-011、案3=blk012-023、案4=blk024-033、案5=blk034-071、blk072=系统勿用。

## 铁律（违反任何一条都算事故）

1. **文本不猜**：只使用已核实来源（译文/实测/档案），绝不"目读字形猜字"
2. **构建产物必须等用户明确说"烧录"才烧**；烧前过账本回归清单（字体/锁屏/封面/翻页/USB 读卡；改共享层升级全项）
3. 墨水屏刷新受控，禁止无意义全屏刷
4. 文档没覆盖的决策点先问用户
5. **账本协议**：每批改动记入 `D:\readpico-rc95\docs\HANDOFF.local.md`（最新在上，不删历史）；游戏线记 nds1-port 档案
6. 构建：`cmd /c "cd /d D:\readpico-rc95 && build_fw.bat"`（cmd 前台，勿用 PowerShell Start-Process 包 cmd；git.exe 不在 PATH，在 D:\tools\MinGit\cmd\）
7. 烧录（当前分区布局）：单段 app = usb-reset 写 Read_Pico.bin@0x10000 → hard-reset(chip_id 触发 RTS)；若涉及分区表/码表则六段一次写（bootloader@0x0+分区表@0x8000+app@0x10000+pinyin@0x410000+liangfen@0x770000+ota_data_initial@0xC10000）；烧前探串口 COM3
8. 用户打断=补充细节，继续前核对上次完整运行后的全部反馈，避免漏项

## 新要求（优先实施）：功能版本留存机制

用户要求：**每个功能定稿前的每次修改都要有文件留存，可随时回退到任意历史状态并在其基础上继续修改**。例如：计算器锁屏要能直接回到"第二次做好的状态"（=第十七批 0x34d130 版）再改。

实施要点：
- 工程无 git（D:\readpico-rc95 无 .git）。二选一：①每批构建通过时把本批涉及源文件快照到 `D:\readpico-rc95\.snapshots\<功能名>\<批次-日期>\` 并写说明（回退=拷回+重建+烧录）②直接 git init 后每批 commit+tag（git 用全路径 D:\tools\MinGit\cmd\git.exe）。选哪种先问用户，选完立即补齐历史。
- **计算器锁屏历史锚点**（回退十七批的具体反推法，已写进账本长期规则区）：app_device_settings.c 两处把"open 后设标志"对调回"先设标志再 open"（行 173-174 的 `s_pin_style_after = style; pin_setup_open(ctx, true);` 与行 2036-2037 的 `s_pin_for_lock_style = true; pin_setup_open(ctx, false);`）——这就是十七批"第二次做好"的状态（它当时有"设完不自动应用"的 bug，用户如要在此基础上改，明确告知该 bug 已知原因再动手）
- 机制建好后，**每批构建通过即快照**，用户真机验收定稿后在快照说明里标记

## 下一步优先级（用户已确认的目标：继续完成逆转裁判完整移植+评估引擎优化）

1. **建立功能版本留存机制**（上文新要求，先实施，含历史补齐）
2. **等待用户对第十九批固件的真机回归反馈**（有反馈先修固件）
3. **案5 翻译重派**：六分片各 1864 行（trans_tasks\case5_a~f.tsv），一次派 2-3 个子代理、每片边译边分 part 写盘到 trans_out\（防超上下文中断；派发 prompt 模板在 02-PIPELINE §2，案5 特有：SL-9 事件/严徒海慈/宝月巴/宝月茜/原灰须/米金斯）
4. **译名统一校对**：案2 a 片"小中火" vs b 片"小中大"、案3 b 片"殿武士/荷星/英都摄影所" vs c 片"大将军/环球摄影场"——写脚本列出各片专名差异并全局统一
5. **mes2dsl_v3 多块通用转译器**（v2 仅案1 单块）：按 00-HANDOFF §8-3 的扩展指引
6. **案1 完整包**：素材替换 9 项占位（gya_chars 223 张立绘+pwaa1 522 张 PNG）→ 预检 → 重打包 → 用户真机验收
7. **案2-5 DSL+打包**
8. **引擎优化评估**：案5 预计 4371 cmds > AVG_MAX_CMDS 3072——优先 say 合并压缩（阈值 32→64 字/高频 char 去重），不够再扩 AVG_MAX_CMDS 到 4096（PSRAM 实量分配安全；改后注意"推理游戏+自定义字体+证物簿同开"的 PSRAM 峰值）——引擎优化完成后需同步 D:\take-easy 仓库（先备份到 .backup\ 再覆盖）

## 环境速查

```
Python:  D:\esp-tools\python_env\idf6.1_py3.12_env\Scripts\python.exe -X utf8  （跑 nds_chs 脚本一律 workdir=.temp\nds_chs）
git:     D:\tools\MinGit\cmd\git.exe（不在 PATH）
构建:    cmd /c "cd /d D:\readpico-rc95 && build_fw.bat"
烧录:    C:\Users\Administrator\.espressif\python_env\idf6.1_py3.12_env\Scripts\esptool.exe
游戏预检: python D:\take-easy\tools\avgr_script_check.py <script.txt>
游戏打包: python D:\take-easy\tools\game_pkg.py avgr <案件目录>   → avgr-*.zip → G:\games\（G 盘挂载时）
工作区:   C:\Users\Administrator\.local\share\TeleAgent\TeleAgent的工作空间\.temp\nds_chs\（主工作区）
素材:     .temp\gya_chars\（223 立绘） .temp\pwaa1\pwaa1-master\graphics\（522 PNG）
网络:    GitHub 直连被墙，ghfast.top / gh-proxy.com 镜像可用
G: TF 卡: 上卡前先确认挂载
```

从"下一步优先级"第 1 条开始执行；每完成一项立即落档对应账本/档案。

> AI生成
