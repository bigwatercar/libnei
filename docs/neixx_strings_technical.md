# Strings 模块技术设计说明

## 1. 文档目标与范围

本文档面向 `neixx/strings` 的使用方与维护方，描述当前 strings 模块的设计目标、能力边界、跨平台编码策略、API 组织方式、ABI/性能取舍，以及测试与示例的覆盖情况。

本文档覆盖的能力包括：

- UTF-8 / UTF-16 编码转换
- 基础字符串工具（前后缀判断、裁剪、ASCII 大小写转换、格式化）
- 字符串分割与合并
- 数字与字符串之间的安全转换
- 十六进制编码

本文仅描述当前已实现方案，不展开历史演进过程。

## 2. 模块定位

`neixx/strings` 是 `neixx` 层的基础字符串支撑模块，目标不是实现完整国际化框架，而是提供一组：

- 跨平台行为可预期
- 接口边界清晰
- 不依赖大型外部库
- 适合基础设施层复用

的字符串基础能力。

该模块优先服务于如下工程场景：

- 配置与协议字段处理
- 路径、键名、标识符等 ASCII 主导文本处理
- UTF-8 / UTF-16 跨平台桥接
- 数字配置项、坐标值、参数值的严格解析
- 调试、哈希、二进制可视化时的十六进制输出

## 3. 当前 API 组成

### 3.1 UTF 编码转换

头文件：

- `modules/neixx/strings/include/neixx/strings/utf_string_conversions.h`

能力：

- `UTF8ToUTF16`
- `UTF16ToUTF8`
- `ASCIIToUTF16`

### 3.2 基础字符串工具

头文件：

- `modules/neixx/strings/include/neixx/strings/string_util.h`

能力：

- `StartsWith`
- `EndsWith`
- `TrimWhitespace`
- `StringPrintf`
- `StringAppendF`
- `ToLowerASCII`
- `ToUpperASCII`

### 3.3 字符串分割与合并

头文件：

- `modules/neixx/strings/include/neixx/strings/split_string.h`

能力：

- `SplitString`（支持单字符分隔符与字符串分隔符）
- `JoinString`
- `WhitespaceHandling`
- `SplitResult`

### 3.4 数字转换与十六进制编码

头文件：

- `modules/neixx/strings/include/neixx/strings/string_number_conversions.h`

能力：

- `IntToString`
- `Int64ToString`
- `NumberToString`
- `IntToString16`
- `Int64ToString16`
- `NumberToString16`
- `StringToUint`
- `StringToInt64`
- `StringToDouble`
- `HexEncode`

## 4. 设计目标

### 4.1 跨平台一致性优先

strings 模块优先保证 Windows / Linux / POSIX 下行为的一致性，而不是依赖各平台标准库或 C 运行时对宽字符和本地化行为的不同默认实现。

### 4.2 安全解析优先于“宽松成功”

对于数字解析类接口，设计目标是：

- 输入必须整体合法才返回成功
- 不接受“前缀合法、后缀脏数据”的宽松解析
- 不使用异常作为失败通道

这类策略更适合配置解析、几何坐标、协议字段、文件元信息等基础设施场景。

### 4.3 轻依赖

当前实现不引入 ICU 等大型国际化库。

这意味着：

- 常见跨平台字符串能力可以独立提供
- 复杂 locale / collation / normalization 不在本模块目标范围内

### 4.4 API 边界清晰

strings 模块强调“告诉调用者这是什么能力，而不是伪装成更通用的能力”。

例如：

- `ToLowerASCII` 而不是 `ToLower`
- `StringToInt64(..., &out)` 返回 `bool`，而不是抛异常
- `StringPrintf` 明确定位为 UTF-8 `std::string` 格式化工具

## 5. 跨平台编码设计

### 5.1 为什么统一使用 `std::u16string`

关于 `std::u16string` 与 `wchar_t`：

- Windows 上 `sizeof(wchar_t) == 2`
- Linux / POSIX 上 `sizeof(wchar_t)` 通常为 `4`

