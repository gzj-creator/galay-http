package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"golang.org/x/net/http2"
)

type benchStats struct {
	success   int64
	fail      int64
	latencyNs int64
}

func (s *benchStats) add(ok bool, latency time.Duration) {
	if ok {
		atomic.AddInt64(&s.success, 1)
	} else {
		atomic.AddInt64(&s.fail, 1)
	}
	atomic.AddInt64(&s.latencyNs, latency.Nanoseconds())
}

func deadlineReached(deadline time.Time) bool {
	return !time.Now().Before(deadline)
}

func makeHTTPClient(proto, addr string, timeout time.Duration) (*http.Client, error) {
	switch proto {
	case "http":
		tr := &http.Transport{
			MaxIdleConnsPerHost: 1,
			MaxConnsPerHost:     1,
			DisableCompression:  true,
		}
		return &http.Client{Transport: tr, Timeout: timeout}, nil
	case "https":
		tr := &http.Transport{
			MaxIdleConnsPerHost: 1,
			MaxConnsPerHost:     1,
			DisableCompression:  true,
			TLSClientConfig: &tls.Config{
				InsecureSkipVerify: true,
				ServerName:         strings.Split(addr, ":")[0],
			},
		}
		if err := http2.ConfigureTransport(tr); err != nil {
			return nil, err
		}
		return &http.Client{Transport: tr, Timeout: timeout}, nil
	case "h2":
		tr := &http2.Transport{
			TLSClientConfig: &tls.Config{
				InsecureSkipVerify: true,
				NextProtos:         []string{"h2"},
				ServerName:         strings.Split(addr, ":")[0],
			},
		}
		return &http.Client{Transport: tr, Timeout: timeout}, nil
	case "h2c":
		dialer := &net.Dialer{Timeout: timeout}
		tr := &http2.Transport{
			AllowHTTP: true,
			DialTLSContext: func(ctx context.Context, network, address string, _ *tls.Config) (net.Conn, error) {
				return dialer.DialContext(ctx, network, address)
			},
		}
		return &http.Client{Transport: tr, Timeout: timeout}, nil
	default:
		return nil, fmt.Errorf("unsupported protocol: %s", proto)
	}
}

func runHTTPWorker(proto, addr, path string, payload []byte, deadline time.Time, stats *benchStats) {
	client, err := makeHTTPClient(proto, addr, 8*time.Second)
	if err != nil {
		stats.add(false, 0)
		return
	}

	base := "http://"
	if proto == "https" || proto == "h2" {
		base = "https://"
	}
	url := base + addr + path

	for !deadlineReached(deadline) {
		start := time.Now()
		ok := true

		var req *http.Request
		if proto == "h2" || proto == "h2c" {
			req, err = http.NewRequest(http.MethodPost, url, bytes.NewReader(payload))
			req.Header.Set("content-type", "text/plain")
			req.Header.Set("content-length", fmt.Sprintf("%d", len(payload)))
		} else {
			req, err = http.NewRequest(http.MethodGet, url, nil)
			req.Header.Set("connection", "keep-alive")
		}
		if err != nil {
			stats.add(false, time.Since(start))
			continue
		}

		resp, err := client.Do(req)
		if err != nil {
			ok = false
			stats.add(ok, time.Since(start))
			continue
		}

		data, readErr := io.ReadAll(resp.Body)
		_ = resp.Body.Close()
		if readErr != nil {
			ok = false
		}
		if resp.StatusCode != http.StatusOK {
			ok = false
		}
		if ok && (proto == "h2" || proto == "h2c") && !bytes.Equal(data, payload) {
			ok = false
		}

		stats.add(ok, time.Since(start))
	}
}

func runWSWorker(proto, addr, path string, payload []byte, deadline time.Time, stats *benchStats) {
	scheme := "ws"
	if proto == "wss" {
		scheme = "wss"
	}

	dialer := websocket.Dialer{
		HandshakeTimeout: 5 * time.Second,
		EnableCompression: false,
	}
	if proto == "wss" {
		dialer.TLSClientConfig = &tls.Config{InsecureSkipVerify: true}
	}

	conn, _, err := dialer.Dial(fmt.Sprintf("%s://%s%s", scheme, addr, path), nil)
	if err != nil {
		stats.add(false, 0)
		return
	}
	defer conn.Close()

	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	if _, _, err := conn.ReadMessage(); err != nil {
		stats.add(false, 0)
		return
	}
	_ = conn.SetReadDeadline(time.Time{})

	for !deadlineReached(deadline) {
		start := time.Now()
		ok := true

		if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
			ok = false
		}
		if ok {
			if err := conn.WriteMessage(websocket.TextMessage, payload); err != nil {
				ok = false
			}
		}
		if ok {
			if err := conn.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
				ok = false
			}
		}

		if ok {
			msgType, data, err := conn.ReadMessage()
			if err != nil {
				ok = false
			} else if msgType != websocket.TextMessage && msgType != websocket.BinaryMessage {
				ok = false
			} else if !bytes.Equal(data, payload) {
				ok = false
			}
		}

		stats.add(ok, time.Since(start))
	}

	_ = conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
}

func main() {
	var (
		proto    = flag.String("proto", "http", "http|https|ws|wss|h2c|h2")
		addr     = flag.String("addr", "127.0.0.1:8080", "server addr")
		path     = flag.String("path", "/", "request path")
		conns    = flag.Int("conns", 100, "concurrency")
		duration = flag.Int("duration", 8, "seconds")
		size     = flag.Int("size", 128, "payload bytes")
	)
	flag.Parse()

	if *conns <= 0 {
		*conns = 1
	}
	if *duration <= 0 {
		*duration = 1
	}
	if *size <= 0 {
		*size = 1
	}

	if *path == "" {
		*path = "/"
	}

	payload := bytes.Repeat([]byte("A"), *size)
	deadline := time.Now().Add(time.Duration(*duration) * time.Second)

	var stats benchStats
	var wg sync.WaitGroup
	wg.Add(*conns)

	for i := 0; i < *conns; i++ {
		go func() {
			defer wg.Done()
			switch *proto {
			case "http", "https", "h2", "h2c":
				runHTTPWorker(*proto, *addr, *path, payload, deadline, &stats)
			case "ws", "wss":
				runWSWorker(*proto, *addr, *path, payload, deadline, &stats)
			default:
				stats.add(false, 0)
			}
		}()
	}

	wg.Wait()

	succ := atomic.LoadInt64(&stats.success)
	fail := atomic.LoadInt64(&stats.fail)
	lat := atomic.LoadInt64(&stats.latencyNs)
	total := succ + fail
	elapsed := float64(*duration)
	rps := 0.0
	avgMs := 0.0
	if elapsed > 0 {
		rps = float64(succ) / elapsed
	}
	if total > 0 {
		avgMs = (float64(lat) / float64(total)) / 1e6
	}

	fmt.Printf("RESULT proto=%s addr=%s conns=%d duration=%d success=%d fail=%d rps=%.2f avg_ms=%.3f\n",
		*proto, *addr, *conns, *duration, succ, fail, rps, avgMs)
}
