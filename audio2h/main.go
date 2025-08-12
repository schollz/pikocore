package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"strings"

	log "github.com/schollz/logger"
	"github.com/schollz/sox"
	"github.com/youpy/go-wav"
)

var flagFileList string
var flagFolder string
var flagFolderOut string
var flagLimit int
var flagBPM float64
var flagSR float64
var fileOrdering []string
var flagIgnoreFileList bool

func init() {
	flag.StringVar(&flagFileList, "list", "", "list of files to use")
	flag.StringVar(&flagFolder, "folder-in", "flacs", "folder for finding audio")
	flag.StringVar(&flagFolderOut, "folder-out", "converted", "folder for placing converted files")
	flag.IntVar(&flagLimit, "limit", 100, "limit number of samples")
	flag.Float64Var(&flagBPM, "bpm", 165, "bpm to set to")
	flag.Float64Var(&flagSR, "sr", 33000, "sample rate to set to")
	flag.BoolVar(&flagIgnoreFileList, "ignore-filelist", false, "ignore the file list")

	if !flagIgnoreFileList {
		b, err := os.ReadFile("jsons/filelist2.txt")
		if err == nil {
			fileOrdering = strings.Fields(string(b))
		}
	}
}

func main() {
	flag.Parse()
	log.SetLevel("trace")
	files, err := getFiles(flagFolder)
	if err != nil {
		return
	}
	err = convertFiles(files)
	if err != nil {
		return
	}
	err = audio2h(files)
	if err != nil {
		return
	}
}

func audio2h(files []File) (err error) {
	// rand.Seed(time.Now().UnixNano())
	// rand.Shuffle(len(files), func(i, j int) { files[i], files[j] = files[j], files[i] })

	// for i := range files {
	// 	for j, f := range fileOrdering {
	// 		if flagIgnoreFileList {
	// 			files[i].Order = i
	// 		} else {
	// 			if strings.Contains(filepath.Base(files[i].Converted), f) {
	// 				files[i].Order = j
	// 			}
	// 		}
	// 	}
	// }

	// sort.Slice(files, func(i, j int) bool {
	// 	return files[i].Order < files[j].Order
	// })

	bb, err := json.Marshal(files)
	if err != nil {
		log.Error(err)
	}
	fb, err := os.Create("files.json")
	if err != nil {
		log.Error(err)
	}
	fb.Write(bb)
	fb.Close()

	var sb strings.Builder
	var sbd strings.Builder
	limit := len(files)
	if limit > flagLimit {
		limit = flagLimit
	}
	sb.WriteString("#include <pico/platform.h>\n\n")
	sb.WriteString(fmt.Sprintf("#define SAMPLE_RATE %d\n", int(flagSR)))
	sb.WriteString(fmt.Sprintf("#define BPM_SAMPLED %d\n", int(flagBPM)))
	sb.WriteString(fmt.Sprintf("#define NUM_SAMPLES %d\n", limit))
	samplesPerBeat := math.Round(60 / flagBPM * flagSR / 2)
	sb.WriteString(fmt.Sprintf("#define SAMPLES_PER_BEAT %d\n", int(samplesPerBeat)))
	retrigMults := []float64{4, 3.66666666, 3, 2.666666, 2.5, 2, 1.5, 1.333333333, 1, 0.75, 0.666666666, 0.5, 0.5 * 0.75, 0.333333, 0.25, 0.25 * 0.75, 0.125, 0.125 * 0.75, 0.0625}
	retrigs := make([]string, len(retrigMults))
	for i, v := range retrigMults {
		retrigs[i] = fmt.Sprintf("%d", int(math.Round(samplesPerBeat*v)))
	}
	sb.WriteString(fmt.Sprintf("#define NUM_RETRIGS %d\n", len(retrigs)))
	sb.WriteString("const uint16_t retrigs[] = { " + strings.Join(retrigs, ", ") + " };")

	sampleStart := 0
	silentBytes := 65536 * 2
	sb.WriteString("\n\n// filename: dummy\n")
	sb.WriteString(fmt.Sprintf("#define RAW_DUMMY_BEATS 1\n"))
	sb.WriteString(fmt.Sprintf("#define RAW_DUMMY_SAMPLES %d\n", silentBytes))
	sb.WriteString(fmt.Sprintf("#define RAW_DUMMY_START %d\n", sampleStart))
	sampleStart += silentBytes
	sbd.WriteString(fmt.Sprintf("const uint8_t __in_flash() raw_audio[] = {\n"))
	intsDummy := make([]int, silentBytes)
	for i, _ := range intsDummy {
		intsDummy[i] = 128
	}
	sbd.WriteString(printInts(intsDummy))
	for i, f := range files {
		var ints []int
		ints, err = convertWavToInts(f.Converted)
		if err != nil {
			log.Error(err)
			return
		}
		log.Tracef("[%3d] %s: %d", f.Order, f.Converted, len(ints))
		sb.WriteString("\n\n// filename: " + filepath.Base(f.Pathname) + "\n")
		sb.WriteString(fmt.Sprintf("#define RAW_%d_BEATS %d\n", i, int(f.Beats)*2))
		sb.WriteString(fmt.Sprintf("#define RAW_%d_SAMPLES %d\n", i, len(ints)))
		sb.WriteString(fmt.Sprintf("#define RAW_%d_START %d\n", i, sampleStart))
		sampleStart += len(ints)
		sbd.WriteString("\n,\n")
		sbd.WriteString(printInts(ints))

		if i == limit {
			break
		}
	}
	sbd.WriteString("};\n\n")
	sb.WriteString("\n\n")
	sb.WriteString(sbd.String())

	sb.WriteString("char raw_val(int s, int i) {\n")
	for i := range files {
		sb.WriteString(fmt.Sprintf("\tif (s==%d) return raw_audio[i+RAW_%d_START];\n", i, i))
		if i == limit {
			break
		}
	}
	sb.WriteString("return raw_audio[i];\n}\n\n")

	sb.WriteString("unsigned int raw_len(int s) {\n")
	for i := range files {
		sb.WriteString(fmt.Sprintf("\tif (s==%d) return RAW_%d_SAMPLES;\n", i, i))
		if i == limit {
			break
		}
	}
	sb.WriteString("return RAW_0_SAMPLES;\n}\n\n")

	sb.WriteString("unsigned int raw_beats(int s) {\n")
	for i := range files {
		sb.WriteString(fmt.Sprintf("\tif (s==%d) return RAW_%d_BEATS;\n", i, i))
		if i == limit {
			break
		}
	}
	sb.WriteString("return 1;\n}\n\n")

	f, err := os.Create("../doth/audio2h.h")
	f.WriteString(sb.String())
	f.Close()
	return
}

