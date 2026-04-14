package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ----------------------------------------------------------------------------
// Build state
// ----------------------------------------------------------------------------

type Build struct {
	mu      sync.Mutex
	logs    []string
	subs    []chan string
	done    bool
	success bool
	uf2Path string
}

func (b *Build) appendLog(line string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.logs = append(b.logs, line)
	for _, ch := range b.subs {
		select {
		case ch <- line:
		default:
		}
	}
}

func (b *Build) subscribe() chan string {
	ch := make(chan string, 256)
	b.mu.Lock()
	// replay existing logs
	for _, l := range b.logs {
		ch <- l
	}
	if b.done {
		close(ch)
	} else {
		b.subs = append(b.subs, ch)
	}
	b.mu.Unlock()
	return ch
}

func (b *Build) finish(success bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.done = true
	b.success = success
	for _, ch := range b.subs {
		close(ch)
	}
	b.subs = nil
}

var (
	builds    sync.Map      // map[string]*Build
	buildLock sync.Mutex    // only one build at a time
	repoRoot  string
)

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

func runCmd(b *Build, dir string, env []string, name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	if len(env) > 0 {
		cmd.Env = append(os.Environ(), env...)
	}
	stdout, _ := cmd.StdoutPipe()
	stderr, _ := cmd.StderrPipe()

	if err := cmd.Start(); err != nil {
		return err
	}

	scan := func(r io.Reader) {
		sc := bufio.NewScanner(r)
		for sc.Scan() {
			b.appendLog(sc.Text())
		}
	}
	go scan(stdout)
	go scan(stderr)

	return cmd.Wait()
}

func changeFlashSize(sizeMB string) error {
	path := filepath.Join(repoRoot, "CMakeLists.txt")
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	s := string(data)
	// Replace any existing size with the new one
	sizes := []string{"2", "4", "16"}
	for _, old := range sizes {
		s = strings.ReplaceAll(s,
			fmt.Sprintf("(%s * 1024 * 1024)", old),
			fmt.Sprintf("(%s * 1024 * 1024)", sizeMB))
	}
	return os.WriteFile(path, []byte(s), 0644)
}

func writeCompileDefinitions(rgb, midi, v2 string) error {
	content := fmt.Sprintf(`target_compile_definitions(${PROJECT_NAME} PRIVATE
    WS2812_ENABLED=%s
    MIDI_IN_ENABLED=%s
    MIDI_RESET_EVERY_BEAT=16
    MIDI_CLOCK_MULTIPLIER=2
    MIDI_NOTE_KEY=0
    PCB_V2_LAYOUT=%s
)
`, rgb, midi, v2)
	return os.WriteFile(filepath.Join(repoRoot, "target_compile_definitions.cmake"), []byte(content), 0644)
}

func openBrowser(url string) {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		cmd = exec.Command("open", url)
	case "linux":
		cmd = exec.Command("xdg-open", url)
	default:
		return
	}
	_ = cmd.Start()
}

// ----------------------------------------------------------------------------
// Build pipeline
// ----------------------------------------------------------------------------

