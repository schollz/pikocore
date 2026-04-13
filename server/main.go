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
<title>pikocore firmware builder</title>
<style>
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  :root {
    --bg:        #111318;
    --surface:   #1c1f27;
    --border:    #2e3140;
    --accent:    #6c8cff;
    --accent2:   #a78bfa;
    --success:   #34d399;
    --error:     #f87171;
    --text:      #e2e8f0;
    --muted:     #64748b;
    --radius:    10px;
  }

  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
    padding: 2rem;
  }

  header {
    margin-bottom: 2rem;
  }
  header h1 {
    font-size: 1.6rem;
    font-weight: 700;
    letter-spacing: -0.02em;
  }
  header p {
    color: var(--muted);
    margin-top: 0.25rem;
    font-size: 0.9rem;
  }

  .layout {
    display: grid;
    grid-template-columns: 380px 1fr;
    gap: 1.5rem;
    align-items: start;
  }

  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 1.5rem;
  }

  .card h2 {
    font-size: 0.8rem;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.08em;
    color: var(--muted);
    margin-bottom: 1rem;
  }

  /* Drop zone */
  #dropzone {
    border: 2px dashed var(--border);
    border-radius: var(--radius);
    padding: 2rem 1rem;
    text-align: center;
    cursor: pointer;
    transition: border-color 0.2s, background 0.2s;
    margin-bottom: 1rem;
  }
  #dropzone:hover, #dropzone.over {
    border-color: var(--accent);
    background: rgba(108, 140, 255, 0.05);
  }
  #dropzone svg {
    width: 32px;
    height: 32px;
    color: var(--muted);
    margin-bottom: 0.5rem;
  }
  #dropzone p { color: var(--muted); font-size: 0.85rem; }
  #dropzone strong { color: var(--accent); }
  #file-input { display: none; }

  #file-list {
    list-style: none;
    margin-bottom: 1rem;
    max-height: 160px;
    overflow-y: auto;
  }
  #file-list li {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    font-size: 0.82rem;
    padding: 0.3rem 0;
    border-bottom: 1px solid var(--border);
    color: var(--muted);
  }
  #file-list li:last-child { border-bottom: none; }
  #file-list li span { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  #file-list li button {
    background: none;
    border: none;
    color: var(--error);
    cursor: pointer;
    font-size: 0.9rem;
    line-height: 1;
    padding: 0 2px;
  }

  /* Options */
  .options { display: flex; flex-direction: column; gap: 0.75rem; margin-bottom: 1.25rem; }
  .option-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.5rem;
  }
  .option-row label {
    font-size: 0.85rem;
    color: var(--text);
  }
  .option-row select, .option-row input[type=number] {
    background: var(--bg);
    border: 1px solid var(--border);
    color: var(--text);
    border-radius: 6px;
    padding: 0.3rem 0.5rem;
    font-size: 0.85rem;
    width: 100px;
    outline: none;
  }
  .option-row select:focus, .option-row input[type=number]:focus {
    border-color: var(--accent);
  }

  /* Build button */
  #build-btn {
    width: 100%;
    padding: 0.75rem;
    background: var(--accent);
    color: #fff;
    font-size: 0.95rem;
    font-weight: 600;
    border: none;
    border-radius: var(--radius);
    cursor: pointer;
    transition: background 0.2s, opacity 0.2s;
  }
  #build-btn:hover { background: #7c9eff; }
  #build-btn:disabled { opacity: 0.4; cursor: not-allowed; }

  /* Log terminal */
  #log-card { display: flex; flex-direction: column; height: 540px; }
  #log-card h2 { flex-shrink: 0; }
  #log {
    flex: 1;
    background: #0a0c10;
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 0.75rem 1rem;
    font-family: "JetBrains Mono", "Fira Code", "SF Mono", monospace;
    font-size: 0.78rem;
    line-height: 1.6;
    overflow-y: auto;
    white-space: pre-wrap;
    word-break: break-all;
    color: #a0aec0;
  }
  #log .log-arrow  { color: var(--accent2); }
  #log .log-ok     { color: var(--success); }
  #log .log-error  { color: var(--error); }

  /* Download */
  #download-area {
    margin-top: 1rem;
    display: none;
  }
  #download-btn {
    width: 100%;
    padding: 0.75rem;
    background: var(--success);
    color: #064e3b;
    font-size: 0.95rem;
    font-weight: 700;
    border: none;
    border-radius: var(--radius);
    cursor: pointer;
    text-decoration: none;
    display: block;
    text-align: center;
    transition: background 0.2s;
  }
  #download-btn:hover { background: #6ee7b7; }

  #status-badge {
    margin-top: 0.75rem;
    font-size: 0.8rem;
    text-align: center;
    color: var(--muted);
  }
  #status-badge.success { color: var(--success); }
  #status-badge.error   { color: var(--error); }
  #status-badge.running { color: var(--accent); }

  @media (max-width: 800px) {
    .layout { grid-template-columns: 1fr; }
    #log-card { height: 360px; }
  }
</style>
</head>
<body>

<header>
  <h1>pikocore firmware builder</h1>
  <p>Build custom firmware locally — no internet required.</p>
</header>

