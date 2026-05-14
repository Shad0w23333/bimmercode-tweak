# bimmercode-tweak

针对 **BimmerCode 4.4 (`com.bimmerlink.BimmerLinkCoder`)** 的解锁 dylib,适用于 **iOS 26+ 非越狱** 与 **macOS PlayCover**。

零外部依赖(不需要 Substrate / ElleKit / CydiaSubstrate),只依赖 `libSystem`,纯 ObjC runtime swizzling 实现。

---

## 工作原理

dylib 在 dyld load 时 `__attribute__((constructor))` 启动,完成两件事:

### Layer 1 — 购买 gate hooks

遍历进程内所有 ObjC 类的方法表,**只 patch BimmerCode 主程序 / BimmerCode.dylib 内的方法**(用 `dladdr` 检查 IMP 所在 image,过滤所有系统 / Firebase / AWS 类),按 selector 名字模式精确分类:

| 行为 | 匹配的 selector |
|---|---|
| 强制 `YES` | `is*FullVersion*Purchased*`、`is*UnlockCoding*Purchased*`、`hasAnyPurchase`、`isAdvancedAdapter`、`is*ProMode*` |
| 强制 `NO`  | `isAdapterRestrictionEnabled`、`isFreeTrial`、`needsPresentSpeedlockUnlockMessage` |

覆盖的关键类(自动发现,不需要硬编码类名):

- `InAppPurchaseController`(ObjC 老 license API)
- `BimmerCode.BillingManager`(Swift 4.4 新 license 后端,`@objc dynamic`)
- `BimmerCode.IAPProduct` / `IAPProductLegacy`
- `ELM327Interface` / `ENETInterface` / `ENETSimInterface` / `MHDAdapter` / 各种 VLinker / OBDLink 子类(`isAdvancedAdapter` → YES)
- `ESeriesCodingECU` / `FSeriesCodingECU` / `USeriesCodingECU` / `KSeriesCodingECU` / `CodingECUInfo`(`isAdapterRestrictionEnabled` → NO)

启动期一次扫描,典型结果:`total=19 (YES=15, NO=4), skipped foreign images=8`。

### Layer 2 — 本地化字符串注入(彩蛋)

Hook `-[NSBundle localizedStringForKey:value:table:]`,**只拦截白名单 key**,其余全部转发给原 IMP:

```
MY_PURCHASES → "Cracked by @xiaochoumao 2026/5/13"
```

不修改 `Localizable.strings` 文件,运行时替换。所有 13 种系统语言下显示同样的字符串(因为 hook 在文件查询之上的语义层完成)。

---

## iOS 26+(非越狱设备)

### 部署到 IPA

```
Payload/BimmerCode.app/Frameworks/BimmerCode.dylib   ← 唯一文件
```

让签名工具(Sideloadly / AltStore / Esign / Cyan / Azule)勾选 "Inject dylib" 选择这个文件;它会自动给主程序追加:

- `LC_LOAD_DYLIB @rpath/BimmerCode.dylib`
- `LC_RPATH @executable_path/Frameworks`(如已有则跳过)

然后 deep-sign 整包用你的开发者证书,装机即可。

无需任何 entitlement / provisioning profile 特殊配置。

---

## macOS PlayCover

```bash
APP=~/Library/Containers/io.playcover.PlayCover/Applications/com.bimmerlink.BimmerLinkCoder.app

cp BimmerCode.dylib "$APP/Frameworks/BimmerCode.dylib"
codesign --force --sign - --timestamp=none "$APP/Frameworks/BimmerCode.dylib"
codesign --force --sign - --timestamp=none --deep "$APP"
```

PlayCover 已经在主程序的 `LC_LOAD_DYLIB` 里加好了 `@executable_path/Frameworks/BimmerCode.dylib`(初始 inject 留下的),直接覆盖文件 + 重签即可。

PlayCover 会把 LC_BUILD_VERSION 标记为 `platform 6 (macCatalyst)`,我们的 dylib 是 `platform 2 (iOS)` 但能正常加载(Apple Silicon Mac 的 dyld 接受 iOS dylib 与 macCatalyst host 同进程)。

---

## 验证

启动后看日志,确认 hook 状态。dylib 三通道输出:`stderr` + `syslog` + `/tmp/bimmercode_mod.log`。