func runBuild(id string, audioDir string, sr, sizeMB, rgb, midi, v2 string) {
	b, _ := builds.Load(id)
	build := b.(*Build)

	buildLock.Lock()
	defer buildLock.Unlock()

	defer func() {
		if r := recover(); r != nil {
			build.appendLog(fmt.Sprintf("panic: %v", r))
			build.finish(false)
		}
	}()

	srInt, _ := strconv.Atoi(sr)
	if srInt <= 0 {
		srInt = 31000
	}

	build.appendLog("→ configuring build options...")
	if err := changeFlashSize(sizeMB); err != nil {
		build.appendLog("error updating CMakeLists.txt: " + err.Error())
		build.finish(false)
		return
	}
	if err := writeCompileDefinitions(rgb, midi, v2); err != nil {
		build.appendLog("error writing compile definitions: " + err.Error())
		build.finish(false)
		return
	}

	build.appendLog(fmt.Sprintf("→ generating easing.h..."))
	if err := runCmd(build,
		filepath.Join(repoRoot, "doth"), nil,
		"sh", "-c", "python3 generate_easing.py > easing.h && clang-format -i --style=google easing.h",
	); err != nil {
		build.appendLog("warning: easing.h generation failed (using existing): " + err.Error())
	}

	build.appendLog(fmt.Sprintf("→ generating filter.h for sample rate %d Hz...", srInt))
	if err := runCmd(build,
		filepath.Join(repoRoot, "doth"), nil,
		"sh", "-c", fmt.Sprintf("python3 biquad.py %d > filter.h && clang-format -i --style=google filter.h", srInt),
	); err != nil {
		build.appendLog("error generating filter.h: " + err.Error())
		build.finish(false)
		return
	}

	build.appendLog("→ converting audio files...")
	if err := runCmd(build,
		filepath.Join(repoRoot, "audio2h"), nil,
		"go", "run", "main.go",
		"--folder-in", audioDir,
		"--folder-out", "converted",
		"--bpm", "165",
		"--sr", strconv.Itoa(srInt),
		"--limit", "254",
	); err != nil {
		build.appendLog("error converting audio: " + err.Error())
		build.finish(false)
		return
	}

	build.appendLog("→ running cmake...")
	if err := os.MkdirAll(filepath.Join(repoRoot, "build"), 0755); err != nil {
		build.appendLog("error creating build dir: " + err.Error())
		build.finish(false)
		return
	}
	if err := runCmd(build, filepath.Join(repoRoot, "build"), nil, "cmake", ".."); err != nil {
		build.appendLog("error running cmake: " + err.Error())
		build.finish(false)
		return
	}

	build.appendLog("→ compiling firmware...")
	if err := runCmd(build, filepath.Join(repoRoot, "build"), nil, "make", "-j4"); err != nil {
		build.appendLog("error compiling: " + err.Error())
		build.finish(false)
		return
	}

	uf2 := filepath.Join(repoRoot, "build", "pikocore.uf2")
	if _, err := os.Stat(uf2); err != nil {
		build.appendLog("build completed but pikocore.uf2 not found")
		build.finish(false)
		return
	}

	build.mu.Lock()
	build.uf2Path = uf2
	build.mu.Unlock()

	build.appendLog("✓ build successful! pikocore.uf2 is ready.")
	build.finish(true)
}

// ----------------------------------------------------------------------------
// HTTP handlers
// ----------------------------------------------------------------------------

func handleIndex(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	fmt.Fprint(w, htmlPage)
}

func handleBuild(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if err := r.ParseMultipartForm(512 << 20); err != nil {
		http.Error(w, "failed to parse form: "+err.Error(), http.StatusBadRequest)
		return
	}

	// Save uploaded audio files to a temp dir
	tmpDir, err := os.MkdirTemp("", "pikocore-audio-*")
	if err != nil {
		http.Error(w, "failed to create temp dir", http.StatusInternalServerError)
		return
	}

	files := r.MultipartForm.File["files"]
	if len(files) == 0 {
		http.Error(w, "no audio files uploaded", http.StatusBadRequest)
		return
	}

	for _, fh := range files {
		src, err := fh.Open()
		if err != nil {
			http.Error(w, "failed to read file: "+err.Error(), http.StatusBadRequest)
			return
		}
		dst, err := os.Create(filepath.Join(tmpDir, fh.Filename))
		if err != nil {
			src.Close()
			http.Error(w, "failed to save file", http.StatusInternalServerError)
			return
		}
		_, err = io.Copy(dst, src)
		src.Close()
		dst.Close()
		if err != nil {
			http.Error(w, "failed to save file", http.StatusInternalServerError)
			return
		}
	}

	sr := r.FormValue("sr")
	if sr == "" {
		sr = "31000"
	}
	sizeMB := r.FormValue("size")
	if sizeMB == "" {
		sizeMB = "16"
	}
	rgb := r.FormValue("rgb")
	if rgb == "" {
		rgb = "1"
	}
	midi := r.FormValue("midi")
	if midi == "" {
		midi = "0"
	}
	v2 := r.FormValue("v2")
	if v2 == "" {
		v2 = "0"
	}

	id := strconv.FormatInt(time.Now().UnixMilli(), 36)
	build := &Build{}
	builds.Store(id, build)

	go runBuild(id, tmpDir, sr, sizeMB, rgb, midi, v2)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"id": id})
}

func handleEvents(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("id")
	val, ok := builds.Load(id)
	if !ok {
		http.Error(w, "build not found", http.StatusNotFound)
		return
	}
	build := val.(*Build)

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", http.StatusInternalServerError)
		return
	}

	ch := build.subscribe()
	for {
		select {
		case line, open := <-ch:
			if !open {
				// build done — send final status event
				build.mu.Lock()
				success := build.success
				build.mu.Unlock()
				if success {
					fmt.Fprintf(w, "event: done\ndata: success\n\n")
				} else {
					fmt.Fprintf(w, "event: done\ndata: error\n\n")
				}
				flusher.Flush()
				return
			}
			// Escape newlines for SSE
			escaped := strings.ReplaceAll(line, "\n", "\\n")
			fmt.Fprintf(w, "data: %s\n\n", escaped)
			flusher.Flush()
		case <-r.Context().Done():
			return
		}
	}
}