func printInts(ints []int) (s string) {
	var sb strings.Builder
	sb.WriteString("\t")
	for i, v := range ints {
		sb.WriteString(fmt.Sprintf("0x%02x", v))
		if i < len(ints)-1 {
			sb.WriteString(", ")
		}
		if i > 0 && i%20 == 0 {
			sb.WriteString("\n\t")
		}

	}
	s = sb.String()
	return

}

type File struct {
	Pathname  string
	Converted string
	Beats     float64
	BPM       float64
	Order     int
	Seconds   float64
}

func getFiles(folderName string) (files []File, err error) {
	fnames := []string{}
	if flagFileList != "" {
		b, _ := os.ReadFile(flagFileList)
		for _, line := range strings.Split(string(b), "\n") {
			f := strings.TrimSpace(line)
			if f != "" {
				fnames = append(fnames, f)
			}
		}
	} else {
		err = filepath.Walk(folderName,
			func(pathname string, info os.FileInfo, err error) error {
				if err != nil {
					return err
				}
				if info.IsDir() {
					return nil
				}
				ext := filepath.Ext(pathname)
				ext = strings.ToLower(ext)
				if ext == ".flac" || ext == ".wav" || ext == ".mp3" || ext == ".aif" || ext == ".ogg" {
					fnames = append(fnames, pathname)
				}
				return nil
			})
	}
	if err != nil {
		return
	}
	// rand.Seed(time.Now().UnixNano())
	// rand.Shuffle(len(fnames), func(i, j int) { fnames[i], fnames[j] = fnames[j], fnames[i] })

	log.Infof("found %d files", len(fnames))
	log.Debugf("folder out: %s", flagFolderOut)

	files = make([]File, len(fnames))
	i := 0
	for _, fname := range fnames {
		files[i].Pathname = fname
		files[i].Converted = path.Join(flagFolderOut, filepath.Base(fname)+".wav")
		files[i].Beats, files[i].BPM, err = sox.GetBPM(fname)
		if err != nil {
			log.Error(err)
		}
		files[i].Seconds, err = sox.Length(fname)
		if err != nil {
			log.Error(err)
		}
		log.Tracef("0: %+v", files[i])
		i++
		if i == flagLimit {
			break
		}
	}
	files = files[:i]
	return
}

func convertFiles(files []File) (err error) {
	os.RemoveAll(flagFolderOut)
	os.MkdirAll(flagFolderOut, os.ModePerm)
	for _, f := range files {
		lpf := int(flagSR*7/16) - 5
		if lpf > 19000 {
			lpf = 19000
		}
		log.Tracef("%s", strings.Join([]string{"sox", f.Pathname, "-r", fmt.Sprint(int(flagSR)), "-c", "1", "-b", "8", f.Converted, "speed", fmt.Sprintf("%2.6f", flagBPM/f.BPM), "lowpass", fmt.Sprint(lpf), "norm", "gain", "-6"}, " "))
		cmd := exec.Command("sox", f.Pathname, "-r", fmt.Sprint(int(flagSR)), "-c", "1", "-b", "8", f.Converted, "speed", fmt.Sprintf("%2.6f", flagBPM/f.BPM), "highpass", "5", "lowpass", fmt.Sprint(lpf), "gain", "-6", "norm", "-3", "dither")
		stdoutStderr, err := cmd.CombinedOutput()
		if err != nil {
			log.Errorf("cmd failed: \n%s", stdoutStderr)
		}
	}
	err = nil
	return
}

func ex(c string) (err error) {
	log.Trace(c)
	cs := strings.Fields(c)
	cmd := exec.Command(cs[0], cs[1:]...)
	stdoutStderr, err := cmd.CombinedOutput()
	if err != nil {
		log.Errorf("cmd failed: %s\n%s", c, stdoutStderr)
	}
	return
}

func convertWavToInts(fname string) (vals []int, err error) {
	file, err := os.Open(fname)
	if err != nil {
		return
	}
	reader := wav.NewReader(file)
	n := 0
	vals = make([]int, 10000000)
	for {
		samples, err := reader.ReadSamples()

		for _, sample := range samples {
			v := reader.IntValue(sample, 0)
			vals[n] = v
			n++
		}

		if err == io.EOF {
			break
		}
	}
	err = file.Close()

	vals = vals[:n]
	return
}
