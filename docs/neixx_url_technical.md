# neixx/url URL 解析与编码技术设计说明

## 1. 文档目标与范围

本文档描述 `neixx/url` 中 `Url`（RFC 3986 / WHATWG 兼容解析）与 `UrlEncode` / `UrlEncodeQuery` / `UrlDecode`（百分号编码）的设计目标、内部机制、解析算法与内存模型。

本文档基于：

- `modules/neixx/url/include/neixx/url/url.h`（公开 API）
- `modules/neixx/url/include/neixx/url/url_encoding.h`
- `modules/neixx/url/src/url.cpp`（内部实现）
- `modules/neixx/url/src/url_encoding.cpp`

## 2. 模块定位

| 组件 | 定位 | 对标 Chromium |
|------|------|--------------|
| `Url` | URL 解析 + 组件访问 + 相对引用解析 | `GURL` |
| `UrlEncode` 族 | 百分号编码/解码 | `base::Escape*` |

## 3. Url — 零拷贝解析

### 3.1 内存模型：单字符串 + 组件偏移

```
Url::Impl {
  std::string spec;                       // 原始 URL 串（唯一分配）
  size_t scheme_begin/end, user_*,        // ★ 各组件为 spec 的偏移量对
         pass_*, host_*, port_*,
         path_*, query_*, frag_*;
  uint16_t port_value;                    // 已解析端口数值
  bool port_explicit;                     // 是否显式写出端口
  bool valid;
  std::string origin_cache;               // origin 惰性缓存
}
```

- 访问器返回 `std::string_view` 切片——**零 per-component 分配**
- `Reset()` 保留 `spec`（所有 view 的来源），仅清偏移量
- `origin()` 惰性计算并缓存（`origin_cache` 非空直接返回）

### 3.2 解析算法

1. **scheme**：`scheme:` 前缀；http/https/ws/wss 走 WHATWG 语义，其余 RFC 3986
2. **authority**：`user[:pass]@host[:port]`
   - IPv6 字面量：`EatIPv6Literal` 消费 `[...]` 方括号
   - 普通主机：`EatHost` 扫到 `:`/`/`/`?`/`#` 为止
   - 主机**小写化**（RFC 3986 §6.2.2.1）
3. **port**：`DefaultPortForScheme`（IANA/WHATWG 默认端口表）：

| scheme | 默认端口 |
|--------|:---:|
| http / ws | 80 |
| https / wss | 443 |
| ftp | 21 |
| ssh | 22 |
| 其他 | 0 |

   `port()` 在未显式写出时返回**默认端口**；`origin()` 只在非默认端口时才拼 `:port`

4. **path/query/fragment**：按分隔符切分（`/`、`?`、`#`）

### 3.3 相对引用解析（RFC 3986 §5）

`base.Resolve(relative)` 支持：绝对路径、含 `.`/`..` 的相对路径、仅 query、仅 fragment。
路径规范化 `NormalizePathSegments`（RFC 3986 §5.2.4）：

```cpp
// "." 丢弃；".." 弹出上一段；保留前导/尾随斜杠
if (seg == "..") { if (!segments.empty()) segments.pop_back(); }
else if (seg != "." && !seg.empty()) segments.push_back(seg);
```

### 3.4 API

```cpp
class NEI_API Url {
public:
  Url();
  explicit Url(std::string url);

  std::string_view scheme() const;      // 零拷贝视图
  std::string_view user() const;
  std::string_view password() const;
  std::string_view host() const;
  std::uint16_t port() const;           // 缺省 → 默认端口
  std::string_view path() const;
  std::string_view query() const;
  std::string_view fragment() const;

  bool is_valid() const;
  bool is_empty() const;
  std::string origin() const;           // scheme://host[:非默认端口]（惰性缓存）
  const std::string &spec() const;
  Url Resolve(std::string_view relative) const;   // RFC 3986 §5

  friend bool operator==(const Url &, const Url &);
  friend std::ostream &operator<<(std::ostream &, const Url &);
};
```

```cpp
Url u("https://user:pass@example.com:8080/path?a=1#frag");
u.scheme();    // "https"   u.port();  // 8080
u.origin();    // "https://example.com:8080"（8080 非默认所以保留）
Url("http://example.com").origin();    // "http://example.com"（80 默认省略）
```

## 4. 百分号编码（application/x-www-form-urlencoded）

| 函数 | 语义 |
|------|------|
| `UrlEncode(raw)` | RFC 3986 §2.3 unreserved 直通；其余 `%XX`；空格 → `%20`（**非 '+'**） |
| `UrlEncodeQuery(raw)` | 同 UrlEncode，但空格 → `+`（query/POST body 约定） |
| `UrlDecode(encoded)` | `%XX` → 原始字节；`+` → 空格；非法序列（`%` 后非两位 hex）原样透传 |

```cpp
NEI_API std::string UrlEncode(std::string_view raw);
NEI_API std::string UrlEncodeQuery(std::string_view raw);
NEI_API std::string UrlDecode(std::string_view encoded);
```

## 5. 设计要点

- **零拷贝组件访问**：偏移量切片替代子串分配（HTTP 请求行/URL 高频解析场景）
- **标准对齐**：http(s)/ws(s) 遵循 WHATWG，其余回退 RFC 3986；主机小写化符合 §6.2.2.1
- **默认端口语义**：`port()` 折叠默认端口，`origin()` 仅在显式非默认端口时保留——与浏览器同源判断一致
- **惰性缓存**：origin 仅首次计算时构造
