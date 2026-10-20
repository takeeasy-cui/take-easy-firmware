# 案件素材目录（逆转裁判 · 最初的逆转 Demo）

图片全部为 16 级灰度 PNG（8bit 灰度 PNG 即可，引擎解码后 16 级抖动上屏）。
素材缺失时引擎自动画占位框（文件名提示），**不影响通关**。

## 目录约定

```
images/
├── bg/                    背景（目标框 684×830，等比缩放居中）
│   └── bg_court.png
└── char/                  角色立绘（目标框 ≤470×640）
    ├── naruhiko.png       成步堂龙一
    ├── chihiro.png        绫里千寻
    └── konaka.png         小中大
```

脚本引用：`bg bg_court.png`（即 images/bg/bg_court.png）；
`char 0 naruhiko.png C 1`（即 images/char/naruhiko.png，槽0/中站位/聚焦）。

## 拷贝到设备

TF 卡根目录：
```
/sdcard/games/gyakuten/case1/
├── script.txt          ← tools/avg_games/gyakuten_case1/script.txt
└── images/...
```
（可通过设备「传输文件 → USB 读卡」把 TF 卡挂为 U 盘后直接拷贝。）

## 立绘站位

- `char <槽> <文件> <L|C|R> <0|1>`：槽 0-2、左/中/右、是否聚焦(说话人)。
- 证据图片暂由证物簿占位字形显示（后续版本接入 ev/ 目录图标）。

> AI生成
