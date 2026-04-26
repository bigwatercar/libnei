# CommandLine 模块技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/command_line` 当前实现的设计目标、数据模型、跨平台解析策略、接口语义与典型用法。

本文档重点覆盖：

- Chromium 风格命令行解析与拼装
- Windows/POSIX 差异行为
- wrapper + child 双阶段命令行模型
- switch 复制策略与过滤器
- 全部公开接口的使用说明

本文档基于当前头文件与实现：

- `modules/neixx/command_line/include/neixx/command_line/command_line.h`
- `modules/neixx/command_line/src/command_line.cpp`

## 2. 模块定位

`CommandLine` 是 `neixx` 层的基础设施组件，用于统一处理：

- 进程启动参数解析
- 开关（switch）查询与追加
- 子进程命令行拼装
- wrapper（launcher/sandbox）前置封装

模块目标是“行为可预测 + 接口边界清晰”，而不是做 shell 解释器或平台专有命令解析器。

## 3. 数据模型

`CommandLine` 逻辑上维护三份视图：

1. wrapper 视图：仅 wrapper token（例如 sandbox、launcher）
2. raw 视图：仅 child 原始 token（程序名、switch、位置参数）
3. full 视图：wrapper + raw 拼接后的完整启动命令行

内部还维护：

- `switches`：规范化后的 `name -> value` 映射（大小写按 ASCII 折叠）
- `args`：位置参数（非 switch，且 `--` 终止符后的 token 也归入 args）

## 4. 跨平台解析行为

### 4.1 POSIX

- switch 前缀：`-`、`--`
- 值分隔：`=`
- 终止符：`--`

### 4.2 Windows

- switch 前缀：`-`、`--`、`/`
- 值分隔：`=`，以及 `/name:value` 的 `:`
- 终止符：`--`

### 4.3 编码策略

- 内部以 UTF-16（`std::u16string`）处理 token
- UTF-8/UTF-16 转换复用 `neixx/strings/utf_string_conversions.h` 中现有接口
- 不在 `command_line` 内重复实现编码转换

## 5. 公开类型与策略

### 5.1 容器别名

- `StringVector`：`std::vector<std::u16string>`
- `SwitchMap`：`std::map<std::u16string, std::u16string>`

### 5.2 DuplicateSwitchPolicy

- `kReplaceExisting`：同名 switch 以新值覆盖旧值
- `kPreserveExisting`：保留已有值，忽略新值

### 5.3 SwitchValueFilter

用于 `CopySwitchesOptions` 的值筛选，避免布尔参数组合误用：

- `kAll`：复制全部 switch
- `kWithValueOnly`：仅复制“有值”开关（例如 `--name=value`）
- `kWithoutValueOnly`：仅复制“布尔开关”（例如 `--flag`）

### 5.4 CopySwitchesOptions

字段语义：

- `whitelist`：仅复制白名单中的 switch（空表示不过滤）
- `blacklist`：从候选中排除黑名单 switch
- `value_filter`：值筛选规则（见 `SwitchValueFilter`）
- `policy`：重复开关处理策略（见 `DuplicateSwitchPolicy`）

执行顺序：

1. 先应用 `whitelist`
2. 再应用 `blacklist`
3. 再应用 `value_filter`
4. 最后按 `policy` 写入目标

## 6. 接口使用说明（逐项）

以下示例默认：

```cpp
#include <neixx/command_line/command_line.h>
```

### 6.1 构造与全局实例

#### `CommandLine()`

- Windows：从当前进程命令行初始化
- 其他平台：构造空对象（通常使用 `CommandLine(argc, argv)`）

#### `CommandLine(int argc, const char* const* argv)`

- 从给定 argv 解析 raw/switch/args
- 推荐在测试和子命令拼装中使用

示例：

```cpp
const char* argv[] = {"prog", "--name=value", "input.txt"};
nei::CommandLine cmd(static_cast<int>(std::size(argv)), argv);
```

#### `static void Init(int argc, const char* const* argv)`

