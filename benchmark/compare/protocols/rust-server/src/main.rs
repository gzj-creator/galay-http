use std::{convert::Infallible, net::SocketAddr, path::PathBuf};

use axum::{
    extract::{ws::{Message, WebSocket, WebSocketUpgrade}, State},
    http::{Method, Request, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Router,
};
use bytes::Bytes;
use http_body_util::{BodyExt, Full};
use hyper::service::service_fn;
use hyper_util::rt::{TokioExecutor, TokioIo};
use tokio::net::TcpListener;

#[derive(Clone)]
struct AppState {
    welcome: String,
}

async fn http_ok() -> impl IntoResponse {
    (StatusCode::OK, "OK")
}

async fn echo(body: String) -> impl IntoResponse {
    (StatusCode::OK, [("content-type", "text/plain")], body)
}

async fn ws_upgrade(ws: WebSocketUpgrade, State(state): State<AppState>) -> impl IntoResponse {
    ws.on_upgrade(move |socket| ws_session(socket, state.welcome))
}

async fn ws_session(mut socket: WebSocket, welcome: String) {
    if socket.send(Message::Text(welcome)).await.is_err() {
        return;
    }

    while let Some(Ok(msg)) = socket.recv().await {
        match msg {
            Message::Text(t) => {
                if socket.send(Message::Text(t)).await.is_err() {
                    return;
                }
            }
            Message::Binary(b) => {
                if socket.send(Message::Binary(b)).await.is_err() {
                    return;
                }
            }
            Message::Ping(p) => {
                if socket.send(Message::Pong(p)).await.is_err() {
                    return;
                }
            }
            Message::Close(_) => {
                let _ = socket.close().await;
                return;
            }
            Message::Pong(_) => {}
        }
    }
}

async fn h2c_service(req: Request<hyper::body::Incoming>) -> Result<Response<Full<Bytes>>, Infallible> {
    let method = req.method().clone();
    let path = req.uri().path().to_string();

    if method == Method::POST && path == "/echo" {
        let body = req.into_body().collect().await;
        match body {
            Ok(collected) => {
                let data = collected.to_bytes();
                let mut resp = Response::new(Full::new(data.clone()));
                *resp.status_mut() = StatusCode::OK;
                resp.headers_mut().insert(
                    axum::http::header::CONTENT_TYPE,
                    axum::http::HeaderValue::from_static("text/plain"),
                );
                resp.headers_mut().insert(
                    axum::http::header::CONTENT_LENGTH,
                    axum::http::HeaderValue::from_str(&data.len().to_string())
                        .unwrap_or_else(|_| axum::http::HeaderValue::from_static("0")),
                );
                return Ok(resp);
            }
            Err(_) => {
                let mut resp = Response::new(Full::new(Bytes::from_static(b"bad body")));
                *resp.status_mut() = StatusCode::BAD_REQUEST;
                return Ok(resp);
            }
        }
    }

    let mut resp = Response::new(Full::new(Bytes::from_static(b"OK")));
    *resp.status_mut() = StatusCode::OK;
    resp.headers_mut().insert(
        axum::http::header::CONTENT_TYPE,
        axum::http::HeaderValue::from_static("text/plain"),
    );
    Ok(resp)
}

async fn run_h2c_server(addr: SocketAddr) -> std::io::Result<()> {
    let listener = TcpListener::bind(addr).await?;
    loop {
        let (stream, _) = listener.accept().await?;
        tokio::spawn(async move {
            let io = TokioIo::new(stream);
            let builder = hyper::server::conn::http2::Builder::new(TokioExecutor::new());
            let conn = builder.serve_connection(io, service_fn(h2c_service));
            if let Err(err) = conn.await {
                eprintln!("h2c connection error: {err}");
            }
        });
    }
}

#[tokio::main]
async fn main() {
    if let Ok(v) = std::env::var("TOKIO_WORKER_THREADS") {
        eprintln!("rust runtime config: TOKIO_WORKER_THREADS={v}");
    } else {
        eprintln!("rust runtime config: TOKIO_WORKER_THREADS=default");
    }

    let mut http_addr = SocketAddr::from(([0, 0, 0, 0], 8080));
    let mut h2c_addr = SocketAddr::from(([0, 0, 0, 0], 9080));
    let mut https_addr = SocketAddr::from(([0, 0, 0, 0], 8443));
    let mut cert_path = PathBuf::from("cert/test.crt");
    let mut key_path = PathBuf::from("cert/test.key");

    let args: Vec<String> = std::env::args().collect();
    if args.len() > 1 {
        if let Ok(addr) = args[1].parse() { http_addr = addr; }
    }
    if args.len() > 2 {
        if let Ok(addr) = args[2].parse() { h2c_addr = addr; }
    }
    if args.len() > 3 {
        if let Ok(addr) = args[3].parse() { https_addr = addr; }
    }
    if args.len() > 4 {
        cert_path = PathBuf::from(&args[4]);
    }
    if args.len() > 5 {
        key_path = PathBuf::from(&args[5]);
    }

    let state = AppState {
        welcome: "Welcome to WebSocket Benchmark Server!".to_string(),
    };

    let app = Router::new()
        .route("/", get(http_ok))
        .route("/echo", post(echo))
        .route("/ws", get(ws_upgrade))
        .with_state(state);

    let rustls_config = axum_server::tls_rustls::RustlsConfig::from_pem_file(cert_path, key_path)
        .await
        .expect("load cert/key failed");

    let http_task = tokio::spawn(async move {
        let listener = TcpListener::bind(http_addr).await.expect("bind http failed");
        axum::serve(listener, app)
            .await
            .expect("http serve failed");
    });

    let app_tls = Router::new()
        .route("/", get(http_ok))
        .route("/echo", post(echo))
        .route("/ws", get(ws_upgrade))
        .with_state(AppState {
            welcome: "Welcome to WebSocket Benchmark Server!".to_string(),
        });

    let https_task = tokio::spawn(async move {
        axum_server::bind_rustls(https_addr, rustls_config)
            .serve(app_tls.into_make_service())
            .await
            .expect("https serve failed");
    });

    let h2c_task = tokio::spawn(async move {
        run_h2c_server(h2c_addr).await.expect("h2c serve failed");
    });

    eprintln!("rust server started: http={http_addr} h2c={h2c_addr} https={https_addr}");

    #[cfg(unix)]
    {
        let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
            .expect("install sigterm handler failed");
        tokio::select! {
            _ = tokio::signal::ctrl_c() => {},
            _ = term.recv() => {},
        }
    }

    #[cfg(not(unix))]
    {
        let _ = tokio::signal::ctrl_c().await;
    }

    http_task.abort();
    https_task.abort();
    h2c_task.abort();
}