func handleDownload(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("id")
	val, ok := builds.Load(id)
	if !ok {
		http.Error(w, "build not found", http.StatusNotFound)
		return
	}
	build := val.(*Build)
	build.mu.Lock()
	uf2 := build.uf2Path
	build.mu.Unlock()

	if uf2 == "" {
		http.Error(w, "firmware not ready", http.StatusNotFound)
		return
	}

	w.Header().Set("Content-Disposition", "attachment; filename=pikocore.uf2")
	w.Header().Set("Content-Type", "application/octet-stream")
	http.ServeFile(w, r, uf2)
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------

func gitInfo() (branch, commit string) {
	branch = "unknown"
	commit = "unknown"
	if b, err := exec.Command("git", "rev-parse", "--abbrev-ref", "HEAD").Output(); err == nil {
		branch = strings.TrimSpace(string(b))
	}
	if c, err := exec.Command("git", "rev-parse", "--short", "HEAD").Output(); err == nil {
		commit = strings.TrimSpace(string(c))
	}
	return
}

func handleInfo(w http.ResponseWriter, r *http.Request) {
	branch, commit := gitInfo()
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{
		"branch": branch,
		"commit": commit,
	})
}

func main() {
	var err error
	repoRoot, err = os.Getwd()
	if err != nil {
		log.Fatal(err)
	}

	// Verify we're in the right place
	if _, err := os.Stat(filepath.Join(repoRoot, "CMakeLists.txt")); err != nil {
		log.Fatal("run this from the pikocore repo root: go run server/main.go")
	}

	http.HandleFunc("/", handleIndex)
	http.HandleFunc("/build", handleBuild)
	http.HandleFunc("/events", handleEvents)
	http.HandleFunc("/download", handleDownload)
	http.HandleFunc("/info", handleInfo)

	port := "8765"
	url := "http://localhost:" + port
	log.Printf("pikocore firmware builder → %s", url)

	go func() {
		time.Sleep(400 * time.Millisecond)
		openBrowser(url)
	}()

	log.Fatal(http.ListenAndServe(":"+port, nil))
}

// ----------------------------------------------------------------------------
// Embedded UI
// ----------------------------------------------------------------------------

const htmlPage = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>pikocore — build firmware</title>
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

:root {
  --bg:        #0d1117;
  --surface:   #161b22;
  --surface2:  #21262d;
  --border:    #30363d;
  --accent:    #58a6ff;
  --accent-dim:#1f3a5f;
  --success:   #3fb950;
  --success-dim:#122d1a;
  --warning:   #d29922;
  --warning-dim:#2d2209;
  --error:     #f85149;
  --error-dim: #3d1212;
  --text:      #e6edf3;
  --text-muted:#8b949e;
  --radius:    8px;
  --radius-lg: 12px;
}

body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  background: var(--bg);
  color: var(--text);
  min-height: 100vh;
  font-size: 14px;
  line-height: 1.5;
}

/* ── Layout ── */
.app-header {
  border-bottom: 1px solid var(--border);
  padding: 0.75rem 1.5rem;
  display: flex;
  align-items: center;
  justify-content: space-between;
  position: sticky;
  top: 0;
  background: var(--bg);
  z-index: 10;
}
.app-header .logo {
  font-size: 1rem;
  font-weight: 700;
  letter-spacing: -0.02em;
  display: flex;
  align-items: center;
  gap: 0.5rem;
}
.app-header .logo span { color: var(--text-muted); font-weight: 400; }
.git-badge {
  display: flex;
  align-items: center;
  gap: 0.4rem;
  background: var(--surface2);
  border: 1px solid var(--border);
  border-radius: 20px;
  padding: 0.2rem 0.65rem;
  font-size: 0.75rem;
  color: var(--text-muted);
  font-family: "SF Mono", "Fira Code", monospace;
}
.git-badge .dot {
  width: 6px; height: 6px;
  border-radius: 50%;
  background: var(--accent);
  flex-shrink: 0;
}

.main {
  max-width: 1100px;
  margin: 0 auto;
  padding: 1.5rem;
  display: grid;
  grid-template-columns: 360px 1fr;
  gap: 1.5rem;
  align-items: start;
}

