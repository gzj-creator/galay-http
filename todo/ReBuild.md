# 带重构事项

1. 类成员函数不要出现引用，有引用的成员变量类和改为指针，构造传入也是指针
2. 当前test/example/benchmark很乱，现在有规则：
    - example对每个常见功能进行举例，如每个协议Client和Server分别列出到文件中
    - test删除不必要的test，某些test可能已经被包含，比如Router的test可能被HttpServer的test包含
    - benchmark移除不必要的benchmark,但是一定要包含大功能的压测，如Client/Server

3. 优化需求: reader和writer分为respReader和reqReader，reqWriter和respWriter，客户端用respoReader和reqWriter，服务端用另外两个，这样WSReader和Writer就不会有m_is_server变量了