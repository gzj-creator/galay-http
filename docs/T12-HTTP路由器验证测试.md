# T12-HTTP 路由器验证测试

## 测试概述

本文档记录 HttpRouter 路径验证功能的测试结果。测试覆盖了路径格式验证、参数名验证、重复路由检测、参数提取验证等功能。

## 测试目标

验证 `HttpRouter` 类的路径验证机制，确保能够正确处理：
- 有效路径格式的接受
- 无效路径格式的拒绝
- 重复路由的检测和处理
- 路径参数的正确提取
- 与 HttpRequest 的集成

## 测试场景

### 1. 有效路径测试

#### 1.1 标准路径格式
- **测试内容**：验证各种有效的路径格式
- **有效路径示例**：
  - `/` - 根路径
  - `/api` - 简单路径
  - `/api/users` - 多段路径
  - `/api/users/:id` - 带参数路径
  - `/api/users/:userId/posts/:postId` - 多参数路径
  - `/static/*` - 单段通配符
  - `/files/**` - 贪婪通配符
- **验证点**：所有有效路径均被接受并成功注册

#### 1.2 特殊字符支持
- **测试内容**：验证路径中允许的特殊字符
- **有效路径示例**：
  - `/path-with-dash` - 连字符
  - `/path_with_underscore` - 下划线
  - `/path.with.dot` - 点号
  - `/path~with~tilde` - 波浪号
  - `/api/users/:user_id` - 参数名中的下划线
  - `/api/users/:userId123` - 参数名中的数字
- **验证点**：特殊字符被正确处理

### 2. 无效路径测试

#### 2.1 格式错误
- **测试内容**：验证各种无效的路径格式被拒绝
- **无效路径及原因**：
  - `""` - 空路径
  - `"api/users"` - 缺少前导斜杠
  - `"/api/users/:id/:id"` - 重复参数名
  - `"/api/*/extra"` - 通配符不在末尾
  - `"/api/**/extra"` - 贪婪通配符不在末尾
- **验证点**：所有无效路径均被拒绝

#### 2.2 参数名错误
- **测试内容**：验证无效的参数名格式
- **无效参数示例**：
  - `"/api/:"` - 空参数名
  - `"/api/:user-id"` - 参数名中的连字符
  - `"/api/:user id"` - 参数名中的空格
  - `"/api/:123"` - 参数名以数字开头
  - `"/api/:user@id"` - 参数名中的 @ 符号
  - `"/api/:user#id"` - 参数名中的 # 符号
- **验证点**：无效参数名被正确拒绝

#### 2.3 通配符错误
- **测试内容**：验证通配符使用规则
- **无效示例**：
  - `"/api/users/*/posts"` - 通配符不在末尾
  - `"/api/users/**/**"` - 多个通配符
- **验证点**：违反通配符规则的路径被拒绝

### 3. 重复路由检测测试

#### 3.1 相同路径和方法
- **测试内容**：两次注册相同的路由
- **验证点**：
  - 第一次注册成功
  - 第二次注册覆盖第一次（发出警告）
  - 路由数量不增加

### 4. 参数提取验证测试

#### 4.1 单参数提取
- **测试内容**：注册 `/user/:id`，请求 `/user/123`
- **验证点**：
  - 路由匹配成功
  - 参数数量为 1
  - `params["id"] == "123"`

#### 4.2 多参数提取
- **测试内容**：注册 `/user/:userId/posts/:postId`
- **验证点**：
  - 路由匹配成功
  - 参数数量为 2
  - `params["userId"] == "456"`
  - `params["postId"] == "789"`

### 5. 边界情况测试

#### 5.1 根路径
- **测试内容**：注册和匹配根路径 `/`
- **验证点**：根路径正常工作

#### 5.2 长路径
- **测试内容**：注册包含 50 个段的长路径
- **验证点**：长路径被正确处理

#### 5.3 多参数路径
- **测试内容**：注册包含 4 个参数的路径
- **验证点**：所有参数正确提取

### 6. HttpRequest 集成测试