/* ── Info banner ── */
.info-banner {
  grid-column: 1 / -1;
  background: var(--warning-dim);
  border: 1px solid var(--warning);
  border-radius: var(--radius);
  padding: 0.75rem 1rem;
  display: flex;
  align-items: flex-start;
  gap: 0.6rem;
  font-size: 0.83rem;
  color: #e3c36a;
  line-height: 1.5;
}
.info-banner .icon { flex-shrink: 0; margin-top: 1px; font-size: 0.95rem; }
.info-banner strong { color: #f0d080; }

/* ── Cards ── */
.card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius-lg);
  overflow: hidden;
}
.card-header {
  padding: 0.75rem 1rem;
  border-bottom: 1px solid var(--border);
  display: flex;
  align-items: center;
  gap: 0.5rem;
}
.step-badge {
  width: 20px; height: 20px;
  border-radius: 50%;
  background: var(--accent-dim);
  border: 1px solid var(--accent);
  color: var(--accent);
  font-size: 0.7rem;
  font-weight: 700;
  display: flex;
  align-items: center;
  justify-content: center;
  flex-shrink: 0;
}
.step-badge.done {
  background: var(--success-dim);
  border-color: var(--success);
  color: var(--success);
}
.card-header h2 {
  font-size: 0.82rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.07em;
  color: var(--text-muted);
}
.card-body { padding: 1rem; }

/* ── Drop zone ── */
#dropzone {
  border: 2px dashed var(--border);
  border-radius: var(--radius);
  padding: 1.5rem 1rem;
  text-align: center;
  cursor: pointer;
  transition: all 0.15s;
  margin-bottom: 0.75rem;
}
#dropzone:hover, #dropzone.over {
  border-color: var(--accent);
  background: var(--accent-dim);
}
#dropzone .dz-icon { font-size: 1.8rem; margin-bottom: 0.4rem; }
#dropzone p { color: var(--text-muted); font-size: 0.83rem; }
#dropzone strong { color: var(--accent); }
#file-input { display: none; }

/* ── File list ── */
#file-list {
  list-style: none;
  max-height: 150px;
  overflow-y: auto;
  margin-bottom: 0.5rem;
  border-radius: var(--radius);
  border: 1px solid var(--border);
}
#file-list:empty { display: none; }
#file-list li {
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.35rem 0.6rem;
  border-bottom: 1px solid var(--border);
  font-size: 0.8rem;
}
#file-list li:last-child { border-bottom: none; }
#file-list li .fname { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--text); }
#file-list li .fsize { color: var(--text-muted); white-space: nowrap; }
#file-list li button {
  background: none; border: none; cursor: pointer;
  color: var(--text-muted); font-size: 0.85rem; line-height: 1; padding: 0 2px;
  transition: color 0.1s;
}
#file-list li button:hover { color: var(--error); }
.file-count {
  font-size: 0.75rem;
  color: var(--text-muted);
  text-align: right;
  margin-bottom: 0.75rem;
}

/* ── Options ── */
.opts { display: flex; flex-direction: column; gap: 0; }
.opt-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0.55rem 0;
  border-bottom: 1px solid var(--border);
  gap: 0.5rem;
}
.opt-row:last-child { border-bottom: none; }
.opt-label { font-size: 0.84rem; color: var(--text); }
.opt-hint  { font-size: 0.73rem; color: var(--text-muted); margin-top: 1px; }
.opt-row select, .opt-row input[type=number] {
  background: var(--surface2);
  border: 1px solid var(--border);
  color: var(--text);
  border-radius: 6px;
  padding: 0.3rem 0.5rem;
  font-size: 0.82rem;
  width: 110px;
  outline: none;
  transition: border-color 0.15s;
}
.opt-row select:focus, .opt-row input:focus { border-color: var(--accent); }

/* ── Build button ── */
#build-btn {
  width: 100%;
  padding: 0.7rem;
  background: var(--accent);
  color: #0d1117;
  font-size: 0.9rem;
  font-weight: 700;
  border: none;
  border-radius: var(--radius);
  cursor: pointer;
  transition: background 0.15s, opacity 0.15s;
  margin-top: 1rem;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 0.4rem;
}
#build-btn:hover:not(:disabled) { background: #79b8ff; }
#build-btn:disabled { opacity: 0.35; cursor: not-allowed; }

/* ── Right panel ── */
.right-panel { display: flex; flex-direction: column; gap: 1rem; }

/* ── Progress steps ── */
.build-steps {
  display: flex;
  gap: 0;
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: var(--radius-lg);
  overflow: hidden;
}
.build-step {
  flex: 1;
  padding: 0.6rem 0.5rem;
  text-align: center;
  font-size: 0.75rem;
  color: var(--text-muted);
  border-right: 1px solid var(--border);
  transition: all 0.2s;
  position: relative;
}
.build-step:last-child { border-right: none; }
.build-step .bs-icon { font-size: 1rem; display: block; margin-bottom: 2px; }
.build-step.active { background: var(--accent-dim); color: var(--accent); }
.build-step.done   { background: var(--success-dim); color: var(--success); }
.build-step.error  { background: var(--error-dim);   color: var(--error);   }