- 初始化当前进程全局实例（POSIX/通用入口）

#### `static void Init()`（Windows）

- 从 `GetCommandLineW` 初始化当前进程全局实例

#### `static CommandLine& ForCurrentProcess()`

- 获取全局实例
- 若未初始化，会按平台默认路径懒初始化

### 6.2 Program 查询

#### `std::string GetProgram() const`
#### `std::u16string GetProgramUTF16() const`

- 返回 child program（raw argv[0]）
- 与 wrapper 前缀无关
- 空命令行时返回空字符串

示例：

```cpp
std::string prog = cmd.GetProgram();
std::u16string prog16 = cmd.GetProgramUTF16();
```

### 6.3 Switch 查询

#### `bool HasSwitch(std::string_view name) const`
#### `bool HasSwitch(std::u16string_view name) const`

- 判断开关是否存在
- `name` 支持传入带前缀或不带前缀，内部会规范化
- 按 ASCII 大小写不敏感比较

#### `std::string GetSwitchValueASCII(std::string_view name) const`
#### `std::string GetSwitchValueASCII(std::u16string_view name) const`

- 返回 UTF-8 字符串值
- 不存在或无值返回空字符串

#### `std::u16string GetSwitchValueUTF16(std::string_view name) const`
#### `std::u16string GetSwitchValueUTF16(std::u16string_view name) const`

- 返回 UTF-16 字符串值
- 不存在或无值返回空 `u16string`

#### `const SwitchMap& GetSwitches() const`

- 返回只读 switch 映射视图（规范化后键）

示例：

```cpp
if (cmd.HasSwitch(u"name")) {
  std::u16string v = cmd.GetSwitchValueUTF16(u"name");
}
```

### 6.4 Switch 追加

#### `void AppendSwitch(std::string_view name, DuplicateSwitchPolicy policy = kReplaceExisting)`

- 追加无值开关（布尔开关）

#### `void AppendSwitchASCII(std::string_view name, std::string_view value, DuplicateSwitchPolicy policy = kReplaceExisting)`

- 追加 UTF-8 名称/值开关

#### `void AppendSwitchUTF16(std::u16string_view name, std::u16string_view value, DuplicateSwitchPolicy policy = kReplaceExisting)`

- 追加 UTF-16 名称/值开关
- 适合 Windows 路径场景，避免绕 UTF-8

追加位置规则：

- 新 switch 会插入到“位置参数或 `--` 终止符”之前
- 不会打乱已有 args 语义

重复策略规则：

- `kReplaceExisting`：更新 map 并替换 argv 中旧 token
- `kPreserveExisting`：若已存在同名 switch，忽略本次追加

示例：

```cpp
cmd.AppendSwitchASCII("mode", "fast");
cmd.AppendSwitchUTF16(u"path", u"C:/测试/文件.txt");
```

### 6.5 Switch 复制

#### `void CopySwitchesFrom(const CommandLine& source, const std::vector<std::string>& whitelist = {}, DuplicateSwitchPolicy policy = kReplaceExisting)`

- 兼容接口：可选白名单 + 重复策略

#### `void CopySwitchesFrom(const CommandLine& source, const CopySwitchesOptions& options)`

- 细粒度复制接口

典型场景：

- 复制父进程部分开关到子进程
- 排除敏感开关（token、密钥等）
- 仅复制布尔开关或仅复制有值开关

示例：

```cpp
nei::CommandLine::CopySwitchesOptions opt;
opt.whitelist = {"keep", "trace", "profile"};
opt.blacklist = {"secret", "token"};
opt.value_filter = nei::CommandLine::SwitchValueFilter::kWithValueOnly;
opt.policy = nei::CommandLine::DuplicateSwitchPolicy::kReplaceExisting;

child.CopySwitchesFrom(parent, opt);
```

### 6.6 Wrapper 前置

#### `void PrependWrapper(const CommandLine& wrapper)`

- 用已有 `CommandLine` 作为 wrapper 前缀