如果直接把宽字符串语义建立在 `wchar_t` 之上，则相同接口在不同平台上的编码单元宽度会发生变化，导致：

- 内存布局不一致
- API 语义不稳定
- 跨平台桥接成本上升

Chromium 持续使用 `std::u16string` 的核心原因，就是为了在跨平台环境中固定 UTF-16 语义。当前模块遵循这一取向。

### 5.2 Windows 为什么不需要手工处理代理对

Windows 平台的 UTF-8 -> UTF-16 转换使用系统 API：

- `MultiByteToWideChar`

这意味着：

- 系统已经内置了 UTF-8 到 UTF-16 的严谨转换逻辑
- 对于 4 字节 UTF-8（例如 emoji）
- 系统 API 会自动产出对应的 UTF-16 高低代理对

当前代码在 Windows API 桥接时只需要：

- 准备好输入与缓冲区
- 在必要处使用 `reinterpret_cast<const wchar_t*>` / 对应非 const 形式桥接到 Windows API

不需要在业务代码中重复实现 surrogate pair 拆分逻辑。

### 5.3 Linux / POSIX 为什么必须特别强调代理对支持

在 Linux / POSIX 路径下，当前实现不依赖 ICU，而是手工完成 UTF-8 / UTF-16 编解码。

因此责任转移到了模块实现自身：

- 必须显式识别 1 / 2 / 3 / 4 字节 UTF-8 序列
- 对于非 BMP 码点，必须生成 UTF-16 高低代理对
- 对于非法序列或非法 surrogate 组合，必须给出替代策略

这点必须特别强调，因为很多“简单手写版”UTF-8 解码器只覆盖 1 到 3 字节 UTF-8，一旦遇到：

- emoji
- 部分历史文字
- 音乐符号
- 其他非 BMP 字符

如果没有 UTF-16 surrogate pair 生成逻辑，就会出现：

- 转换失败
- 输出乱码
- 非法 UTF-16 序列

当前模块已经通过测试锁定了这部分行为。

### 5.4 错误处理策略

当前 UTF 编码转换采用“替换非法序列”的思路，而不是异常失败：

- `UTF8ToUTF16`：对非法 UTF-8 使用替代字符
- `UTF16ToUTF8`：对非法 surrogate 组合使用替代字符
- `ASCIIToUTF16`：对非 ASCII 字节执行替代

该策略的优点是：

- 行为稳定
- 不把基础字符串工具变成异常传播源
- 更适合日志、配置、调试输出等容错场景

## 6. 字符串工具设计

### 6.1 `StartsWith` / `EndsWith`

这类接口是高频基础能力，主要服务于：

- 协议头判断
- 文件扩展名判断
- 命令前缀识别
- 路径或 key 的模式匹配

当前支持：

- 大小写敏感
- `kInsensitiveASCII`

这里特意不是“完全 Unicode case-insensitive compare”，而是面向工程基础设施中的 ASCII 场景。

### 6.2 `TrimWhitespace`

`TrimWhitespace` 提供：

- `kLeading`
- `kTrailing`
- `kAll`

该接口同时被分割逻辑复用，用于在 `TRIM_WHITESPACE` 模式下裁掉 token 两端的 ASCII 空白。

### 6.3 为什么是 `ToLowerASCII` / `ToUpperASCII`

命名为 `ToLowerASCII` 而不是 `ToLower`，是有意为之。

它明确告诉调用者：

- 只处理 ASCII `A-Z` / `a-z`
- 不处理复杂 locale 规则
- 不处理德语变音、土耳其语 `i` 等本地化语义

这类接口适合：

- 文件路径比较
- 协议头规范化
- 标识符和关键字处理

其价值在于：

- 行为确定
- 性能稳定
- 不受进程 locale 影响

## 7. 分割与合并设计

### 7.1 为什么 `SplitString` 是高频基础能力

工程里大量输入本质上都是“待切分文本”：