/* ── Log ── */
.log-card { flex: 1; }
#log-wrap {
  background: #0a0d12;
  border: 1px solid var(--border);
  border-radius: var(--radius);
  height: 320px;
  overflow-y: auto;
  padding: 0.75rem 1rem;
  font-family: "SF Mono", "Fira Code", "JetBrains Mono", monospace;
  font-size: 0.75rem;
  line-height: 1.65;
  color: #8b949e;
}
#log-wrap .l-step  { color: #79b8ff; }
#log-wrap .l-ok    { color: var(--success); }
#log-wrap .l-warn  { color: var(--warning); }
#log-wrap .l-error { color: var(--error); }

/* ── Download / flash ── */
.flash-card { display: none; }
.flash-card.visible { display: block; }

.flash-inner {
  background: var(--success-dim);
  border: 1px solid var(--success);
  border-radius: var(--radius-lg);
  overflow: hidden;
}
.flash-top {
  padding: 1rem;
  border-bottom: 1px solid rgba(63,185,80,0.25);
}
.flash-top h3 { font-size: 0.95rem; font-weight: 700; color: var(--success); margin-bottom: 0.25rem; }
.flash-top p  { font-size: 0.8rem; color: #6ecea8; }

#download-btn {
  display: block;
  width: calc(100% - 2rem);
  margin: 1rem;
  padding: 0.7rem;
  background: var(--success);
  color: #0d2818;
  font-size: 0.9rem;
  font-weight: 700;
  border: none;
  border-radius: var(--radius);
  cursor: pointer;
  text-align: center;
  text-decoration: none;
  transition: background 0.15s;
}
#download-btn:hover { background: #56d364; }

.flash-steps {
  padding: 0 1rem 1rem;
}
.flash-steps h4 {
  font-size: 0.73rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.07em;
  color: #6ecea8;
  margin-bottom: 0.6rem;
}
.flash-steps ol {
  list-style: none;
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
}
.flash-steps ol li {
  display: flex;
  gap: 0.6rem;
  align-items: flex-start;
  font-size: 0.8rem;
  color: #6ecea8;
}
.flash-steps ol li .num {
  background: rgba(63,185,80,0.2);
  border: 1px solid rgba(63,185,80,0.4);
  border-radius: 50%;
  width: 18px; height: 18px;
  display: flex; align-items: center; justify-content: center;
  font-size: 0.68rem; font-weight: 700;
  flex-shrink: 0;
  color: var(--success);
}

.error-card { display: none; }
.error-card.visible {
  display: block;
  background: var(--error-dim);
  border: 1px solid var(--error);
  border-radius: var(--radius-lg);
  padding: 1rem;
  font-size: 0.83rem;
  color: #ffa0a0;
}
.error-card h3 { color: var(--error); margin-bottom: 0.4rem; font-size: 0.9rem; }

/* ── Overwrite warning ── */
.overwrite-warn {
  display: flex;
  gap: 0.5rem;
  align-items: flex-start;
  background: var(--warning-dim);
  border: 1px solid rgba(210,153,34,0.4);
  border-radius: var(--radius);
  padding: 0.65rem 0.75rem;
  font-size: 0.78rem;
  color: #c9a840;
  margin: 0 1rem 1rem;
}
.overwrite-warn .icon { flex-shrink: 0; }

@media (max-width: 820px) {
  .main { grid-template-columns: 1fr; }
  .info-banner { grid-column: 1; }
}
</style>
</head>
<body>

<header class="app-header">
  <div class="logo">pikocore <span>/ firmware builder</span></div>
  <div class="git-badge">
    <span class="dot"></span>
    <span id="git-branch">loading…</span>
    <span style="color:#444">@</span>
    <span id="git-commit">…</span>
  </div>
</header>