#### 6.1 参数设置和获取
- **测试内容**：将路由参数设置到 HttpRequest 对象
- **验证点**：
  - `request.hasRouteParam("id")` 返回 true
  - `request.getRouteParam("id")` 返回正确值
  - `request.getRouteParam("nonexistent", "default")` 返回默认值

## 测试用例列表

| 编号 | 测试用例 | 类型 | 预期结果 |
|------|---------|------|---------|
| 1 | 标准有效路径 | Valid | ✓ 全部接受 |
| 2 | 特殊字符路径 | Valid | ✓ 正确处理 |
| 3 | 空路径 | Invalid | ✓ 拒绝 |
| 4 | 缺少前导斜杠 | Invalid | ✓ 拒绝 |
| 5 | 重复参数名 | Invalid | ✓ 拒绝 |
| 6 | 通配符位置错误 | Invalid | ✓ 拒绝 |
| 7 | 无效参数名 | Invalid | ✓ 拒绝 |
| 8 | 重复路由检测 | Duplicate | ✓ 覆盖并警告 |
| 9 | 单参数提取 | Parameter | ✓ 提取正确 |
| 10 | 多参数提取 | Parameter | ✓ 提取正确 |
| 11 | 根路径 | Edge | ✓ 正常工作 |
| 12 | 长路径 | Edge | ✓ 正确处理 |
| 13 | 多参数路径 | Edge | ✓ 全部提取 |
| 14 | HttpRequest 集成 | Integration | ✓ 集成正常 |

## 测试代码位置

- **文件路径**：`/Users/gongzhijie/Desktop/projects/git/galay-http/test/T12-HttpRouterValidation.cc`
- **测试函数数量**：6 个
- **代码行数**：256 行

## 运行测试

### 编译测试

```bash
cd build
cmake ..
make T12-HttpRouterValidation
```

### 运行测试

```bash
./test/T12-HttpRouterValidation
```

### 预期输出

```
========================================
HttpRouter Path Validation Tests
========================================

========================================
Test 1: Valid Paths
========================================
✓ Valid path accepted: /
✓ Valid path accepted: /api
✓ Valid path accepted: /api/users
✓ Valid path accepted: /api/users/:id
...
Valid paths: 14/14 accepted

========================================
Test 2: Invalid Paths (Should be Rejected)
========================================
✓ Invalid path rejected:  (Empty path)
✓ Invalid path rejected: api/users (Missing leading /)
✓ Invalid path rejected: /api/users/:id/:id (Duplicate parameter name)
...
Invalid paths: 14/14 rejected

========================================
✓ ALL VALIDATION TESTS PASSED!
========================================

Summary:
- Path validation: ✅ Working
- Duplicate detection: ✅ Working
- Parameter extraction: ✅ Working
- HttpRequest integration: ✅ Working
- Edge cases: ✅ Working
```

## 测试结论

### 功能验证

✅ **路径验证严格**：有效拒绝各种格式错误的路径
✅ **参数名验证完善**：确保参数名符合命名规范
✅ **重复检测有效**：能够检测并处理重复路由
✅ **参数提取准确**：正确提取单个和多个路径参数
✅ **集成良好**：与 HttpRequest 无缝集成
✅ **边界处理完善**：正确处理根路径、长路径等特殊情况

### 验证规则

1. **路径格式规则**：
   - 必须以 `/` 开头
   - 不能包含空段（连续的 `/`）
   - 参数名必须以字母或下划线开头
   - 参数名只能包含字母、数字、下划线

2. **参数规则**：
   - 参数名不能重复
   - 参数名不能为空
   - 参数名不能包含特殊字符

3. **通配符规则**：
   - 通配符必须在路径末尾
   - 不能有多个通配符

### 安全性

- **防止路径注入**：严格的路径验证防止恶意路径
- **参数安全**：参数名验证防止注入攻击
- **错误提示清晰**：帮助开发者快速定位问题

### 适用场景

1. **API 开发**：确保路由定义的正确性
2. **框架开发**：作为路由验证的参考实现
3. **安全审计**：验证路由配置的安全性

---

**测试日期**：2026-01-29
**测试人员**：galay-http 开发团队
**文档版本**：v1.0
