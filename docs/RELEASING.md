# libnei Release Checklist

## 版本号单一来源

所有版本信息由仓库根 `CMakeLists.txt` 的 `project(nei VERSION x.y.z)` 派生:

| 派生点 | 机制 |
|--------|------|
| 编译期宏 `NEI_VERSION_MAJOR/MINOR/PATCH/STRING/HEX` | `modules/nei/build/version.h.in` 经 `configure_file` 生成 |
| 运行期 API `nei_get_version_string()` / `nei_get_version_info()` | 消费生成头(`modules/nei/core/version.c`) |
| 共享库后缀 | `VERSION`/`SOVERSION` target 属性(`libnei.so.0.9.0` + `libnei.so.0.9` 符号链接) |
| CMake 包版本 | `neiConfigVersion.cmake`(自动,`find_package(nei 0.9)` 校验) |

**发布时只需修改 `project()` 的 VERSION 一行。**

## SOVERSION 策略

- **0.x 阶段**:`SOVERSION = major.minor`(当前 0.9)。API 无稳定性承诺,
  每次 minor 发布即换 SONAME,依赖方必须重链。
- **1.0 之后**:切换为 `SOVERSION = ${PROJECT_VERSION_MAJOR}`,仅 major 发布破坏 ABI。

## 发布步骤

1. 修改根 `CMakeLists.txt` 的 `project(nei VERSION x.y.z)`(唯一改动点)
2. 同步 `tests/version_test.cpp` 的 `ReportsZeroNineSeries` 基线断言
3. 四象限验证:Windows / WSL × Debug / Release 构建 + 全量测试
4. `git tag -a v<version> -m "libnei v<version>"` 并推送 tag
5. 验证安装树三件套:
   - `libnei.so.<major>.<minor>`(+ `libnei.so.<major>.<minor>.<patch>`)
   - `${prefix}/include/nei/build/version.h`
   - `lib/cmake/nei/neiConfigVersion.cmake`