```bash
# 方法 1 — 文件
tail -f /tmp/bimmercode_mod.log

# 方法 2 — 系统日志(macOS)
log stream --predicate 'eventMessage CONTAINS "BimmerMod"' --info

# 方法 3 — Console.app GUI,搜索 "BimmerMod"
```

期望输出:

```
[BimmerMod] Bimmercode 4.4 Mod v8 | hooks + localization easter-egg
[BimmerMod] YES -[InAppPurchaseController hasAnyPurchase]  (image=BimmerCode)
[BimmerMod] YES -[BimmerCode.BillingManager isUnlockCodingPurchased]  (image=BimmerCode)
[BimmerMod] YES -[ELM327Interface isAdvancedAdapter]  (image=BimmerCode)
[BimmerMod]  NO -[CodingECUInfo isAdapterRestrictionEnabled]  (image=BimmerCode)
... (~19 patched)
[BimmerMod] === scan summary: total=19 (YES=15, NO=4), skipped foreign images=8 ===
[BimmerMod] hooked -[NSBundle localizedStringForKey:value:table:]
[BimmerMod]   substitution: 'MY_PURCHASES' -> custom NSString
```

### 用 frida 实时验证(可选)

```bash
pip3 install --user frida-tools
PID=$(pgrep -f BimmerCode | head -1)
frida -p $PID -l verify_hooks.js   # 看每个 BOOL 方法实际返回值
```

---

## 从源码构建

需要 macOS + Xcode Command Line Tools。

```bash
SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk

for arch in arm64 arm64e; do
  case $arch in
    arm64)  TARGET="arm64-apple-ios13.0" ;;
    arm64e) TARGET="arm64e-apple-ios14.0" ;;
  esac
  clang -arch $arch -target $TARGET -isysroot "$SDK" \
    -dynamiclib -install_name "@rpath/BimmerCode.dylib" \
    -Wno-incompatible-sysroot -O2 \
    -o "BimmerCode-$arch.dylib" src/patch.c
done

lipo -create BimmerCode-arm64.dylib BimmerCode-arm64e.dylib -output BimmerCode.dylib
```

注意:`clang` 会发 `building for iOS but linking macOS libSystem.tbd` 的 warning —— 无害,我们用 dlsym 在运行时拿所有需要的符号,不依赖 link-time tbd。

---

## 修改替换字符串

`src/patch.c` 找这一段:

```c
static struct subst g_substitutions[] = {
    { "MY_PURCHASES", &g_replacement_my_purchases },
    { NULL, NULL }
};
```

新增条目:
1. 声明一个 `static id g_replacement_xxx = NULL;`
2. 在 `install_localization_hook()` 里加一行 `g_replacement_xxx = make_nsstring("...");`
3. 在 `g_substitutions[]` 加 `{ "FULL_VERSION_CAR", &g_replacement_xxx }`

重编、重签、重启即可。

iOS 中文本地化 key 在 `Payload/BimmerCode.app/zh-Hans.lproj/Localizable.strings`(binary plist),`plutil -p` 可看全部。

---

## dylib 元数据

| | |
|---|---|
| 架构 | arm64 + arm64e fat |
| install_name | `@rpath/BimmerCode.dylib` |
| 依赖 | 仅 `/usr/lib/libSystem.B.dylib` |
| LC_BUILD_VERSION | platform=iOS, minos=13.0/14.0 |
| 签名 | 无(等下游签) |

---

## 版本历史

| 版本 | 关键改动 |
|---|---|
| v2 | 适配 4.4 `isFullVersionPurchased` → `Legacy` 重命名 |
| v3 | 增加 `isUnlockCoding*Purchased` / `hasAnyPurchase` hook |
| v4 | 增加 `isAdapterRestrictionEnabled` / `isAdvancedAdapter`(菜单层 gate) |
| v5 | Pattern-based 全类扫描 + 调用日志 |
| v6 | 三通道日志(stderr + syslog + 文件)适配 macOS PlayCover |
| v7 | `dladdr` 镜像过滤,只 patch BimmerCode 自家类,避免误伤 macOS 系统类 |
| **v8** | 增加 NSBundle 本地化字符串替换(彩蛋) |

---

## License

MIT — 见 [LICENSE](LICENSE)
