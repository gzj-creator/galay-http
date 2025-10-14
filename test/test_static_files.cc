#include "galay/kernel/runtime/Runtime.h"
#include "kernel/HttpRouter.h"
#include "server/HttpServer.h"
#include "utils/HttpLogger.h"
#include <iostream>

using namespace galay;
using namespace galay::http;

int main()
{
    HttpLogger::getInstance()->getLogger()->getSpdlogger()->set_level(spdlog::level::level_enum::debug);
    
    RuntimeBuilder runtimeBuilder;
    auto runtime = runtimeBuilder.build();
    runtime.start();
    
    HttpServerBuilder builder;
    HttpServer server = builder.build();
    server.listen(Host("0.0.0.0", 8060));
    
    // 创建路由器并挂载静态文件目录
    HttpRouter router;
    
    // 挂载静态文件服务
    // 注意：mount() 会立即验证路径，如果路径不存在会抛出异常
    try {
        // 例如: GET /static/css/style.css -> 会读取 ./public/css/style.css
        router.mount("/questionnaire/static", "/Users/gongzhijie/Desktop/projects/cursor/questionnaire/dist");
        
        // 也可以挂载多个目录
        // router.mount("/assets", "./assets");
        // router.mount("/images", "./images");
        
    } catch (const std::runtime_error& e) {
        std::cerr << "Mount failed: " << e.what() << std::endl;
        std::cerr << "Please ensure the directory exists before starting the server." << std::endl;
        return 1;
    }
    
    std::cout << "Static file server started on http://0.0.0.0:8060" << std::endl;
    std::cout << "Try: http://localhost:8060/questionnaire/static/index.html" << std::endl;
    
    server.run(runtime, router);
    server.wait();
    server.stop();
    
    return 0;
}

