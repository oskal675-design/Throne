package probe

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/Mahdi-zarei/speedtest-go/speedtest"
)

// Real HTTP requests and the pinned speedtest implementation run over net.Pipe.
// There is no listener, DNS resolution, external connection, or VPN instance.
type memorySpeedtestHTTP struct {
	mu     sync.Mutex
	closed bool
	conns  map[net.Conn]struct{}
	wg     sync.WaitGroup
}

func (f *memorySpeedtestHTTP) dial(ctx context.Context, _, _ string) (net.Conn, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.closed {
		return nil, net.ErrClosed
	}
	client, server := net.Pipe()
	f.conns[server] = struct{}{}
	f.wg.Add(1)
	go func() {
		defer f.wg.Done()
		defer server.Close()
		defer func() {
			f.mu.Lock()
			delete(f.conns, server)
			f.mu.Unlock()
		}()
		req, err := http.ReadRequest(bufio.NewReader(server))
		if err != nil {
			return
		}
		defer req.Body.Close()
		buf := make([]byte, 8192)
		if req.Method == http.MethodPost {
			for {
				time.Sleep(2 * time.Millisecond)
				if _, err := req.Body.Read(buf); err != nil {
					if err == io.EOF {
						_, _ = io.WriteString(server, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
					}
					return
				}
			}
		}
		if _, err := fmt.Fprintf(server, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", 128*len(buf)); err != nil {
			return
		}
		for i := 0; i < 128; i++ {
			time.Sleep(2 * time.Millisecond)
			if _, err := server.Write(buf); err != nil {
				return
			}
		}
	}()
	return client, nil
}

func (f *memorySpeedtestHTTP) close() {
	f.mu.Lock()
	f.closed = true
	for conn := range f.conns {
		_ = conn.Close()
	}
	f.mu.Unlock()
	f.wg.Wait()
}

// Observe the real capture callbacks without replacing the dependency's
// estimator, workers, request handlers, or atomic traffic counters.
type observedSpeedtestManager struct {
	speedtest.Manager
	mu       sync.Mutex
	download map[string]bool
	upload   map[string]bool
	calls    atomic.Int64
}

func (m *observedSpeedtestManager) SetCallbackDownload(callback func(speedtest.ByteRate)) {
	m.Manager.SetCallbackDownload(func(rate speedtest.ByteRate) {
		m.mu.Lock()
		m.download[BrateToStr(float64(rate))] = true
		m.mu.Unlock()
		callback(rate)
		m.calls.Add(1)
	})
}

func (m *observedSpeedtestManager) SetCallbackUpload(callback func(speedtest.ByteRate)) {
	m.Manager.SetCallbackUpload(func(rate speedtest.ByteRate) {
		m.mu.Lock()
		m.upload[BrateToStr(float64(rate))] = true
		m.mu.Unlock()
		callback(rate)
		m.calls.Add(1)
	})
}

func newMemorySpeedtest(t *testing.T, frequency time.Duration) (*speedtest.Server, *observedSpeedtestManager) {
	t.Helper()
	fixture := &memorySpeedtestHTTP{conns: make(map[net.Conn]struct{})}
	config := &speedtest.UserConfig{DialContextFunc: fixture.dial, MaxConnections: 2}
	client := speedtest.New(speedtest.WithUserConfig(config))
	config.T.DisableKeepAlives = true
	client.SetRateCaptureFrequency(frequency)
	client.SetCaptureTime(2 * time.Second)
	manager := &observedSpeedtestManager{Manager: client.Manager, download: make(map[string]bool), upload: make(map[string]bool)}
	client.Manager = manager
	t.Cleanup(func() {
		config.T.CloseIdleConnections()
		fixture.close()
	})
	return &speedtest.Server{Context: client, URL: "http://fixture.invalid/upload.php", Name: "Memory fixture", Country: "Test", Latency: 17 * time.Millisecond}, manager
}

func collectSpeedtest(t *testing.T, ctx context.Context, srv *speedtest.Server, dl, ul bool, timeout time.Duration) (SpeedTestResult, []SpeedTestResult, error) {
	t.Helper()
	SpTQuerier.storeResult(&SpeedTestResult{})
	result := SpeedTestResult{Tag: "fixture"}
	done := make(chan error, 1)
	go func() { done <- speedTestWithServer(ctx, srv, &result, dl, ul, timeout) }()
	ticker := time.NewTicker(5 * time.Millisecond)
	defer ticker.Stop()
	var live []SpeedTestResult
	deadline := time.NewTimer(5 * time.Second)
	defer deadline.Stop()
	for {
		select {
		case err := <-done:
			if _, running := SpTQuerier.Result(); running {
				t.Error("querier remains running after completion")
			}
			return result, live, err
		case <-ticker.C:
			if sample, running := SpTQuerier.Result(); running && sample.Tag == result.Tag {
				live = append(live, sample)
			}
		case <-deadline.C:
			t.Fatal("in-memory speedtest did not finish")
		}
	}
}

func TestSpeedtestConcurrentLiveReporting(t *testing.T) {
	for _, mode := range []struct {
		name   string
		dl, ul bool
	}{{"download", true, false}, {"upload", false, true}, {"both", true, true}, {"neither", false, false}} {
		t.Run(mode.name, func(t *testing.T) {
			srv, manager := newMemorySpeedtest(t, 10*time.Millisecond)
			result, live, err := collectSpeedtest(t, context.Background(), srv, mode.dl, mode.ul, 350*time.Millisecond)
			if err != nil || result.Error != nil || result.Cancelled {
				t.Fatalf("unexpected completion: %+v, %v", result, err)
			}
			if result.DlSpeed != BrateToStr(float64(srv.DLSpeed)) || result.UlSpeed != BrateToStr(float64(srv.ULSpeed)) || result.DlBytes != srv.Context.GetTotalDownload() || result.UlBytes != srv.Context.GetTotalUpload() {
				t.Fatalf("final result differs from dependency result: %+v", result)
			}
			if result.ServerName != srv.Name || result.ServerCountry != srv.Country || result.Latency != 17 {
				t.Errorf("server metadata changed: %+v", result)
			}
			if (result.DlBytes > 0) != mode.dl || (result.UlBytes > 0) != mode.ul {
				t.Errorf("direction byte accounting: %+v", result)
			}
			if (mode.dl || mode.ul) && len(live) == 0 {
				t.Fatal("no live samples")
			}
			manager.mu.Lock()
			defer manager.mu.Unlock()
			var dlPositive, ulPositive bool
			for _, sample := range live {
				for direction, rate := range map[string]string{"download": sample.DlSpeed, "upload": sample.UlSpeed} {
					if strings.Contains(rate, "NaN") || strings.Contains(rate, "Inf") {
						t.Errorf("non-finite live %s rate %q", direction, rate)
					}
				}
				if sample.DlSpeed != BrateToStr(0) && !manager.download[sample.DlSpeed] {
					t.Errorf("download sample is not a captured bytes/s value: %s", sample.DlSpeed)
				}
				if sample.UlSpeed != BrateToStr(0) && !manager.upload[sample.UlSpeed] {
					t.Errorf("upload sample is not a captured bytes/s value: %s", sample.UlSpeed)
				}
				dlPositive = dlPositive || sample.DlSpeed != BrateToStr(0)
				ulPositive = ulPositive || sample.UlSpeed != BrateToStr(0)
			}
			if dlPositive != mode.dl || ulPositive != mode.ul {
				t.Errorf("live direction isolation: download=%v upload=%v", dlPositive, ulPositive)
			}
		})
	}
}

func TestSpeedtestCancellationAndRepeatedInvocations(t *testing.T) {
	for iteration := 0; iteration < 3; iteration++ {
		srv, manager := newMemorySpeedtest(t, 10*time.Millisecond)
		ctx, cancel := context.WithCancel(context.Background())
		timer := time.AfterFunc(175*time.Millisecond, cancel)
		result, live, err := collectSpeedtest(t, ctx, srv, false, true, time.Second)
		timer.Stop()
		cancel()
		if !errors.Is(err, context.Canceled) || !result.Cancelled || len(live) == 0 || manager.calls.Load() == 0 {
			t.Fatalf("iteration %d: cancellation/live callback failure: result=%+v err=%v samples=%d", iteration, result, err, len(live))
		}
		// A fresh client must not inherit the previous invocation's upload rate.
		next, _ := newMemorySpeedtest(t, 10*time.Millisecond)
		result, _, err = collectSpeedtest(t, context.Background(), next, true, false, 200*time.Millisecond)
		if err != nil || result.UlSpeed != BrateToStr(0) || result.UlBytes != 0 {
			t.Fatalf("iteration %d: rate leaked across invocations: %+v, %v", iteration, result, err)
		}
	}
}

// Deliberately let one real capture callback finish after the sampling function
// has returned on cancellation. The captured atomics must remain valid and
// must not write into a later invocation's result.
type delayedSpeedtestCallback struct {
	speedtest.Manager
	entered, release, finished chan struct{}
	pauseOnce, finishOnce      sync.Once
}

func (m *delayedSpeedtestCallback) SetCallbackUpload(callback func(speedtest.ByteRate)) {
	m.Manager.SetCallbackUpload(func(rate speedtest.ByteRate) {
		m.pauseOnce.Do(func() {
			close(m.entered)
			<-m.release
		})
		callback(rate)
		m.finishOnce.Do(func() { close(m.finished) })
	})
}

func TestSpeedtestInitialSnapshotAndCallbackAfterCancellation(t *testing.T) {
	srv, _ := newMemorySpeedtest(t, 10*time.Millisecond)
	delayed := &delayedSpeedtestCallback{Manager: srv.Context.Manager, entered: make(chan struct{}), release: make(chan struct{}), finished: make(chan struct{})}
	srv.Context.Manager = delayed
	var releaseOnce sync.Once
	release := func() { releaseOnce.Do(func() { close(delayed.release) }) }
	defer release()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	result := SpeedTestResult{Tag: "cancelled"}
	done := make(chan error, 1)
	go func() { done <- speedTestWithServer(ctx, srv, &result, false, true, time.Second) }()
	select {
	case <-delayed.entered:
	case <-time.After(2 * time.Second):
		t.Fatal("capture callback did not start")
	}
	// The callback is paused, so this can only observe a live zero snapshot,
	// never the dependency's final result before isRunning is cleared.
	deadline := time.NewTimer(time.Second)
	defer deadline.Stop()
	ticker := time.NewTicker(5 * time.Millisecond)
	defer ticker.Stop()
initialSample:
	for {
		select {
		case <-ticker.C:
			if current, running := SpTQuerier.Result(); running && current.Tag == result.Tag {
				if current.DlSpeed != BrateToStr(0) || current.UlSpeed != BrateToStr(0) {
					t.Fatalf("rate before first callback must be zero: %+v", current)
				}
				break initialSample
			}
		case <-deadline.C:
			t.Fatal("no initial live snapshot")
		}
	}
	cancel()
	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) || !result.Cancelled {
			t.Fatalf("unexpected cancellation: %+v, %v", result, err)
		}
	case <-time.After(time.Second):
		t.Fatal("sampling did not return while callback was paused")
	}
	cancelledResult := result
	marker := SpeedTestResult{Tag: "next invocation", DlSpeed: "marker", UlSpeed: "marker"}
	SpTQuerier.storeResult(&marker)
	release()
	select {
	case <-delayed.finished:
	case <-time.After(time.Second):
		t.Fatal("late callback did not finish")
	}
	if current, running := SpTQuerier.Result(); current != marker || running || result != cancelledResult {
		t.Fatalf("late callback changed a returned/current result: current=%+v cancelled=%+v running=%v", current, result, running)
	}
}