<div class="main">

  <!-- Info banner -->
  <div class="info-banner">
    <span class="icon">💡</span>
    <div>
      <strong>Your audio files become the firmware.</strong>
      pikocore has no SD card or file system — samples are compiled directly into flash memory.
      Building generates a single <code>.uf2</code> binary that contains both the firmware logic
      <em>and</em> your audio. Flashing it <strong>completely replaces</strong> everything currently on the device.
    </div>
  </div>

  <!-- LEFT column -->
  <div style="display:flex;flex-direction:column;gap:1rem">

    <!-- Step 1: Audio -->
    <div class="card">
      <div class="card-header">
        <div class="step-badge" id="step1-badge">1</div>
        <h2>Audio samples</h2>
        <button id="tips-btn" title="Naming tips" onclick="toggleTips()" style="margin-left:auto;background:none;border:1px solid var(--border);border-radius:20px;color:var(--text-muted);cursor:pointer;font-size:0.72rem;padding:0.15rem 0.55rem;transition:all 0.15s">ⓘ tips</button>
      </div>

      <!-- Collapsible tips panel -->
      <div id="tips-panel" style="display:none;border-bottom:1px solid var(--border);background:var(--surface2);padding:0.85rem 1rem;font-size:0.78rem;line-height:1.7;color:var(--text-muted)">
        <div style="display:grid;gap:0.6rem">

          <div>
            <span style="color:var(--text);font-weight:600">Supported formats</span><br>
            WAV · FLAC · MP3 · AIF · OGG
          </div>

          <div>
            <span style="color:var(--text);font-weight:600">Embed BPM in the filename</span><br>
            Include <code style="color:var(--accent)">_bpmXXX</code> anywhere in the name and pikocore will read it directly.
            Audio is then time-stretched to match the build's target BPM.<br>
            <span style="color:var(--text-muted);font-size:0.73rem">e.g. <code>amen_bpm170.wav</code> or <code>loop170bpm.flac</code></span>
          </div>

          <div>
            <span style="color:var(--text);font-weight:600">Embed beat count in the filename</span><br>
            Include <code style="color:var(--accent)">_beatsN</code> to tell pikocore how many beats are in the loop.
            More beats = finer slicing granularity on the buttons.<br>
            <span style="color:var(--text-muted);font-size:0.73rem">e.g. <code>amen_beats16_bpm170.wav</code></span>
          </div>

          <div>
            <span style="color:var(--text);font-weight:600">No BPM in filename?</span><br>
            pikocore will try to detect it automatically. Works best for loops between 100–200 BPM. For best results, always embed the BPM.
          </div>

          <div>
            <span style="color:var(--text);font-weight:600">Multiple samples</span><br>
            Upload up to 254 files. Each maps to a sample slot selectable via the knob. Files are loaded in alphabetical order — prefix with numbers to control ordering (e.g. <code>01_kick.wav</code>, <code>02_snare.wav</code>).
          </div>

        </div>
      </div>

      <div class="card-body">
        <div id="dropzone">
          <div class="dz-icon">🎵</div>
          <p><strong>Click to choose</strong> or drag &amp; drop</p>
          <p style="margin-top:3px;font-size:0.73rem">WAV · FLAC · MP3 &nbsp;·&nbsp; up to 254 files</p>
        </div>
        <input type="file" id="file-input" multiple accept="audio/*">
        <ul id="file-list"></ul>
        <div class="file-count" id="file-count"></div>
      </div>
    </div>

    <!-- Step 2: Options -->
    <div class="card">
      <div class="card-header">
        <div class="step-badge" id="step2-badge">2</div>
        <h2>Build options</h2>
      </div>
      <div class="card-body">
        <div class="opts">
          <div class="opt-row">
            <div><div class="opt-label">Pico flash size</div></div>
            <select id="opt-size">
              <option value="16" selected>16 mb</option>
              <option value="4">4 mb</option>
              <option value="2">2 mb</option>
            </select>
          </div>
          <div class="opt-row">
            <div>
              <div class="opt-label">Sample rate</div>
              <div class="opt-hint">lower if build fails</div>
            </div>
            <input type="number" id="opt-sr" value="31000" min="8000" max="48000" step="1000">
          </div>
          <div class="opt-row">
            <div>
              <div class="opt-label">RGB LED</div>
            </div>
            <select id="opt-rgb">
              <option value="1" selected>enabled</option>
              <option value="0">disabled</option>
            </select>
          </div>
          <div class="opt-row">
            <div>
              <div class="opt-label">Sync input</div>
              <div class="opt-hint">clock in or MIDI (itty bitty midi)</div>
            </div>
            <select id="opt-midi">
              <option value="0" selected>clock</option>
              <option value="1">midi</option>
            </select>
          </div>
          <div class="opt-row">
            <div>
              <div class="opt-label">PCB V2 layout</div>
              <div class="opt-hint">knobs A &amp; B swapped</div>
            </div>
            <select id="opt-v2">
              <option value="0" selected>no</option>
              <option value="1">yes</option>
            </select>
          </div>
        </div>
        <button id="build-btn" disabled>
          <span id="build-btn-icon">🔨</span>
          <span id="build-btn-label">Build firmware</span>
        </button>
      </div>
    </div>

  </div>

  <!-- RIGHT column -->
  <div class="right-panel">

    <!-- Step progress bar -->
    <div class="build-steps">
      <div class="build-step" id="ps-audio">
        <span class="bs-icon">🎵</span>convert audio
      </div>
      <div class="build-step" id="ps-codegen">
        <span class="bs-icon">⚙️</span>codegen
      </div>
      <div class="build-step" id="ps-cmake">
        <span class="bs-icon">📐</span>cmake
      </div>
      <div class="build-step" id="ps-compile">
        <span class="bs-icon">🔧</span>compile
      </div>
      <div class="build-step" id="ps-done">
        <span class="bs-icon">✅</span>ready
      </div>
    </div>

    <!-- Build log -->
    <div class="card log-card">
      <div class="card-header">
        <div class="step-badge" id="step3-badge">3</div>
        <h2>Build log</h2>
      </div>
      <div class="card-body" style="padding:0.5rem">
        <div id="log-wrap"><span class="l-step">waiting for build…</span></div>
      </div>
    </div>

    <!-- Success: download + flash instructions -->
    <div class="flash-card" id="flash-card">
      <div class="flash-inner">
        <div class="flash-top">
          <h3>✓ Firmware ready</h3>
          <p>Your audio has been compiled into the firmware and is ready to flash.</p>
        </div>
        <a id="download-btn" href="#">⬇ Download pikocore.uf2</a>
        <div class="overwrite-warn">
          <span class="icon">⚠️</span>
          <span>Flashing this file will <strong>completely erase</strong> the existing firmware and samples on your pikocore. There is no undo.</span>
        </div>
        <div class="flash-steps">
          <h4>How to flash</h4>
          <ol>
            <li><span class="num">1</span><span>Hold the <strong>BOOTSEL</strong> button on the Pico, then plug it in via USB while holding it.</span></li>
            <li><span class="num">2</span><span>Release BOOTSEL. A drive called <strong>RPI-RP2</strong> will appear on your desktop.</span></li>
            <li><span class="num">3</span><span>Drag and drop <strong>pikocore.uf2</strong> onto the <strong>RPI-RP2</strong> drive.</span></li>
            <li><span class="num">4</span><span>The drive disappears and pikocore reboots automatically — done!</span></li>
          </ol>
        </div>
      </div>
    </div>

    <!-- Error card -->
    <div class="error-card" id="error-card">
      <h3>Build failed</h3>
      <p>Check the log above for details. Common causes: missing toolchain, sample rate too high, or audio files in unsupported format.</p>
    </div>

  </div>