<div class="layout">

  <!-- LEFT: upload + options -->
  <div>
    <div class="card" style="margin-bottom:1rem">
      <h2>Audio files</h2>
      <div id="dropzone">
        <svg fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round"
            d="M9 8.25H7.5a2.25 2.25 0 0 0-2.25 2.25v9a2.25 2.25 0 0 0 2.25
               2.25h9a2.25 2.25 0 0 0 2.25-2.25v-9a2.25 2.25 0 0 0-2.25-2.25H15
               M9 12l3 3m0 0 3-3m-3 3V2.25"/>
        </svg>
        <p><strong>Click to choose</strong> or drag &amp; drop audio files</p>
        <p style="margin-top:0.25rem;font-size:0.75rem">(WAV, FLAC, MP3 · max 254 files)</p>
      </div>
      <input type="file" id="file-input" multiple accept="audio/*">
      <ul id="file-list"></ul>
    </div>

    <div class="card">
      <h2>Build options</h2>
      <div class="options">
        <div class="option-row">
          <label>Pico flash size</label>
          <select id="opt-size">
            <option value="16" selected>16 mb</option>
            <option value="4">4 mb</option>
            <option value="2">2 mb</option>
          </select>
        </div>
        <div class="option-row">
          <label>Sample rate (Hz)</label>
          <input type="number" id="opt-sr" value="31000" min="8000" max="48000" step="1000">
        </div>
        <div class="option-row">
          <label>RGB LED</label>
          <select id="opt-rgb">
            <option value="1" selected>enabled</option>
            <option value="0">disabled</option>
          </select>
        </div>
        <div class="option-row">
          <label>Input type</label>
          <select id="opt-midi">
            <option value="0" selected>clock</option>
            <option value="1">midi</option>
          </select>
        </div>
        <div class="option-row">
          <label>PCB V2 layout</label>
          <select id="opt-v2">
            <option value="0" selected>no</option>
            <option value="1">yes</option>
          </select>
        </div>
      </div>
      <button id="build-btn" disabled>Build firmware</button>
    </div>
  </div>

  <!-- RIGHT: log + download -->
  <div class="card" id="log-card">
    <h2>Build log</h2>
    <div id="log"><span class="log-arrow">waiting for build to start…</span></div>
    <div id="download-area">
      <a id="download-btn" href="#">⬇ Download pikocore.uf2</a>
    </div>
    <div id="status-badge"></div>
  </div>

</div>

<script>
  const dropzone   = document.getElementById('dropzone');
  const fileInput  = document.getElementById('file-input');
  const fileList   = document.getElementById('file-list');
  const buildBtn   = document.getElementById('build-btn');
  const logEl      = document.getElementById('log');
  const dlArea     = document.getElementById('download-area');
  const dlBtn      = document.getElementById('download-btn');
  const statusEl   = document.getElementById('status-badge');

  let selectedFiles = [];

  function refreshFileList() {
    fileList.innerHTML = '';
    selectedFiles.forEach((f, i) => {
      const li = document.createElement('li');
      const name = document.createElement('span');
      name.textContent = f.name;
      const btn = document.createElement('button');
      btn.textContent = '✕';
      btn.title = 'Remove';
      btn.onclick = () => { selectedFiles.splice(i, 1); refreshFileList(); };
      li.appendChild(name);
      li.appendChild(btn);
      fileList.appendChild(li);
    });
    buildBtn.disabled = selectedFiles.length === 0;
  }

  dropzone.addEventListener('click', () => fileInput.click());
  dropzone.addEventListener('dragover', e => { e.preventDefault(); dropzone.classList.add('over'); });
  dropzone.addEventListener('dragleave', () => dropzone.classList.remove('over'));
  dropzone.addEventListener('drop', e => {
    e.preventDefault();
    dropzone.classList.remove('over');
    addFiles(Array.from(e.dataTransfer.files));
  });
  fileInput.addEventListener('change', () => {
    addFiles(Array.from(fileInput.files));
    fileInput.value = '';
  });

  function addFiles(files) {
    files.forEach(f => {
      if (!selectedFiles.find(x => x.name === f.name)) selectedFiles.push(f);
    });
    refreshFileList();
  }

  function appendLog(text, cls) {
    const span = document.createElement('span');
    if (cls) span.className = cls;
    // colorize arrows/ticks/errors inline
    if (!cls) {
      if (text.startsWith('→') || text.startsWith('-')) span.className = 'log-arrow';
      else if (text.startsWith('✓'))                     span.className = 'log-ok';
      else if (text.toLowerCase().includes('error'))     span.className = 'log-error';
    }
    span.textContent = text + '\n';
    logEl.appendChild(span);
    logEl.scrollTop = logEl.scrollHeight;
  }

  buildBtn.addEventListener('click', async () => {
    if (selectedFiles.length === 0) return;

    buildBtn.disabled = true;
    dlArea.style.display = 'none';
    statusEl.className = 'running';
    statusEl.textContent = 'building…';
    logEl.innerHTML = '';

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
      if (!res.ok) { appendLog('server error: ' + await res.text(), 'log-error'); buildBtn.disabled = false; return; }
      const data = await res.json();
      buildId = data.id;
    } catch(e) {
      appendLog('network error: ' + e.message, 'log-error');
      buildBtn.disabled = false;
      return;
    }

    const es = new EventSource('/events?id=' + buildId);
    es.onmessage = e => appendLog(e.data);
    es.addEventListener('done', e => {
      es.close();
      if (e.data === 'success') {
        statusEl.className = 'success';
        statusEl.textContent = 'firmware ready!';
        dlBtn.href = '/download?id=' + buildId;
        dlArea.style.display = 'block';
      } else {
        statusEl.className = 'error';
        statusEl.textContent = 'build failed — see log above';
      }
      buildBtn.disabled = false;
    });
    es.onerror = () => {
      appendLog('connection lost', 'log-error');
      es.close();
      buildBtn.disabled = false;
    };
  });
</script>
</body>
</html>
`
