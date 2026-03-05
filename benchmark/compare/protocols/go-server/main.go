package main

import (
	"context"
	"flag"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true
	},
}

func wsHandler(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer conn.Close()

	if err := conn.WriteMessage(websocket.TextMessage, []byte("Welcome to WebSocket Benchmark Server!")); err != nil {
		return
	}

	for {
		mt, message, err := conn.ReadMessage()
		if err != nil {
			return
		}
		switch mt {
		case websocket.TextMessage, websocket.BinaryMessage:
			if err := conn.WriteMessage(mt, message); err != nil {
				return
			}
		case websocket.PingMessage:
			if err := conn.WriteMessage(websocket.PongMessage, message); err != nil {
				return
			}
		case websocket.CloseMessage:
			_ = conn.WriteControl(websocket.CloseMessage,
				websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""),
				time.Now().Add(time.Second))
			return
		}
	}
}

func rootHandler(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path == "/ws" || r.URL.Path == "/" {
		if websocket.IsWebSocketUpgrade(r) {
			wsHandler(w, r)
			return
		}
	}

	if r.URL.Path == "/echo" && r.Method == http.MethodPost {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, "read body failed", http.StatusBadRequest)
			return
		}
		_ = r.Body.Close()
		w.Header().Set("Content-Type", "text/plain")
		w.Header().Set("Content-Length", itoa(len(body)))
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write(body)
		return
	}

	w.Header().Set("Content-Type", "text/plain")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Content-Length", "2")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte("OK"))
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var buf [20]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	return string(buf[i:])
}

func startHTTPServer(addr string, handler http.Handler) *http.Server {
	srv := &http.Server{
		Addr:              addr,
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			log.Fatalf("listen %s failed: %v", addr, err)
		}
		if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
			log.Fatalf("serve %s failed: %v", addr, err)
		}
	}()
	return srv
}

func startHTTPSServer(addr, certPath, keyPath string, handler http.Handler) *http.Server {
	srv := &http.Server{
		Addr:              addr,
		Handler:           handler,
		ReadHeaderTimeout: 5 * time.Second,
	}
	if err := http2.ConfigureServer(srv, &http2.Server{}); err != nil {
		log.Fatalf("configure h2 tls failed: %v", err)
	}
	go func() {
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			log.Fatalf("listen tls %s failed: %v", addr, err)
		}
		if err := srv.ServeTLS(ln, certPath, keyPath); err != nil && err != http.ErrServerClosed {
			log.Fatalf("serve tls %s failed: %v", addr, err)
		}
	}()
	return srv
}

func startH2CServer(addr string, baseHandler http.Handler) *http.Server {
	h2s := &http2.Server{
		MaxConcurrentStreams: 1000,
	}
	srv := &http.Server{
		Addr:              addr,
		Handler:           h2c.NewHandler(baseHandler, h2s),
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			log.Fatalf("listen h2c %s failed: %v", addr, err)
		}
		if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
			log.Fatalf("serve h2c %s failed: %v", addr, err)
		}
	}()
	return srv
}

func main() {
	var (
		httpAddr  = flag.String("http", ":8080", "http/ws listen addr")
		h2cAddr   = flag.String("h2c", ":9080", "h2c listen addr")
		httpsAddr = flag.String("https", ":8443", "https/wss/h2 tls listen addr")
		certPath  = flag.String("cert", "cert/test.crt", "tls cert path")
		keyPath   = flag.String("key", "cert/test.key", "tls key path")
	)
	flag.Parse()
	log.Printf("go runtime config: GOMAXPROCS=%d", runtime.GOMAXPROCS(0))

	mux := http.NewServeMux()
	mux.HandleFunc("/", rootHandler)
	mux.HandleFunc("/ws", rootHandler)
	mux.HandleFunc("/echo", rootHandler)

	httpSrv := startHTTPServer(*httpAddr, mux)
	h2cSrv := startH2CServer(*h2cAddr, mux)
	httpsSrv := startHTTPSServer(*httpsAddr, *certPath, *keyPath, mux)

	log.Printf("go server started: http=%s h2c=%s https=%s", *httpAddr, *h2cAddr, *httpsAddr)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	<-sigCh

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(ctx)
	_ = h2cSrv.Shutdown(ctx)
	_ = httpsSrv.Shutdown(ctx)
}
