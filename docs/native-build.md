# 原生工具交叉编译指南（NDK r27c）

目标：aarch64 Android（bionic），API 29+（glob() 等需要 API 28+）。

```bash
export NDK=/opt/android/android-ndk-r27c
export PATH=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH
export CC=aarch64-linux-android29-clang AR=llvm-ar RANLIB=llvm-ranlib
```

## wimlib 1.14.4（静态）

```bash
./configure --host=aarch64-linux-android --prefix=$OUT/wimlib \
  --disable-shared --enable-static --without-ntfs-3g --without-fuse
make -j$(nproc) && make install
```
产物 `bin/wim{info,apply,extract}` 仅依赖 libc/libdl。

## ntfs-3g 2022.10.3（静态 + 内置 fuse-lite + ntfsprogs）

bionic 无独立 libpthread，先造 stub：

```bash
$CC -c /dev/null -o /tmp/empty.o && llvm-ar rcs \
  $NDK/sysroot/usr/lib/aarch64-linux-android/29/libpthread.a /tmp/empty.o

./autogen.sh
./configure --host=aarch64-linux-android --prefix=$OUT/ntfs3g \
  --disable-shared --enable-static --enable-ntfsprogs \
  --with-fuse=internal --disable-plugins ac_cv_lib_pthread_pthread_create=yes
make -C lib && make -C libfuse-lite && make -C src && make -C ntfsprogs
cp src/ntfs-3g $OUT/bin/ntfs3g
cp ntfsprogs/mkntfs $OUT/bin/mkfs.ntfs
```

## hivex 1.3.24（必须用官方 release tarball）

GitHub tag 归档缺预生成文件（generator 是 OCaml）；用 `https://libguestfs.org/download/hivex/hivex-1.3.24.tar.gz`：

```bash
./configure --host=aarch64-linux-android --prefix=$OUT/hivex \
  --disable-shared --enable-static --without-perl --without-python \
  --without-ocaml --without-ruby
make -C lib && make -C sh
# hivexregedit 是 Perl 脚本（Android 无 Perl）→ 用本仓库自研 hiveutil 替代
gcc -O2 -Iinclude hiveutil.c lib/.libs/libhivex.a -o hiveutil   # 改用 NDK CC 交叉编译
```

## 自研工具

```bash
$CC -O2 -static makegpt.c -o makegpt     # GPT 分区表写入
$CC -O2 -static lsetup.c -o lsetup       # LOOP_CONFIGURE ioctl 挂载（offset/sizelimit）
$CC -O2 -static -I<hivex-include> hiveutil.c <libhivex.a> -o hiveutil
```

## Termux 预编译直取（免编译）

`busybox 1.38`、`mkfs.fat 4.2`、`7zip 26.06`、`zstd`、`xz`：
从 `https://packages.termux.dev/apt/termux-main/dists/stable/main/binary-aarch64/Packages`
取 Filename 路径下载 deb，`dpkg -x` 解包即用（bionic ELF，注意 `-L` 跟随重定向）。

busybox 需 patch：`DT_NEEDED libbusybox.so.1.38.0 → libbusybox.so`（Android jniLibs 不允许带版本号文件名）。

## 打包进 APK

jniLibs 命名约定（可执行文件必须 `lib*.so`，安装后位于 `nativeLibraryDir`，可执行位自动保留）：

```
lib_wiminfo.so lib_wimapply.so lib_wimextract.so
lib_mkfs.ntfs.so lib_ntfs3g.so lib_mkfs.fat.so
lib_hiveutil.so lib_makegpt.so lib_lsetup.so lib_hivexsh.so
lib_busybox.so libbusybox.so lib_7zz.so libc++_shared.so
```

运行时 `LD_LIBRARY_PATH=<nativeLibraryDir>`。

## 本机 x86 测试

同一份源码用系统 gcc + `apt install libhivex-dev` 编 x86 版做 hive/BCD 逻辑验证。
