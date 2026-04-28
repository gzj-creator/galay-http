import galay.http;
import galay.http2;
import galay.websocket;

int main() {
    using namespace galay::http;
    using namespace galay::http2;
    using namespace galay::websocket;

    auto http_client_config = HttpClientBuilder().headerMode(HeaderPair::Mode::ClientSide).buildConfig();
    auto http_server_config = HttpServerBuilder().host("127.0.0.1").port(0).buildConfig();
    auto ws_client_config = WsClientBuilder().headerMode(HeaderPair::Mode::ClientSide).buildConfig();
    auto h2c_client_config = H2cClientBuilder().maxConcurrentStreams(8).buildConfig();
    auto h2c_server_config = H2cServerBuilder().enablePush(false).buildConfig();

    (void)http_client_config;
    (void)http_server_config;
    (void)ws_client_config;
    (void)h2c_client_config;
    (void)h2c_server_config;

    HttpRequest request;
    HttpResponse response;
    (void)request;
    (void)response;

#ifdef GALAY_HTTP_SSL_ENABLED
    auto https_client_config = HttpsClientBuilder().verifyPeer(false).buildConfig();
    auto wss_client_config = WssClientBuilder().verifyPeer(false).buildConfig();
    auto h2_client_config = H2ClientBuilder().verifyPeer(false).buildConfig();
    auto https_server_config = HttpsServerBuilder()
        .certPath("test/test.crt")
        .keyPath("test/test.key")
        .buildConfig();
    auto h2_server_config = H2ServerBuilder()
        .certPath("test/test.crt")
        .keyPath("test/test.key")
        .buildConfig();

    (void)https_client_config;
    (void)wss_client_config;
    (void)h2_client_config;
    (void)https_server_config;
    (void)h2_server_config;
#endif

    return 0;
}
