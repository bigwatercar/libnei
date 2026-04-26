# CommandLine 接口速查表

> 面向调用方的快速查用文档。
> 详细设计见 `docs/neixx_command_line_technical.md`。

## 1. 最小初始化

```cpp
#include <neixx/command_line/command_line.h>

int main(int argc, char** argv) {
#if defined(_WIN32)
  nei::CommandLine::Init();
#else
  nei::CommandLine::Init(argc, argv);
#endif

  nei::CommandLine& cl = nei::CommandLine::ForCurrentProcess();
  return 0;
}
```

## 2. 快速查询

### Program

```cpp
std::string prog = cl.GetProgram();
std::u16string prog16 = cl.GetProgramUTF16();
```

### Switch（UTF-8 / UTF-16）

```cpp
bool has_test = cl.HasSwitch("test");
bool has_test16 = cl.HasSwitch(u"test");

std::string name = cl.GetSwitchValueASCII("name");
std::string name16_as_utf8 = cl.GetSwitchValueASCII(u"name");

std::u16string path16 = cl.GetSwitchValueUTF16("path");
std::u16string path16_2 = cl.GetSwitchValueUTF16(u"path");
```

## 3. 追加开关

### 默认策略（同名覆盖）

```cpp
cl.AppendSwitch("enable-feature");
cl.AppendSwitchASCII("mode", "fast");
cl.AppendSwitchUTF16(u"path", u"C:/\u6d4b\u8bd5/\u6587\u4ef6.txt");
```

### 保留已有值（同名忽略）

```cpp
cl.AppendSwitchASCII("mode",
                     "new",
                     nei::CommandLine::DuplicateSwitchPolicy::kPreserveExisting);
```

## 4. 复制开关

### 简单版：白名单 + 重复策略

```cpp
child.CopySwitchesFrom(parent,
                       {"lang", "user-data-dir"},
                       nei::CommandLine::DuplicateSwitchPolicy::kReplaceExisting);
```

### 高级版：CopySwitchesOptions

```cpp
nei::CommandLine::CopySwitchesOptions opt;
opt.whitelist = {"keep", "trace", "profile"};
opt.blacklist = {"secret", "token"};
opt.value_filter = nei::CommandLine::SwitchValueFilter::kAll;
opt.policy = nei::CommandLine::DuplicateSwitchPolicy::kReplaceExisting;

child.CopySwitchesFrom(parent, opt);
```

### value_filter 速查

- `kAll`: 复制全部开关
- `kWithValueOnly`: 仅复制有值开关（如 `--name=value`）
- `kWithoutValueOnly`: 仅复制布尔开关（如 `--flag`）

## 5. Wrapper（launcher/sandbox）

### 直接用 CommandLine 作为 wrapper

```cpp
nei::CommandLine wrapper(static_cast<int>(std::size(wrapper_argv)), wrapper_argv);
child.PrependWrapper(wrapper);
```

### UTF-8 便捷重载

```cpp
child.PrependWrapper("sandbox");
child.PrependWrapper("sandbox", {"--trace", "--profile=dev"});
```

### UTF-16 便捷重载

```cpp
child.PrependWrapperUTF16(u"sandbox");
child.PrependWrapperUTF16(u"sandbox", {u"--profile", u"C:/\u6d4b\u8bd5"});
```

## 6. 参数与视图

### 位置参数

```cpp
std::vector<std::string> args = cl.GetArgs();
const nei::CommandLine::StringVector& args16 = cl.GetArgsUTF16();
```

### 三视图（wrapper/raw/full）

```cpp
const auto& wrapper = cl.GetWrapperArgv(); // 仅 wrapper
const auto& raw = cl.GetRawArgv();         // 仅 child 原始 argv
const auto& full = cl.GetFullArgv();       // wrapper + child

const auto& compat = cl.argv();            // 兼容接口，等价 full
```

## 7. 子进程拼装

```cpp
// child 初始命令
const char* child_argv[] = {"child", "--child-flag"};
nei::CommandLine child(static_cast<int>(std::size(child_argv)), child_argv);

// 复制父进程开关
nei::CommandLine::CopySwitchesOptions opt;
opt.whitelist = {"lang", "trace"};
opt.value_filter = nei::CommandLine::SwitchValueFilter::kAll;
child.CopySwitchesFrom(nei::CommandLine::ForCurrentProcess(), opt);

// 追加 wrapper
child.PrependWrapper("sandbox", {"--trace"});

// 输出完整命令行
std::string cmdline = child.GetCommandLineString();
```

## 8. 常见注意点

1. `GetProgram*()` 读取的是 child program，不是 wrapper program。
2. `argv()` 是兼容接口，当前等价 `GetFullArgv()`。
3. 空值开关合法，输出形态为 `--flag`。
4. `CopySwitchesOptions` 执行顺序：white -> black -> value_filter -> duplicate policy。
5. Windows 建议优先使用 UTF-16 相关接口处理路径参数，减少编码往返。

## 9. 常见错误对照

### 9.1 把 full 视图当成 child 原始视图

错误写法：

```cpp
const auto& tokens = cmd.argv();
// 假设 tokens[0] 一定是 child program
```

推荐写法：

```cpp
const auto& raw = cmd.GetRawArgv();
const auto& full = cmd.GetFullArgv();
```

说明：

- `GetRawArgv()` 只含 child token
- `GetFullArgv()` 含 wrapper + child token
- `argv()` 为兼容接口，当前等价 full 视图

### 9.2 需要 Windows 路径时仍走 ASCII 值接口

错误写法：

```cpp
cmd.AppendSwitchASCII("path", "C:/测试/文件.txt");
```

推荐写法：

```cpp
cmd.AppendSwitchUTF16(u"path", u"C:/\u6d4b\u8bd5/\u6587\u4ef6.txt");
```

说明：

- UTF-16 接口更适合 Windows 路径参数，避免不必要编码往返

### 9.3 同时想“只复制有值开关”和“只复制布尔开关”

错误写法（旧思维）：

```cpp
// 试图通过多个布尔参数叠加过滤
```

推荐写法：

```cpp
nei::CommandLine::CopySwitchesOptions opt;
opt.value_filter = nei::CommandLine::SwitchValueFilter::kWithValueOnly;
// 或 kWithoutValueOnly
```

说明：

- `SwitchValueFilter` 是互斥枚举，避免组合误用

### 9.4 复制开关时忘记排除敏感项

错误写法：

```cpp
child.CopySwitchesFrom(parent);
```

推荐写法：

```cpp
nei::CommandLine::CopySwitchesOptions opt;
opt.blacklist = {"token", "secret", "auth"};
child.CopySwitchesFrom(parent, opt);
```

说明：

- 子进程拼装建议默认考虑敏感开关泄漏风险

### 9.5 期望 PrependWrapper 修改 child 语义

错误理解：

- prepend 后 `GetProgram()` 应该返回 wrapper program

正确理解：

- `GetProgram()` / `GetRawArgv()` 始终描述 child
- wrapper 只影响 `GetWrapperArgv()` / `GetFullArgv()` / `GetCommandLineString()`