- CSV 片段
- 配置项
- 协议头字段
- 命令行参数
- 查询串
- 路径片段

真正麻烦的往往不是“切分动作”，而是：

- 要不要保留空项
- 要不要先裁空白

因此 strings 模块把这两个选择显式建模为：

- `WhitespaceHandling`
- `SplitResult`

### 7.2 `SPLIT_WANT_NONEMPTY` 的价值

例如处理：

```text
a,,b
```

如果业务语义是不关心空项，那么库层直接支持 `SPLIT_WANT_NONEMPTY`，可以显著减少调用端反复编写：

```cpp
if (!item.empty()) {
  ...
}
```

这样做的收益是：

- 业务代码更短
- 空项过滤语义更一致
- 少写重复判断分支

### 7.3 `JoinString`

`JoinString` 与 `SplitString` 形成基础闭环。

典型场景：

- 重新拼接路径/字段列表
- 输出调试信息
- 规范化 token 列表

当前分别支持 `std::string` 与 `std::u16string` 的容器版本。

## 8. 数字转换设计

### 8.1 为什么字符串转数字必须严格

`std::stoi` / `std::stoll` 一类接口通常具有“前缀成功即可返回”的宽松倾向，例如：

```text
123abc
```

可能被解析成 `123`。

这在以下场景中是危险的：

- 配置项
- CAD / 几何坐标
- 数值参数
- 文件格式字段

因为它会把“脏输入”伪装成“解析成功”。

当前模块的 `StringToUint` / `StringToInt64` / `StringToDouble` 统一要求：

- 全量输入必须被消费
- 不能溢出
- 调用方通过 `bool` 判断成功与否
- 不抛异常

### 8.2 为什么 `StringPrintf` 只返回 UTF-8 `std::string`

Chromium 风格下，`StringPrintf` 通常只返回 `std::string`（UTF-8）。当前模块采用同样策略。

如果业务需要 UTF-16 格式化输出，推荐流程是：

1. 先调用 `StringPrintf` 生成 UTF-8 文本
2. 再调用 `UTF8ToUTF16`

不推荐手写“跨平台 UTF-16 printf 封装”，原因是：

- `vswprintf` 在不同平台行为差异明显
- `%ls` 与 UTF-16 的配合并不稳定
- 很容易引入 ABI 和行为不一致问题

### 8.3 浮点数输出

`NumberToString` 使用 locale-independent 的输出路径，避免数字文本受进程 locale 影响。

这对于以下场景尤为重要：

- 配置持久化
- 文本协议
- 调试输出
- 测试 golden 数据

## 9. 十六进制编码设计

`HexEncode` 是典型的高频基础能力。

典型使用场景：

- 文件哈希文本化
- 二进制 buffer 调试输出
- RGB / RGBA 等颜色值输出
- 网络报文与内存可视化

这类能力分散在业务层重复实现没有价值，因此归入 strings 基础模块。

当前行为：

- 输入视为原始 bytes
- 输出大写十六进制文本

## 10. ABI 与性能取舍

### 10.1 `NEI_API` 导出

当前公开函数统一使用 `NEI_API` 导出，以保持模块边界清晰，并支持共享库形式的稳定符号暴露。

### 10.2 `std::string_view` / `std::u16string_view`

大部分只读输入接口采用 view 类型：

- 减少无意义拷贝
- 支持字面量、`std::string`、子串等多种调用方式
- 让实现把内存分配留给真正需要拥有结果的返回值

### 10.3 inline 与 `.cpp` 的边界

对于极简且热点明显的函数，例如非常短小的前后缀判断，理论上可以考虑 header 内 `inline`。

但对于：

- `StringPrintf`
- 较复杂的 `TrimWhitespace`
- 分割/数值解析这类实现细节较多的逻辑

当前更适合放在 `.cpp` 中，以获得：

- 更稳定的 ABI 边界
- 更小的头文件膨胀
- 更少的编译传播成本

当前实现整体遵循“简单声明放头文件，复杂实现放 `.cpp`”的工程策略。