</div>

<script>
  // ── Git info ──
  fetch('/info').then(r => r.json()).then(d => {
    document.getElementById('git-branch').textContent = d.branch;
    document.getElementById('git-commit').textContent = d.commit;
  }).catch(() => {});

  // ── Tips toggle ──
  function toggleTips() {
    const panel = document.getElementById('tips-panel');
    const btn   = document.getElementById('tips-btn');
    const open  = panel.style.display === 'none';
    panel.style.display = open ? 'block' : 'none';
    btn.style.background  = open ? 'var(--accent-dim)' : 'none';
    btn.style.borderColor = open ? 'var(--accent)'     : 'var(--border)';
    btn.style.color       = open ? 'var(--accent)'     : 'var(--text-muted)';
  }

  // ── File handling ──
  const dropzone  = document.getElementById('dropzone');
  const fileInput = document.getElementById('file-input');
  const fileList  = document.getElementById('file-list');
  const fileCount = document.getElementById('file-count');
  const buildBtn  = document.getElementById('build-btn');
  const step1     = document.getElementById('step1-badge');
  const step2     = document.getElementById('step2-badge');
  const step3     = document.getElementById('step3-badge');

  let selectedFiles = [];

  function fmtBytes(b) {
    if (b < 1024)       return b + ' B';
    if (b < 1048576)    return (b/1024).toFixed(1) + ' KB';
    return (b/1048576).toFixed(1) + ' MB';
  }

  function refreshFileList() {
    fileList.innerHTML = '';
    selectedFiles.forEach((f, i) => {
      const li = document.createElement('li');
      const nm = document.createElement('span'); nm.className = 'fname'; nm.textContent = f.name;
      const sz = document.createElement('span'); sz.className = 'fsize'; sz.textContent = fmtBytes(f.size);
      const rm = document.createElement('button'); rm.textContent = '✕'; rm.title = 'Remove';
      rm.onclick = () => { selectedFiles.splice(i, 1); refreshFileList(); };
      li.append(nm, sz, rm);
      fileList.appendChild(li);
    });
    const n = selectedFiles.length;
    fileCount.textContent = n > 0 ? n + ' file' + (n > 1 ? 's' : '') + ' selected' : '';
    buildBtn.disabled = n === 0;
    if (n > 0) step1.classList.add('done'); else step1.classList.remove('done');
  }

  dropzone.addEventListener('click', () => fileInput.click());
  dropzone.addEventListener('dragover', e => { e.preventDefault(); dropzone.classList.add('over'); });
  dropzone.addEventListener('dragleave', () => dropzone.classList.remove('over'));
  dropzone.addEventListener('drop', e => {
    e.preventDefault(); dropzone.classList.remove('over');
    addFiles(Array.from(e.dataTransfer.files));
  });
  fileInput.addEventListener('change', () => { addFiles(Array.from(fileInput.files)); fileInput.value = ''; });

  function addFiles(files) {
    files.forEach(f => { if (!selectedFiles.find(x => x.name === f.name)) selectedFiles.push(f); });
    refreshFileList();
  }

  // ── Log ──
  const logWrap = document.getElementById('log-wrap');
  function appendLog(text) {
    const s = document.createElement('span');
    if      (text.startsWith('→'))                       s.className = 'l-step';
    else if (text.startsWith('✓'))                       s.className = 'l-ok';
    else if (text.toLowerCase().includes('warning'))     s.className = 'l-warn';
    else if (text.toLowerCase().includes('error'))       s.className = 'l-error';
    s.textContent = text + '\n';
    logWrap.appendChild(s);
    logWrap.scrollTop = logWrap.scrollHeight;
  }

  // ── Progress steps ──
  const steps = {
    audio:   document.getElementById('ps-audio'),
    codegen: document.getElementById('ps-codegen'),
    cmake:   document.getElementById('ps-cmake'),
    compile: document.getElementById('ps-compile'),
    done:    document.getElementById('ps-done'),
  };

  function resetSteps() {
    Object.values(steps).forEach(el => el.className = 'build-step');
  }
  function setStep(name) {
    // mark previous steps done
    const order = ['audio','codegen','cmake','compile','done'];
    const idx = order.indexOf(name);
    order.forEach((k, i) => {
      if (i < idx)       steps[k].className = 'build-step done';
      else if (i === idx) steps[k].className = 'build-step active';
      else               steps[k].className = 'build-step';
    });
  }
  function allStepsDone(success) {
    Object.values(steps).forEach(el => {
      el.className = 'build-step ' + (success ? 'done' : 'error');
    });
  }

  function inferStep(line) {
    if (line.includes('converting audio'))  setStep('audio');
    if (line.includes('generating'))        setStep('codegen');
    if (line.includes('running cmake'))     setStep('cmake');
    if (line.includes('compiling firmware'))setStep('compile');
    if (line.includes('build successful'))  setStep('done');
  }

  // ── Build ──
  buildBtn.addEventListener('click', async () => {
    if (selectedFiles.length === 0) return;

    // Reset UI
    buildBtn.disabled = true;
    document.getElementById('build-btn-label').textContent = 'Building…';
    document.getElementById('flash-card').classList.remove('visible');
    document.getElementById('error-card').classList.remove('visible');
    logWrap.innerHTML = '';
    resetSteps();
    step2.classList.add('done');
    step3.className = 'step-badge';

    const form = new FormData();
    selectedFiles.forEach(f => form.append('files', f));
    form.append('sr',   document.getElementById('opt-sr').value);
    form.append('size', document.getElementById('opt-size').value);
    form.append('rgb',  document.getElementById('opt-rgb').value);
    form.append('midi', document.getElementById('opt-midi').value);
    form.append('v2',   document.getElementById('opt-v2').value);

    let buildId;
    try {
      const res = await fetch('/build', { method: 'POST', body: form });
      if (!res.ok) {
        appendLog('server error: ' + await res.text());
        buildBtn.disabled = false;
        document.getElementById('build-btn-label').textContent = 'Build firmware';
        return;
      }
      buildId = (await res.json()).id;
    } catch(e) {
      appendLog('network error: ' + e.message);
      buildBtn.disabled = false;
      document.getElementById('build-btn-label').textContent = 'Build firmware';
      return;
    }

    const es = new EventSource('/events?id=' + buildId);
    es.onmessage = e => { appendLog(e.data); inferStep(e.data); };
    es.addEventListener('done', e => {
      es.close();
      const ok = e.data === 'success';
      allStepsDone(ok);
      step3.classList.add(ok ? 'done' : '');
      if (ok) {
        document.getElementById('download-btn').href = '/download?id=' + buildId;
        document.getElementById('flash-card').classList.add('visible');
      } else {
        document.getElementById('error-card').classList.add('visible');
      }
      document.getElementById('build-btn-label').textContent = 'Build firmware';
      buildBtn.disabled = false;
    });
    es.onerror = () => {
      appendLog('connection lost');
      es.close();
      document.getElementById('build-btn-label').textContent = 'Build firmware';
      buildBtn.disabled = false;
    };
  });
</script>
</body>
</html>
`
