# 构建说明

## 调试日志控制

galay-http 现在支持通过 `CMAKE_BUILD_TYPE` 自动控制调试日志的启用/禁用。

### 构建模式说明

| 构建类型 | ENABLE_DEBUG 宏 | Debug 日志 | 性能 | 适用场景 |
|---------|----------------|-----------|------|---------|
| **Release** | ❌ 未定义 | 编译时移除 | ⚡ 最优 | 生产环境 |
| **Debug** | ✅ 定义 | 完全输出 | 🐌 较慢 | 开发调试 |

### 快速开始

#### 方式 1: Release 构建（默认，推荐用于生产）

```bash
cd galay-http
mkdir build && cd build
cmake ..                    # 默认为 Release 模式
make -j4
sudo make install
```

**输出示例**：
```
-- Build type: Release
-- Build type: Release - Debug logs disabled for performance
```

**效果**：
- ✅ 所有 `HTTP_LOG_DEBUG()` 和 `WS_LOG_DEBUG()` 在编译时被完全移除
- ✅ 零性能开销
- ✅ 只保留 INFO/WARN/ERROR 级别日志

#### 方式 2: Debug 构建（用于开发调试）

```bash
cd galay-http
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
sudo make install
```

**输出示例**：
```
-- Build type: Debug
-- Build type: Debug - Enabling ENABLE_DEBUG macro
```

**效果**：
- ✅ 所有 `HTTP_LOG_DEBUG()` 和 `WS_LOG_DEBUG()` 都会编译进去
- ⚠️ 性能会降低约 5-10%（取决于日志输出量）
- ✅ 可查看详细的执行流程

#### 方式 3: 显式指定构建类型

```bash
# Release 模式（性能最优）
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# Debug 模式（调试信息完整）
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4

# RelWithDebInfo 模式（Release + 调试符号，不启用 debug 日志）
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j4

# MinSizeRel 模式（最小体积，不启用 debug 日志）
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel
make -j4
```

### 运行时日志级别控制

除了编译时控制，还可以在**运行时**调整日志级别：

```cpp
#include "galay-http/utils/HttpLogger.h"

int main() {
    auto logger = HttpLogger::getInstance()->getLogger()->getSpdlogger();
    
    // 设置日志级别
    logger->set_level(spdlog::level::debug);  // 显示所有日志（包括 debug）
    logger->set_level(spdlog::level::info);   // 只显示 info 及以上
    logger->set_level(spdlog::level::warn);   // 只显示 warn 及以上
    logger->set_level(spdlog::level::error);  // 只显示 error
    
    // 你的代码...
}
```

**注意**：
- ⚠️ 如果编译时使用了 Release 模式，运行时无法恢复 debug 日志（已被移除）
- ✅ 如果编译时使用了 Debug 模式，可以通过运行时级别过滤日志

### 两种控制方式对比

#### 编译时控制（CMAKE_BUILD_TYPE）

```cmake
# CMakeLists.txt 自动设置
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DENABLE_DEBUG)  # 定义宏
endif()
```

**优点**：
- ✅ Release 模式下，debug 日志在编译阶段被完全移除
- ✅ 零性能开销（不会有任何字符串格式化等操作）
- ✅ 二进制文件更小
- ✅ 适合生产环境

**缺点**：
- ❌ 需要重新编译才能切换
- ❌ 不够灵活

#### 运行时控制（set_level）

```cpp
logger->set_level(spdlog::level::info);  // 运行时过滤
```

**优点**：
- ✅ 无需重新编译，可动态调整
- ✅ 灵活方便

**缺点**：
- ❌ debug 日志的参数格式化等开销仍然存在
- ❌ 只能过滤，无法移除已编译的日志
- ❌ 性能略有影响

### 最佳实践

#### 开发阶段

```bash
# 使用 Debug 模式，方便查看详细日志
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
```

```cpp
// 代码中设置为 debug 级别
HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::debug);
```

#### 性能测试阶段

```bash
# 使用 Release 模式，获得真实性能数据
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
```

```cpp
// 代码中设置为 info 级别
HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::info);
```

#### 生产环境

```bash
# 使用 Release 模式编译
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
sudo make install
```

```cpp
// 代码中只显示 warn 及以上
HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::warn);
```

#### 问题诊断

如果生产环境出现问题需要调试：

```bash
# 重新编译 Debug 版本
cd build
rm -rf *
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
sudo make install

# 重启你的服务
./your_service
```

### 验证当前构建类型

查看编译输出：

```bash
cd build
cmake ..

# 输出会显示：
# -- Build type: Release
# -- Build type: Release - Debug logs disabled for performance
# 或
# -- Build type: Debug
# -- Build type: Debug - Enabling ENABLE_DEBUG macro
```

### 常见问题

#### Q1: 如何查看当前库是 Release 还是 Debug 版本？

**A**: 查看构建时的输出信息，或者：

```bash
# 查看库文件大小（Debug 版本通常更大）
ls -lh build/galay-http/libgalay-http.a

# Release: 约 500-800 KB
# Debug:   约 1-2 MB（包含调试符号和日志代码）
```

#### Q2: 已经编译好的 Release 库，能启用 debug 日志吗？

**A**: 不能。必须重新编译为 Debug 版本。

```bash
cd build
rm -rf *
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
```

#### Q3: 我想同时拥有 Release 和 Debug 两个版本怎么办？

**A**: 使用不同的构建目录：

```bash
# Release 版本
mkdir build-release && cd build-release
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
cd ..

# Debug 版本
mkdir build-debug && cd build-debug
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j4
cd ..

# 使用时链接不同的库
# Release: build-release/galay-http/libgalay-http.a
# Debug:   build-debug/galay-http/libgalay-http.a
```

#### Q4: 测试程序中 `#define ENABLE_DEBUG` 还有用吗？

**A**: 
- 如果库是 **Release 编译**：测试程序中定义 `ENABLE_DEBUG` **无效**（库中的日志已被移除）
- 如果库是 **Debug 编译**：测试程序中定义与否都一样（库中的日志已编译进去）

**建议**：不要在测试程序中定义 `ENABLE_DEBUG`，通过 CMAKE_BUILD_TYPE 统一控制。

#### Q5: 如何验证 debug 日志是否真的被移除了？

**A**: 编译 Release 版本后运行，即使设置了 `set_level(debug)`，也不会看到 debug 日志：

```bash
# Release 构建
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
./test/test_ws_server

# 输出不会包含 [D] 级别的日志，只有 [I] [W] [E]
```

### 构建脚本示例

创建便捷脚本：

**build-release.sh**:
```bash
#!/bin/bash
cd build
rm -rf *
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
echo "Release build complete!"
```

**build-debug.sh**:
```bash
#!/bin/bash
cd build
rm -rf *
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
echo "Debug build complete!"
```

使用：
```bash
chmod +x build-*.sh
./build-release.sh  # Release 构建
# 或
./build-debug.sh    # Debug 构建
```

### 总结

- ✅ **默认为 Release 模式**：性能最优，适合生产
- ✅ **使用 `-DCMAKE_BUILD_TYPE=Debug`**：启用完整调试日志
- ✅ **编译时控制 + 运行时过滤**：灵活且高效
- ✅ **无需手动定义宏**：CMake 自动处理

Happy coding! 🚀