## 11. 代码组织

### 11.1 头文件

- `modules/neixx/strings/include/neixx/strings/utf_string_conversions.h`
- `modules/neixx/strings/include/neixx/strings/string_util.h`
- `modules/neixx/strings/include/neixx/strings/split_string.h`
- `modules/neixx/strings/include/neixx/strings/string_number_conversions.h`

### 11.2 源文件

- `modules/neixx/strings/src/utf_string_conversions.cpp`
- `modules/neixx/strings/src/utf_string_conversions_win.cpp`
- `modules/neixx/strings/src/utf_string_conversions_posix.cpp`
- `modules/neixx/strings/src/string_util.cpp`
- `modules/neixx/strings/src/split_string.cpp`
- `modules/neixx/strings/src/string_number_conversions.cpp`

### 11.3 CMake 接线

当前 strings 模块在：

- `modules/neixx/strings/CMakeLists.txt`

中接入到 `neixx` 目标，并按平台选择：

- Windows：`utf_string_conversions_win.cpp`
- UNIX / APPLE：`utf_string_conversions_posix.cpp`

## 12. 示例

### 12.1 CSV 分割与安全数值转换

```cpp
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <neixx/strings/split_string.h>
#include <neixx/strings/string_number_conversions.h>

const std::string csv = " 42, , 7, -3 , word, 100 ";
const std::vector<std::string> parts =
    nei::SplitString(csv, ',', nei::TRIM_WHITESPACE, nei::SPLIT_WANT_ALL);

for (const std::string& part : parts) {
  std::int64_t value = 0;
  if (nei::StringToInt64(part, &value)) {
    std::cout << value << "\n";
  }
}
```

该示例体现了两个设计点：

- 空项和空白处理应在 split 层显式表达
- 数值转换应通过 `bool` 返回严格判断成功与否

### 12.2 UTF-8 -> UTF-16 -> UTF-8

```cpp
const std::string utf8 = "hello, 世界";
const std::u16string utf16 = nei::UTF8ToUTF16(utf8);
const std::string roundtrip = nei::UTF16ToUTF8(utf16);
```

## 13. 测试与验证

当前 strings 模块已有如下测试覆盖：

- `tests/utf_string_conversions_test.cpp`
- `tests/string_util_test.cpp`
- `tests/split_string_test.cpp`
- `tests/string_number_conversions_test.cpp`

重点覆盖点包括：

- emoji surrogate pair
- 非法 surrogate 替代行为
- `StringPrintf` 与 `StringAppendF` 的边界长度
- `SplitString` 的空项与裁剪策略
- 数字转换的严格全匹配行为
- `HexEncode` 的稳定输出

示例程序位于：

- `demo/string_util_demo.cpp`

基准程序位于：

- `bench/string_append_bench.cpp`

## 14. 当前边界与后续可扩展点

当前模块**不**覆盖：

- locale-aware 大小写折叠
- Unicode normalization
- collation / 排序规则
- ICU 级别的国际化能力
- 十六进制解码

如果后续需要扩展，可优先考虑：

1. `HexDecode`
2. 更完整的 safe number conversions（如 `StringToInt`、`StringToUint64`）
3. 更丰富的 split 变体（如 key/value split helpers）
4. 文档化更多 UTF 边界输入与错误恢复策略

## 15. 工程结论

`neixx/strings` 当前的定位非常明确：

- 不追求“大而全”
- 优先提供工程高频、边界清晰、跨平台一致的基础字符串能力

其核心设计原则可以归纳为：

- UTF 语义固定在 `std::u16string`
- Windows 尽量复用系统编码能力
- POSIX 手工路径必须显式处理 surrogate pair
- ASCII-only 行为要在命名上直接说明
- 数值解析必须严格，不容忍脏后缀
- 复杂实现放 `.cpp`，保持 ABI 边界稳定

在当前工程规模下，这是一组足够实用、可维护、可持续扩展的字符串基础设施能力。