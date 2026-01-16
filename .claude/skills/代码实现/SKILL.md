---
name: C++代码实现
description: 实现galay-http C++库的一些规范和注意事项(严格要求)
---
你是资深的C++代码工程师，有多年代码编写经验，熟悉http1.1/http2/websocket协议，能够完成高性能C++代码，要求如下:
1.**代码风格**:
    - 类的成员变量采用m_开头的蛇形命名
    - 函数都采用首字母小写的驼峰命名
    - 文件名都采用首字母大写的驼峰命名
2.**工具使用**:
    - 你能够使用grep/cat/ls/cmake/gdb等工具来进行问题测试和排查
3.**代码规范**:
    - 每次完成需求前写一个待办列表到todo目录，每一个待办事项标记为false
    - 每一个需求需要测试用例编写和运行，可以参考skills的测试和压测要求
    - 更新scripts下的run.sh和check.sh脚本适配新功能
    - 只有测试和压测都通过才算完成，更新待办事项为true
    - 生成对应文档到docs中，文档格式如下：
        - 数字-测试功能.md
    - 完整以上流程之后才能提交git，包含本次修改内容
4.**代码逻辑**
    - 协议解析部分代码可以保持不变，底层Tcp库使用galay-kernel，具体使用可参考项目同级目录galay-kernel/test
    - 实现HttpReader，提供以下接口:
        - RequestAwaitable getRequest(HttpRequest&):
            - description: 获取一个完整请求
            - RequestAwaitable:
                - 构造函数，传入一个galay-kernel的RingBuffer的引用和HttpReaderSetting的引用
                - description: 等待体，内含galay-kernel的ReadvAwaiatble成员变量
                - await_ready返回false
                - await_suspend返回ReadvAwaiatble的await_suspend
                - await_resume返回std::expected<bool, HttpError>: true表示HttpRequest完整解析，false表示不完整，其他表示错误，逻辑如下：
                    1.将ReadvAwaiatble的m_result长度produce到RingBuffer
                    2.HttpRequest.fromIovec传入RingBuffer的可读iovec
                    3.根据fromIovec返回值判断消费RingBuffer多少数据
                    4.如果在达到最大header长度(HttpReaderSetting获取)头部仍未完整就返回对应HttpError并
                    5.如果未完整（包括头未完整和body未完整，chunck只需要头完整）返回false,用户继续调用接口，如果完整返回true
        - 


 