#### `void PrependWrapper(std::string_view wrapper_program)`
#### `void PrependWrapper(std::string_view wrapper_program, const std::vector<std::string>& wrapper_args)`

- UTF-8 便捷重载

#### `void PrependWrapperUTF16(std::u16string_view wrapper_program)`
#### `void PrependWrapperUTF16(std::u16string_view wrapper_program, const StringVector& wrapper_args)`

- UTF-16 便捷重载

语义要点：

- wrapper 仅影响 full 视图，不改变 child 自身 program/switch/args 语义
- 可多次 prepend（后调用的 wrapper 放在更前面）

示例：

```cpp
child.PrependWrapper("sandbox", {"--trace"});
// full: sandbox --trace child ...
```

### 6.7 Args 与 argv 视图

#### `std::vector<std::string> GetArgs() const`
#### `const StringVector& GetArgsUTF16() const`

- 返回位置参数（含 `--` 终止符后的 token）

#### `const StringVector& GetWrapperArgv() const`

- 仅 wrapper token 视图

#### `const StringVector& GetRawArgv() const`

- 仅 child 原始 token 视图

#### `const StringVector& GetFullArgv() const`

- wrapper + raw 拼接后的完整视图

#### `const StringVector& argv() const`

- 兼容接口，当前等价于 `GetFullArgv()`

### 6.8 追加参数

#### `void AppendArg(std::string_view value)`

- 追加位置参数到 raw argv 尾部

#### `void AppendArguments(const CommandLine& source, bool include_program)`

- 把 `source` 的 argv token 合并到当前对象
- `include_program = false` 时跳过 source 的 argv[0]
- 合并后会重新解析 raw/switch/args

### 6.9 输出完整命令行

#### `std::string GetCommandLineString() const`

- 输出 full 视图命令行字符串
- 自动处理空格/引号/反斜杠转义

## 7. 典型用法

### 7.1 进程初始化

```cpp
int main(int argc, char** argv) {
#if defined(_WIN32)
  nei::CommandLine::Init();
#else
  nei::CommandLine::Init(argc, argv);
#endif

  nei::CommandLine& cl = nei::CommandLine::ForCurrentProcess();
  if (cl.HasSwitch("test")) {
    // ...
  }
}
```

### 7.2 子进程命令拼装

```cpp
const char* child_argv[] = {"child", "--child-flag"};
nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);

// 复制父进程可转发开关
nei::CommandLine::CopySwitchesOptions opt;
opt.whitelist = {"lang", "user-data-dir", "trace"};
opt.blacklist = {"token"};
opt.value_filter = nei::CommandLine::SwitchValueFilter::kAll;
child.CopySwitchesFrom(nei::CommandLine::ForCurrentProcess(), opt);

// 前置 sandbox wrapper
child.PrependWrapper("sandbox", {"--trace"});

std::string full_cmd = child.GetCommandLineString();
```

## 8. 边界与注意事项

1. `GetProgram*` 读取的是 raw program，不包含 wrapper program
2. `argv()` 当前返回 full 视图，为兼容保留；新代码建议显式使用 `GetRawArgv()` / `GetFullArgv()`
3. 空值开关合法，表现为 `--flag`，其 value 为空
4. `CopySwitchesOptions` 中若同时配置 whitelist 与 blacklist，blacklist 具有最终排除效果
5. `SwitchValueFilter` 与 `DuplicateSwitchPolicy` 是独立维度：先筛选、后按重复策略写入

## 9. 测试覆盖（摘要）

当前 `tests/command_line_test.cpp` 已覆盖：

- POSIX 风格解析
- Windows `/name:value` 解析
- `--` 终止符行为
- UTF-16 switch 追加与查询
- wrapper/raw/full 三视图
- `CopySwitchesOptions` 的白名单、黑名单、值筛选
- 重复开关保留与替换策略

建议后续新增：

- 多层 wrapper 叠加顺序测试
- `AppendArguments` 与 wrapper 组合场景测试
- 更复杂引号/反斜杠边界字符串回归样例
