# 路径遍历攻击防护演示

## 为什么需要安全检查？

虽然 `full_path = base_path / relative_file` 看起来安全，但 `std::filesystem::canonical()` 会**解析路径中的 `..` 符号**，导致路径可能跳出限制范围。

## 攻击场景

### 场景 1: 成功的攻击（如果没有安全检查）

```cpp
// 配置
base_path = "/var/www/public"  // 规范化的绝对路径

// 恶意请求
GET /static/../../etc/passwd

// 参数解析
params["*"] = "../../etc/passwd"

// 路径构建
full_path = base_path / params["*"]
         = "/var/www/public" / "../../etc/passwd"
         = "/var/www/public/../../etc/passwd"

// 路径规范化（这里是关键！）
canonical(full_path) = "/etc/passwd"

// 如果没有安全检查，服务器会读取并返回 /etc/passwd 的内容！❌
```

### 场景 2: 被阻止的攻击（有安全检查）

```cpp
// 前面步骤相同...
canonical(full_path) = "/etc/passwd"

// 安全检查
full_path_str = "/etc/passwd"
base_path_str = "/var/www/public"

if (full_path_str.substr(0, base_path_str.length()) != base_path_str) {
    // "/etc/passwd".startsWith("/var/www/public") ? NO
    return 403 Forbidden;  // 攻击被阻止！✅
}
```

## 实际测试

### 测试代码

```cpp
#include <filesystem>
#include <iostream>

void testPathTraversal(const std::string& base, const std::string& relative) {
    std::filesystem::path base_path(base);
    std::filesystem::path full_path = base_path / relative;
    
    std::cout << "Base:     " << base_path << std::endl;
    std::cout << "Relative: " << relative << std::endl;
    std::cout << "Combined: " << full_path << std::endl;
    
    if (std::filesystem::exists(full_path)) {
        auto canonical_path = std::filesystem::canonical(full_path);
        std::cout << "Canonical: " << canonical_path << std::endl;
        
        auto canonical_str = canonical_path.string();
        auto base_str = base_path.string();
        
        bool is_safe = (canonical_str.substr(0, base_str.length()) == base_str);
        std::cout << "Is Safe:  " << (is_safe ? "YES ✓" : "NO ✗ (ATTACK DETECTED!)") << std::endl;
    }
    std::cout << std::endl;
}

int main() {
    // 测试正常请求
    std::cout << "=== Normal Request ===" << std::endl;
    testPathTraversal("/var/www/public", "css/style.css");
    
    // 测试路径遍历攻击
    std::cout << "=== Path Traversal Attack ===" << std::endl;
    testPathTraversal("/var/www/public", "../../etc/passwd");
    
    return 0;
}
```

### 预期输出

```
=== Normal Request ===
Base:     /var/www/public
Relative: css/style.css
Combined: /var/www/public/css/style.css
Canonical: /var/www/public/css/style.css
Is Safe:  YES ✓

=== Path Traversal Attack ===
Base:     /var/www/public
Relative: ../../etc/passwd
Combined: /var/www/public/../../etc/passwd
Canonical: /etc/passwd
Is Safe:  NO ✗ (ATTACK DETECTED!)
```

## 为什么 canonical() 会改变路径？

`std::filesystem::canonical()` 的作用：

1. **解析符号链接** (symlinks)
2. **解析相对路径符号**：
   - `.` → 当前目录（移除）
   - `..` → 父目录（向上移动）
3. **返回绝对路径**

### 示例

```cpp
// 路径解析过程
"/var/www/public/../../etc/passwd"

// Step 1: 从左到右处理
/var/www/public  ← 开始位置
           ../   ← 向上一级 → /var/www
              ../ ← 向上一级 → /var
                 etc/passwd ← 拼接 → /var/etc/passwd

// 实际上 /var 的上级是 /，所以：
/var ← 上一级 → /
     etc/passwd → /etc/passwd

// 最终结果
canonical() = "/etc/passwd"
```

## 常见的攻击模式

| 攻击路径 | 规范化后 | 是否安全 |
|---------|---------|---------|
| `css/style.css` | `/var/www/public/css/style.css` | ✅ 安全 |
| `../config.json` | `/var/www/config.json` | ✅ 被拦截 |
| `../../etc/passwd` | `/etc/passwd` | ✅ 被拦截 |
| `../../../../root/.ssh/id_rsa` | `/root/.ssh/id_rsa` | ✅ 被拦截 |
| `./../../../etc/hosts` | `/etc/hosts` | ✅ 被拦截 |
| `static/../../etc/shadow` | `/etc/shadow` | ✅ 被拦截 |

## 安全检查的重要性

### ❌ 没有安全检查

```cpp
full_path = base_path / relative_file;
full_path = std::filesystem::canonical(full_path);
// 直接读取文件 - 危险！
std::ifstream file(full_path);
```

攻击者可以：
- 读取系统密码文件 `/etc/passwd`
- 读取私钥 `/root/.ssh/id_rsa`
- 读取应用配置 `/etc/app/config.ini`
- 读取其他用户文件

### ✅ 有安全检查

```cpp
full_path = base_path / relative_file;
full_path = std::filesystem::canonical(full_path);

// 安全检查
if (full_path.string().substr(0, base_path.string().length()) != base_path.string()) {
    return 403 Forbidden;  // 阻止攻击
}

// 现在安全了
std::ifstream file(full_path);
```

## 更严格的检查方式

可以使用 C++17 的 `std::filesystem` 提供的更安全的方法：

```cpp
// 方法 1: 检查是否是子路径
bool isSafe(const std::filesystem::path& base, const std::filesystem::path& full) {
    auto rel = std::filesystem::relative(full, base);
    return !rel.empty() && rel.native()[0] != '.';
}

// 方法 2: 使用 lexically_relative (C++17)
bool isSafe2(const std::filesystem::path& base, const std::filesystem::path& full) {
    auto rel = full.lexically_relative(base);
    return !rel.empty() && !rel.string().starts_with("..");
}
```

## 总结

**安全检查是必需的！** 因为：

1. `canonical()` 会解析 `..` 符号
2. 恶意的 `relative_file` 可能包含 `../../` 
3. 解析后的路径可能跳出 `base_path` 范围
4. 没有检查会导致严重的安全漏洞

**这不是多余的检查，而是关键的安全防线！** 🔒


