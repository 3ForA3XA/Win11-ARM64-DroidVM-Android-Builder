# Win11-ARM64-DroidVM-Android-Builder

在 **root 过的安卓手机**上，直接把 **Windows 11 ARM64 ISO** 构建成 **DroidVM（crosvm/Gunyah）可导入的 raw 磁盘镜像**。无需 PC、无需 Termux、无需跑虚拟机 —— 纯 root 文件操作 + 自带 aarch64 原生工具链。

> Fork 谱系：`Droid-VM/win11-arm64-image-builder`（上游，Windows/macOS 两条构建路线）→ 本项目新增 **Android 路线**。

## APK 安装

直接安装 `DroidVM-Win11-Builder.apk`（~3.7MB，arm64，Android 10+）：

1. 需要 root（Magisk / KernelSU / APatch）
2. 需要一台 Gunyah 可用设备（骁龙 8 Gen 3+，与 DroidVM 要求一致）
3. 把 Win11 ARM64 ISO 放到手机存储（官方直链可从 [微软官网 ARM64 页面](https://www.microsoft.com/software-download/windows11arm64) 获取）
4. 打开 app → 填 ISO 路径（点"扫描"自动发现）→ 可选填 Gunyah/VirtIO 驱动 zip → 开始构建
5. 完成后 raw 镜像输出到 `/storage/emulated/0/DroidVM/Windows.raw`，在 DroidVM app 中导入即可

## 构建流程（engine.sh）

| 步骤 | 工具 | 说明 |
|---|---|---|
| 1 | `7zz` | 从 ISO 提取 install.wim |
| 2 | `wiminfo` | 列出镜像索引（自动选 Pro） |
| 3 | `makegpt` | 自研 C 工具：写 protective MBR + GPT（ESP / MSR / NTFS 三分区） |
| 4 | `mkfs.fat` + `lsetup` + `hiveutil` | ESP：FAT32 + bootmgfw.efi + BCD（由 install.wim 内的 bcd.template patch 生成） |
| 5 | `mkfs.ntfs` + `ntfs3g` + `wimapply` | NTFS 分区 + 离线 apply Windows |
| 6 | `unattend.xml` + `SetupComplete.cmd` + `hiveutil` | OOBE 配置、帐户、自动登录；首开机 pnputil 注入 Gunyah/VirtIO 驱动 |
| 7 | 输出 raw（sparse） | DroidVM 直接导入 |

全程 root 环节：`lsetup`（自研 ioctl 版 loop 挂载，支持 offset/sizelimit）、`mount`、`/dev/fuse`（ntfs-3g）。

## 自带工具链（全部打包进 APK，免 Termux）

| 工具 | 来源 |
|---|---|
| wimlib 1.14.4（wiminfo/apply/extract） | 交叉编译（NDK r27c，API 29） |
| ntfs-3g 2022.10.3（mkfs.ntfs + ntfs-3g，内置 fuse-lite） | 交叉编译 |
| hivex 1.3.24 + **hiveutil**（自研注册表/BCD 编辑器） | 交叉编译 |
| makegpt / lsetup（自研） | 本仓库 `native/` |
| busybox 1.38、mkfs.fat 4.2、7zz 26.06 | Termux 预编译解包 |

## 从源码构建 APK

```bash
# 依赖：JDK17+、Android build-tools 34、platform-34 android.jar、NDK r27c
cd android && ./build.sh
# 产物: out/DroidVM-Win11-Builder.apk
```

原生工具交叉编译见 `native/*.c` 与 [docs/native-build.md](docs/native-build.md)。

## 已知限制

- MVP 输出 **raw** 磁盘；qcow2/`.vmpkg`（vms.json 内嵌配置）后续版本补充
- BCD 生成的完整闭环仍在真机验证中（bcd.template patch 逻辑基于上游 windows_build.ps1 等价实现）
- 首开机驱动注入用 SetupComplete.cmd + pnputil（上游用 DISM 离线注入，Android 无 DISM）

## 许可

GPL-3.0（跟随上游）
