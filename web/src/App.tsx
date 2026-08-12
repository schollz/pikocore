import {
  ArrowDown,
  ArrowUp,
  Cable,
  Download,
  Eraser,
  FolderOpen,
  HardDrive,
  Pause,
  Play,
  Plus,
  HelpCircle,
  Terminal,
  Trash2,
  Upload,
} from 'lucide-react';
import { driver, type Driver } from 'driver.js';
import 'driver.js/dist/driver.css';
import { Estimation } from 'arrival-time';
import { useEffect, useMemo, useRef, useState } from 'react';
import { decodeAndEncodeFile, makePreviewBuffer } from './audio';
import {
  BANK_MAX_SAMPLES,
  BANK_SAMPLE_RATE,
  BankSample,
  buildBankBlob,
  croppedPcm,
  parseBankBlob,
  usedAudioBytes,
} from './bank';
import {
  ClockDiagnostics,
  DeviceInfo,
  PikocoreSerial,
  hasClockSync,
  isCompatibleFirmware,
  shouldPollClockDiagnostics,
} from './serial';
import ittybittymidiConnection from './assets/ittybittymidi_connection.jpg';
import pikocoreInstructions from './assets/pikocore_instructions.png';

const serial = new PikocoreSerial();
const requiredFirmwareLabel = '2.2 or newer';
const firmwareOptions = [
  {
    file: 'pikocore-16mb.uf2',
    name: 'pikocore-16mb.uf2',
    label: '16 MB pikocore',
    title: 'Download the default 16 MB pikocore UF2 firmware',
    default: true,
  },
  {
    file: 'pikocore-4mb.uf2',
    name: 'pikocore-4mb.uf2',
    label: '4 MB',
    title: 'Download the 4 MB UF2 firmware',
    default: false,
  },
  {
    file: 'pikocore-2mb.uf2',
    name: 'pikocore-2mb.uf2',
    label: '2 MB',
    title: 'Download the 2 MB UF2 firmware',
    default: false,
  },
] as const;

type StatusKind = 'idle' | 'good' | 'warn' | 'bad';
type Theme = 'light' | 'dark';

interface Status {
  text: string;
  kind: StatusKind;
}

interface TransferDetail {
  transferredBytes: number;
  totalBytes: number | null;
  etaMs: number | null;
}

export function App() {
  const [samples, setSamples] = useState<BankSample[]>([]);
  const [device, setDevice] = useState<DeviceInfo | null>(null);
  const [connected, setConnected] = useState(false);
  const [incompatibleDevice, setIncompatibleDevice] = useState<DeviceInfo | null>(null);
  const [bankDirty, setBankDirty] = useState(false);
  const [status, setStatus] = useState<Status>({ text: 'Disconnected', kind: 'idle' });
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState(0);
  const [uploadDetail, setUploadDetail] = useState<TransferDetail | null>(null);
  const [downloadDetail, setDownloadDetail] = useState<TransferDetail | null>(null);
  const [firmwareDownloadDetail, setFirmwareDownloadDetail] = useState<TransferDetail | null>(null);
  const [playingId, setPlayingId] = useState<string | null>(null);
  const [playheadFrame, setPlayheadFrame] = useState(0);
  const [firmwareFile, setFirmwareFile] = useState<(typeof firmwareOptions)[number]['file']>(firmwareOptions[0].file);
  const [debugLog, setDebugLog] = useState<string[]>([]);
  const [debugOpen, setDebugOpen] = useState(false);
  const [ittybittymidiInfoOpen, setIttybittymidiInfoOpen] = useState(false);
  const [clockDiagnostics, setClockDiagnostics] = useState<ClockDiagnostics | null>(null);
  const [theme] = useState<Theme>(() => loadTheme());
  const debugOpenRef = useRef(false);
  const debugEntriesRef = useRef<string[]>([]);
  const debugFlushTimerRef = useRef<number | null>(null);
  const debugInteractionRef = useRef(false);
  const busyRef = useRef(false);
  const diagnosticsRequestRef = useRef(false);
  const audioRef = useRef<{
    context: AudioContext;
    source: AudioBufferSourceNode;
    startedAt: number;
    frameCount: number;
  } | null>(null);

  useEffect(() => {
    window.localStorage.setItem('pikocore-theme', theme);
  }, [theme]);

  useEffect(() => {
    busyRef.current = busy;
  }, [busy]);

  useEffect(() => {
    if (!connected || !hasClockSync(device)) {
      setClockDiagnostics(null);
      return;
    }
    let cancelled = false;
    const poll = async () => {
      if (!shouldPollClockDiagnostics(connected, busyRef.current, device, diagnosticsRequestRef.current)) return;
      diagnosticsRequestRef.current = true;
      try {
        const next = await serial.clockDiagnostics(true);
        if (!cancelled) setClockDiagnostics(next);
      } catch (_) {
        // A foreground command or disconnect owns status reporting. Polling is
        // intentionally silent and resumes after serial becomes idle.
      } finally {
        diagnosticsRequestRef.current = false;
      }
    };
    void poll();
    const timer = window.setInterval(() => void poll(), 500);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [connected, device]);

  useEffect(() => {
    debugOpenRef.current = debugOpen;
    if (debugOpen) flushDebugLog(true);
  }, [debugOpen]);

  useEffect(() => {
    const closeSerial = () => {
      void serial.disconnect();
    };
    window.addEventListener('pagehide', closeSerial);
    window.addEventListener('beforeunload', closeSerial);
    return () => {
      window.removeEventListener('pagehide', closeSerial);
      window.removeEventListener('beforeunload', closeSerial);
      void serial.disconnect();
    };
  }, []);

  useEffect(() => {
    if (!playingId || !audioRef.current) {
      setPlayheadFrame(0);
      return;
    }

    let raf = 0;
    const tick = () => {
      const active = audioRef.current;
      if (!active) return;
      const elapsed = active.context.currentTime - active.startedAt;
      const frame = Math.floor((elapsed * BANK_SAMPLE_RATE) % Math.max(1, active.frameCount));
      setPlayheadFrame(frame);
      raf = window.requestAnimationFrame(tick);
    };
    raf = window.requestAnimationFrame(tick);
    return () => window.cancelAnimationFrame(raf);
  }, [playingId]);

  const capacity = device?.capacityBytes ?? null;
  const used = useMemo(() => usedAudioBytes(samples), [samples]);
  const overCapacity = capacity != null && used > capacity;
  const usageRatio = capacity != null && capacity > 0 ? Math.min(1, used / capacity) : 0;
  const showStatusSpinner = busy && status.kind === 'idle';
  const bankEditingDisabled = !connected || busy || incompatibleDevice != null;
  const uploadNeedsSync = connected && incompatibleDevice == null && bankDirty;
  const uploadDisabled = !connected || incompatibleDevice != null || busy || overCapacity || (!uploadNeedsSync && samples.length === 0);
  const selectedFirmware = firmwareOptions.find((option) => option.file === firmwareFile) ?? firmwareOptions[0];
  const firmwareDownloadUrl = `${import.meta.env.BASE_URL}${selectedFirmware.file}`;
  const uploadTransferText =
    busy && status.text === 'Uploading' && uploadDetail
      ? formatTransferDetail(uploadDetail)
      : null;
  const downloadTransferText =
    busy && status.text === 'Downloading' && downloadDetail
      ? formatTransferDetail(downloadDetail)
      : null;
  const firmwareDownloadTransferText =
    busy && status.text === 'Downloading UF2' && firmwareDownloadDetail
      ? formatTransferDetail(firmwareDownloadDetail)
      : null;
  const transferText = uploadTransferText ?? downloadTransferText ?? firmwareDownloadTransferText;

  async function beginBusyOperation() {
    busyRef.current = true;
    setBusy(true);
    while (diagnosticsRequestRef.current) {
      await new Promise((resolve) => window.setTimeout(resolve, 10));
    }
  }

  function endBusyOperation() {
    busyRef.current = false;
    setBusy(false);
  }

  async function connect() {
    if (connected) {
      await beginBusyOperation();
      try {
        stopPreview();
        await serial.disconnect();
        setConnected(false);
        setDevice(null);
        setClockDiagnostics(null);
        setIncompatibleDevice(null);
        setBankDirty(false);
        setStatus({ text: 'Disconnected', kind: 'idle' });
      } finally {
        endBusyOperation();
      }
      return;
    }

    try {
      await beginBusyOperation();
      clearDebugLog();
      setIncompatibleDevice(null);
      serial.setLogger(appendDebugLog);
      setStatus({ text: 'Connecting', kind: 'idle' });
      await serial.connect();
      const info = await initialiseWithRetry();
      if (!isCompatibleFirmware(info)) {
        stopPreview();
        setDevice(null);
        setConnected(false);
        setIncompatibleDevice(info);
        setBankDirty(false);
        setProgress(0);
        setStatus({ text: 'Firmware update required', kind: 'warn' });
        await serial.disconnect();
        serial.setLogger(null);
        return;
      }
      setDevice(info);
      setConnected(true);
      setStatus({ text: 'Downloading', kind: 'idle' });
      const bank = await readBankWithProgress();
      if (bank) {
        const parsed = parseBankBlob(bank);
        setSamples(parsed.samples);
        setBankDirty(false);
        setStatus({ text: `Loaded ${parsed.samples.length} samples from device`, kind: 'good' });
      } else {
        setSamples([]);
        setBankDirty(false);
        setStatus({ text: 'Connected: device bank is empty', kind: 'good' });
      }
      setProgress(0);
      setDownloadDetail(null);
    } catch (error) {
      setStatus({ text: errorMessage(error), kind: 'bad' });
      try {
        await serial.disconnect();
      } catch (_) {}
      serial.setLogger(null);
      setConnected(false);
      setBankDirty(false);
    } finally {
      endBusyOperation();
    }
  }

  async function enterBootloaderMode() {
    const wasConnected = connected;
    try {
      await beginBusyOperation();
      stopPreview();
      serial.setLogger(appendDebugLog);
      setStatus({ text: 'Entering BOOTSEL mode', kind: 'idle' });
      if (serial.connected()) {
        await serial.bootloader();
      } else {
        await serial.resetToBootloader();
      }
      setConnected(false);
      setDevice(null);
      setIncompatibleDevice(null);
      setBankDirty(false);
      setProgress(0);
      setUploadDetail(null);
      setDownloadDetail(null);
      setFirmwareDownloadDetail(null);
      setStatus({ text: `BOOTSEL mode: copy ${selectedFirmware.name} to the new RPI-RP2 drive`, kind: 'good' });
    } catch (error) {
      if (wasConnected) {
        try {
          await serial.disconnect();
        } catch (_) {}
        setConnected(false);
        setDevice(null);
        setBankDirty(false);
      }
      const message = errorMessage(error);
      setStatus({
        text:
          wasConnected && /Timed out waiting for serial line|Bootloader reset rejected/.test(message)
            ? 'Connected firmware does not support web BOOTSEL yet; use the BOOTSEL button once to install updated firmware'
            : message,
        kind: 'bad',
      });
    } finally {
      endBusyOperation();
    }
  }

  async function downloadFirmware() {
    try {
      await beginBusyOperation();
      setProgress(0);
      setUploadDetail(null);
      setDownloadDetail(null);
      setFirmwareDownloadDetail({ transferredBytes: 0, totalBytes: null, etaMs: null });
      setStatus({ text: 'Downloading UF2', kind: 'idle' });

      const response = await fetch(firmwareDownloadUrl);
      if (!response.ok) throw new Error(`Firmware download failed: ${response.status} ${response.statusText}`);

      const totalBytes = parseContentLength(response.headers.get('content-length'));
      if (!response.body) {
        const blob = await response.blob();
        setProgress(1);
        setFirmwareDownloadDetail({ transferredBytes: blob.size, totalBytes: blob.size || totalBytes, etaMs: null });
        saveBlob(blob, selectedFirmware.name);
        setStatus({ text: `Downloaded ${selectedFirmware.name}`, kind: 'good' });
        return;
      }

      const reader = response.body.getReader();
      const chunks: BlobPart[] = [];
      const eta = totalBytes != null ? new Estimation({ progress: 0, total: totalBytes }) : null;
      let receivedBytes = 0;
      let lastEtaUpdate = 0;
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value) continue;
        const chunk = new ArrayBuffer(value.byteLength);
        new Uint8Array(chunk).set(value);
        chunks.push(chunk);
        receivedBytes += value.length;
        const ratio = totalBytes != null && totalBytes > 0 ? Math.min(1, receivedBytes / totalBytes) : 0;
        const now = performance.now();
        const estimate =
          eta && totalBytes != null && now - lastEtaUpdate >= 500 && ratio < 1 ? eta.update(receivedBytes, totalBytes).estimate : null;
        if (estimate != null) lastEtaUpdate = now;
        setProgress(ratio);
        setFirmwareDownloadDetail({
          transferredBytes: receivedBytes,
          totalBytes,
          etaMs: estimate != null && Number.isFinite(estimate) && estimate > 0 ? estimate : null,
        });
      }

      const blob = new Blob(chunks, { type: 'application/octet-stream' });
      saveBlob(blob, selectedFirmware.name);
      setProgress(1);
      setFirmwareDownloadDetail({ transferredBytes: receivedBytes, totalBytes: totalBytes ?? receivedBytes, etaMs: null });
      setStatus({ text: `Downloaded ${selectedFirmware.name}`, kind: 'good' });
    } catch (error) {
      setStatus({ text: errorMessage(error), kind: 'bad' });
    } finally {
      setProgress(0);
      setFirmwareDownloadDetail(null);
      setDownloadDetail(null);
      endBusyOperation();
    }
  }

  async function initialiseWithRetry(): Promise<DeviceInfo> {
    let lastError: unknown = null;
    for (let attempt = 0; attempt < 4; attempt++) {
      try {
        setStatus({ text: attempt === 0 ? 'Connecting' : 'Retrying connection', kind: 'idle' });
        await serial.sync();
        return await serial.info(true);
      } catch (error) {
        lastError = error;
        await new Promise((resolve) => setTimeout(resolve, 250 * (attempt + 1)));
      }
    }
    throw lastError instanceof Error ? lastError : new Error(String(lastError));
  }

  async function readBankWithProgress() {
    setProgress(0);
    setDownloadDetail(null);
    let eta: Estimation | null = null;
    let lastEtaUpdate = 0;
    return serial.readBank((ratio, transferredBytes, totalBytes) => {
      setProgress(ratio);
      if (!eta) {
        eta = new Estimation({ progress: 0, total: totalBytes });
        setDownloadDetail({ transferredBytes: 0, totalBytes, etaMs: null });
      }
      const now = performance.now();
      if (now - lastEtaUpdate < 500 && ratio < 1) {
        setDownloadDetail((current) =>
          current ? { ...current, transferredBytes, totalBytes } : { transferredBytes, totalBytes, etaMs: null },
        );
        return;
      }
      lastEtaUpdate = now;
      const estimate = eta.update(transferredBytes, totalBytes).estimate;
      setDownloadDetail({
        transferredBytes,
        totalBytes,
        etaMs: Number.isFinite(estimate) && estimate > 0 && ratio < 1 ? estimate : null,
      });
    });
  }

  function clearDebugLog() {
    debugEntriesRef.current = [];
    setDebugLog([]);
    if (debugFlushTimerRef.current != null) {
      window.clearTimeout(debugFlushTimerRef.current);
      debugFlushTimerRef.current = null;
    }
  }

  function appendDebugLog(message: string) {
    debugEntriesRef.current.push(`${new Date().toLocaleTimeString()} ${message}`);
    if (debugEntriesRef.current.length > 80) debugEntriesRef.current.splice(0, debugEntriesRef.current.length - 80);
    if (!debugOpenRef.current || debugInteractionRef.current || debugFlushTimerRef.current != null) return;
    debugFlushTimerRef.current = window.setTimeout(() => {
      debugFlushTimerRef.current = null;
      flushDebugLog();
    }, 250);
  }

  function flushDebugLog(force = false) {
    if (debugFlushTimerRef.current != null) {
      window.clearTimeout(debugFlushTimerRef.current);
      debugFlushTimerRef.current = null;
    }
    if (!force && debugInteractionRef.current) return;
    setDebugLog([...debugEntriesRef.current]);
  }

  function toggleDebug() {
    const next = !debugOpenRef.current;
    debugOpenRef.current = next;
    setDebugOpen(next);
    if (next) flushDebugLog(true);
  }

  function pauseDebugLog() {
    debugInteractionRef.current = true;
    if (debugFlushTimerRef.current != null) {
      window.clearTimeout(debugFlushTimerRef.current);
      debugFlushTimerRef.current = null;
    }
  }

  function resumeDebugLog() {
    debugInteractionRef.current = false;
    flushDebugLog(true);
  }

  async function addFiles(fileList: FileList | File[]) {
    if (!connected) {
      setStatus({
        text: incompatibleDevice ? 'Firmware update required before adding audio' : 'Connect to a pikocore before adding audio',
        kind: 'warn',
      });
      return;
    }
    const files = Array.from(fileList).filter((file) => file.type.startsWith('audio/') || /\.(aif|aiff|wav|mp3|flac|ogg)$/i.test(file.name));
    if (files.length === 0) return;
    const availableSlots = Math.max(0, BANK_MAX_SAMPLES - samples.length);
    if (availableSlots === 0) {
      setStatus({ text: `pikocore supports up to ${BANK_MAX_SAMPLES} samples`, kind: 'warn' });
      return;
    }
    const filesToProcess = files.slice(0, availableSlots);
    const skipped = files.length - filesToProcess.length;
    await beginBusyOperation();
    try {
      const next: BankSample[] = [];
      for (const file of filesToProcess) {
        setStatus({ text: `Processing ${file.name}`, kind: 'idle' });
        next.push(await decodeAndEncodeFile(file));
      }
      setSamples((current) => [...current, ...next].slice(0, BANK_MAX_SAMPLES));
      setBankDirty(true);
      setStatus({
        text:
          skipped > 0
            ? `Added ${next.length} sample${next.length === 1 ? '' : 's'}; ignored ${skipped} over the ${BANK_MAX_SAMPLES}-sample limit`
            : `Added ${next.length} sample${next.length === 1 ? '' : 's'}`,
        kind: skipped > 0 ? 'warn' : 'good',
      });
    } catch (error) {
      setStatus({ text: errorMessage(error), kind: 'bad' });
    } finally {
      endBusyOperation();
    }
  }

  async function uploadBank() {
    if (!connected || incompatibleDevice) {
      setStatus({ text: 'Connect to a pikocore before uploading', kind: 'warn' });
      return;
    }
    try {
      if (!device) throw new Error('Connect to a pikocore before uploading');
      await beginBusyOperation();
      stopPreview();
      setStatus({ text: 'Uploading', kind: 'idle' });
      const blob = buildBankBlob(samples, device.capacityBytes);
      const eta = new Estimation({ progress: 0, total: blob.length });
      let lastEtaUpdate = 0;
      setProgress(0);
      setDownloadDetail(null);
      setUploadDetail({ transferredBytes: 0, totalBytes: blob.length, etaMs: null });
      await serial.writeBank(blob, (ratio, transferredBytes, totalBytes) => {
        setProgress(ratio);
        const now = performance.now();
        if (now - lastEtaUpdate < 500 && ratio < 1) return;
        lastEtaUpdate = now;
        const estimate = eta.update(transferredBytes, totalBytes).estimate;
        setUploadDetail({
          transferredBytes,
          totalBytes,
          etaMs: Number.isFinite(estimate) && estimate > 0 && ratio < 1 ? estimate : null,
        });
      });
      const info = await serial.info();
      setDevice(info);
      setBankDirty(false);
      setProgress(0);
      setUploadDetail(null);
      setStatus({ text: 'Upload complete', kind: 'good' });
    } catch (error) {
      setStatus({ text: errorMessage(error), kind: 'bad' });
    } finally {
      setProgress(0);
      setUploadDetail(null);
      setDownloadDetail(null);
      endBusyOperation();
    }
  }

  async function readDevice() {
    if (!connected || incompatibleDevice) return;
    try {
      await beginBusyOperation();
      stopPreview();
      setStatus({ text: 'Downloading', kind: 'idle' });
      const bank = await readBankWithProgress();
      const info = await serial.info();
      setDevice(info);
      if (bank) {
        const parsed = parseBankBlob(bank);
        setSamples(parsed.samples);
        setBankDirty(false);
        setStatus({ text: `Read ${parsed.samples.length} samples`, kind: 'good' });
      } else {
        setSamples([]);
        setBankDirty(false);
        setStatus({ text: 'Device bank is empty', kind: 'good' });
      }
      setProgress(0);
      setDownloadDetail(null);
    } catch (error) {
      setStatus({ text: errorMessage(error), kind: 'bad' });
    } finally {
      setProgress(0);
      setDownloadDetail(null);
      endBusyOperation();
    }
  }

  async function eraseDevice() {
    if (!connected || incompatibleDevice) return;
    try {
      await beginBusyOperation();
      stopPreview();
      setStatus({ text: 'Erasing', kind: 'idle' });
      await serial.erase();
      const info = await serial.info();
      setDevice(info);
      setSamples([]);
      setBankDirty(false);
      setStatus({ text: 'Device bank erased', kind: 'good' });
    } catch (error) {
      setStatus({ text: errorMessage(error), kind: 'bad' });
    } finally {
      endBusyOperation();
    }
  }

  async function setIttybittymidiMode(enabled: boolean) {
    if (!connected || !device) return;
    try {
      await beginBusyOperation();
      setStatus({ text: 'Saving clock input mode', kind: 'idle' });
      await serial.setClockInputMode(enabled);
      const info = await serial.info();
      setDevice(info);
      setStatus({
        text: enabled ? 'Clock input set to ittybittymidi' : 'Clock input set to pulses',
        kind: 'good',
      });
    } catch (error) {
      setStatus({ text: errorMessage(error), kind: 'bad' });
    } finally {
      endBusyOperation();
    }
  }

  async function setPulsePpqn(ppqn: 1 | 2 | 4) {
    if (!connected || !device || !hasClockSync(device)) return;
    try {
      await beginBusyOperation();
      setStatus({ text: 'Saving pulse division', kind: 'idle' });
      await serial.setPulsePpqn(ppqn);
      const info = await serial.info();
      setDevice(info);
      setStatus({ text: `Pulse clock set to ${pulseDivisionLabel(ppqn)}`, kind: 'good' });
    } catch (error) {
      setStatus({ text: errorMessage(error), kind: 'bad' });
    } finally {
      endBusyOperation();
    }
  }

  function updateSample(id: string, patch: Partial<BankSample>) {
    setSamples((current) => current.map((sample) => (sample.id === id ? { ...sample, ...patch } : sample)));
    if (connected) setBankDirty(true);
  }

  function removeSample(id: string) {
    if (playingId === id) stopPreview();
    setSamples((current) => current.filter((sample) => sample.id !== id));
    if (connected) setBankDirty(true);
  }

  function moveSample(index: number, direction: -1 | 1) {
    const target = index + direction;
    const changed = target >= 0 && target < samples.length;
    setSamples((current) => {
      const next = [...current];
      if (target < 0 || target >= next.length) return current;
      [next[index], next[target]] = [next[target], next[index]];
      return next;
    });
    if (connected && changed) setBankDirty(true);
  }

  function stopPreview() {
    audioRef.current?.source.stop();
    audioRef.current?.context.close();
    audioRef.current = null;
    setPlayingId(null);
  }

  async function togglePreview(sample: BankSample) {
    if (playingId === sample.id) {
      stopPreview();
      return;
    }
    stopPreview();
    const context = new AudioContext();
    const pcm = croppedPcm(sample);
    const source = context.createBufferSource();
    source.buffer = makePreviewBuffer(context, pcm);
    source.loop = true;
    source.connect(context.destination);
    const startedAt = context.currentTime;
    source.start();
    source.onended = () => {
      if (audioRef.current?.source === source) setPlayingId(null);
    };
    audioRef.current = { context, source, startedAt, frameCount: pcm.length };
    setPlayingId(sample.id);
  }

  function startTour() {
    let tour: Driver;
    tour = driver({
      animate: true,
      smoothScroll: true,
      overlayOpacity: 0.62,
      stagePadding: 6,
      stageRadius: 7,
      popoverClass: `pikocore-tour pikocore-tour-${theme}`,
      showProgress: true,
      progressText: '{{current}} / {{total}}',
      nextBtnText: 'Next',
      prevBtnText: 'Back',
      doneBtnText: 'Done',
      showButtons: ['previous', 'next', 'close'],
      overlayClickBehavior: () => undefined,
      steps: [
        {
          popover: {
            title: 'Welcome to pikocore loader',
            description: `<img class="tour-pikocore" src="${pikocoreInstructions}" alt="pikocore front panel" /><p>Use this page to update firmware and manage the samples on your pikocore.</p>`,
            side: 'over',
          },
        },
        {
          element: '[data-tour="uf2"]',
          popover: {
            title: 'Download firmware',
            description:
              "Download the latest v2 UF2 firmware if you haven't already. Use Boot to put pikocore in drive mode, then copy the UF2 to it.",
            side: 'bottom',
            align: 'center',
          },
        },
        {
          element: '[data-tour="connect"]',
          popover: {
            title: 'Connect',
            description: 'Connect your pikocore over USB serial so the loader can read the current sample bank.',
            side: 'bottom',
            align: 'center',
          },
        },
        {
          element: '[data-tour="add"]',
          popover: {
            title: 'Add samples',
            description: 'After connecting, add audio files or drag them into the sample area. The loader converts them for pikocore.',
            side: 'bottom',
            align: 'center',
          },
        },
        {
          element: '[data-tour="upload"]',
          popover: {
            title: 'Upload samples',
            description: 'When your sample list is ready, upload it to write the bank back to your pikocore.',
            side: 'bottom',
            align: 'center',
          },
        },
        {
          element: '[data-tour="read"]',
          popover: {
            title: 'Read from pikocore',
            description: 'Use Read to reload the sample bank from the device if you want to inspect or restore what is currently on your pikocore.',
            side: 'bottom',
            align: 'center',
          },
        },
        {
          element: '[data-tour="erase"]',
          popover: {
            title: 'Erase the bank',
            description: 'Use Erase only when you want to clear the sample bank on the device.',
            side: 'bottom',
            align: 'center',
          },
        },
      ],
      onNextClick: (_, __, { driver: activeTour }) => {
        if (activeTour.isLastStep()) {
          activeTour.destroy();
          return;
        }
        activeTour.moveNext();
      },
      onCloseClick: (_, __, { driver: activeTour }) => {
        activeTour.destroy();
      },
    });
    tour.drive();
  }

  return (
    <main className="app" data-theme={theme}>
      <header className="topbar">
        <div>
          <h1>pikocore loader</h1>
          <div className={`status ${status.kind}`}>
            {showStatusSpinner ? <span className="spinner" aria-hidden="true" /> : null}
            {status.text}
          </div>
        </div>
        <div className="toolbar">
          <button
            className="primary"
            data-tour="connect"
            onClick={connect}
            disabled={busy}
            title={connected ? 'Disconnect from pikocore' : 'Connect to pikocore over USB serial'}
          >
            <Cable size={18} />
            {connected ? 'Disconnect' : 'Connect'}
          </button>
          <label
            className={`button ${bankEditingDisabled ? 'disabled' : ''}`}
            data-tour="add"
            title={connected ? 'Add audio files to the sample list' : 'Connect to a pikocore before adding audio'}
          >
            <Plus size={18} />
            Add
            <input
              type="file"
              multiple
              accept="audio/*,.aif,.aiff"
              disabled={bankEditingDisabled}
              onChange={(event) => {
                if (event.target.files) void addFiles(event.target.files);
                event.currentTarget.value = '';
              }}
            />
          </label>
          <button
            data-tour="upload"
            onClick={uploadBank}
            disabled={uploadDisabled}
            className={uploadNeedsSync ? 'needs-sync' : undefined}
            title={uploadNeedsSync ? 'Upload changes to sync pikocore' : 'Upload the current sample bank to the device'}
          >
            <Upload size={18} />
            Upload
          </button>
          <button
            data-tour="read"
            onClick={readDevice}
            disabled={!connected || incompatibleDevice != null || busy}
            title="Read the sample bank from the device"
          >
            <Download size={18} />
            Read
          </button>
          <button
            data-tour="erase"
            onClick={eraseDevice}
            disabled={!connected || incompatibleDevice != null || busy}
            title="Erase the sample bank from the device"
          >
            <Eraser size={18} />
            Erase
          </button>
          <div className="firmware-download" data-tour="uf2">
            <select
              className={selectedFirmware.default ? 'default-firmware' : undefined}
              value={firmwareFile}
              onChange={(event) => setFirmwareFile(event.currentTarget.value as (typeof firmwareOptions)[number]['file'])}
              title="Choose UF2 firmware flash size"
              aria-label="Choose UF2 firmware flash size"
            >
              {firmwareOptions.map((option) => (
                <option key={option.file} value={option.file}>
                  {option.label}
                </option>
              ))}
            </select>
            <button
              type="button"
              className={selectedFirmware.default ? 'primary' : undefined}
              onClick={downloadFirmware}
              disabled={busy}
              title={selectedFirmware.title}
              aria-label={selectedFirmware.title}
            >
              <Download size={18} />
              UF2
            </button>
            <button
              type="button"
              onClick={enterBootloaderMode}
              disabled={busy}
              title="Put pikocore into BOOTSEL drive mode"
              aria-label="Put pikocore into BOOTSEL drive mode"
            >
              <HardDrive size={18} />
              Boot
            </button>
          </div>
          <button
            className="icon-button"
            onClick={startTour}
            title="Show pikocore tour"
            aria-label="Show pikocore tour"
          >
            <HelpCircle size={18} />
          </button>
          <button
            className="icon-button debug-toggle"
            onClick={toggleDebug}
            title={debugOpen ? 'Hide serial debug messages' : 'Show serial debug messages'}
            aria-label={debugOpen ? 'Hide serial debug messages' : 'Show serial debug messages'}
            aria-pressed={debugOpen}
          >
            <Terminal size={18} />
          </button>
        </div>
        <div className="toolbar-subrow">
          <div
            className="clock-mode"
          >
            <label
              className={!connected || incompatibleDevice != null || busy ? 'disabled' : ''}
              title="Use serial MIDI from ittybittymidi on the clock input"
            >
              <input
                type="checkbox"
                checked={device?.ittybittymidiMode ?? false}
                disabled={!connected || incompatibleDevice != null || busy}
                onChange={(event) => void setIttybittymidiMode(event.currentTarget.checked)}
              />
              Ittybittymidi mode
            </label>
            <button
              type="button"
              className="text-button"
              title="Learn more about ittybittymidi"
              onClick={(event) => {
                event.stopPropagation();
                setIttybittymidiInfoOpen(true);
              }}
            >
              more info
            </button>
            {hasClockSync(device) ? (
              <label className={!connected || incompatibleDevice != null || busy ? 'disabled' : ''}>
                Pulse division
                <select
                  value={device?.pulsePpqn ?? 2}
                  disabled={!connected || incompatibleDevice != null || busy}
                  onChange={(event) => void setPulsePpqn(Number(event.currentTarget.value) as 1 | 2 | 4)}
                  title="Choose the pulse clock division"
                  aria-label="Pulse clock division"
                >
                  <option value={1}>Quarter note (1 PPQN)</option>
                  <option value={2}>Eighth note (2 PPQN)</option>
                  <option value={4}>Sixteenth note (4 PPQN)</option>
                </select>
              </label>
            ) : null}
          </div>
        </div>
      </header>

      {hasClockSync(device) ? (
        <details className="clock-diagnostics">
          <summary>
            Clock diagnostics
            {clockDiagnostics ? ` — ${clockDiagnostics.state.toLowerCase()} at ${(clockDiagnostics.bpmX100 / 100).toFixed(2)} BPM` : ''}
          </summary>
          {clockDiagnostics ? (
            <dl>
              <div><dt>Source</dt><dd>{clockDiagnostics.source}</dd></div>
              <div><dt>State</dt><dd>{clockDiagnostics.state}</dd></div>
              <div><dt>Measured</dt><dd>{(clockDiagnostics.bpmX100 / 100).toFixed(2)} BPM</dd></div>
              <div><dt>Target</dt><dd>{(clockDiagnostics.targetBpmX100 / 100).toFixed(2)} BPM</dd></div>
              <div><dt>Jitter</dt><dd>{clockDiagnostics.jitterUs} µs</dd></div>
              <div><dt>Phase error</dt><dd>{clockDiagnostics.phaseErrorUs} µs</dd></div>
              <div><dt>Maximum phase error</dt><dd>{clockDiagnostics.maxPhaseErrorUs} µs</dd></div>
              <div><dt>Last edge age</dt><dd>{clockDiagnostics.lastEdgeAgeUs} µs</dd></div>
              <div><dt>PPQN</dt><dd>{clockDiagnostics.ppqn}</dd></div>
              <div><dt>Events</dt><dd>{clockDiagnostics.accepted} accepted / {clockDiagnostics.rejected} rejected / {clockDiagnostics.missed} missed</dd></div>
              <div><dt>Queue drops</dt><dd>{clockDiagnostics.clockQueueDrops} clock / {clockDiagnostics.midiQueueDrops} MIDI</dd></div>
            </dl>
          ) : (
            <p>Waiting for clock data…</p>
          )}
        </details>
      ) : null}

      {incompatibleDevice ? (
        <section className="firmware-warning" role="alert">
          <strong>Firmware update required</strong>
          <p>
            The connected pikocore firmware {incompatibleDevice.firmware ? `(${incompatibleDevice.firmware}) ` : ''}is too old for this
            loader. Install the latest UF2 firmware to continue, then reload this website after the new firmware is uploaded.
            Installing firmware will remove the pikocore's current samples. Stock samples can be found{' '}
            <a href="https://drive.google.com/file/d/1PyBJkbb_NRL8k7lSmdfswb9TaqgYocIz/view?usp=sharing" target="_blank" rel="noreferrer">
              here
            </a>
            .
          </p>
          <p>Required firmware: {requiredFirmwareLabel}. Use the UF2 selector and download button above.</p>
        </section>
      ) : null}

      {ittybittymidiInfoOpen ? (
        <div className="modal-backdrop" role="presentation" onClick={() => setIttybittymidiInfoOpen(false)}>
          <div
            className="info-modal"
            role="dialog"
            aria-modal="true"
            aria-label="ittybittymidi clock input"
            onClick={(event) => event.stopPropagation()}
          >
            <img src={ittybittymidiConnection} alt="ittybittymidi connected to a pikocore clock input" />
            <div className="info-modal-copy">
              <h2>ittybittymidi mode</h2>
              <p>
                If you have an ittybittymidi, enable this mode to send TRS MIDI directly into the pikocore clock input.
                Without it, leave this unchecked and use the clock input for pulses; USB MIDI still works either way.
              </p>
              <a href="https://infinitedigits.co/ittybittymidi/" title="Open the ittybittymidi page">
                ittybittymidi details
              </a>
            </div>
            <button
              className="modal-close"
              onClick={() => setIttybittymidiInfoOpen(false)}
              title="Close ittybittymidi info"
              aria-label="Close ittybittymidi info"
            >
              Close
            </button>
          </div>
        </div>
      ) : null}

      <section className="capacity">
        <div className="capacity-line">
          <span>{formatBytes(used)} used</span>
          <span>{capacity == null ? 'Capacity unknown' : `${formatBytes(Math.max(0, capacity - used))} free`}</span>
          <span>
            {samples.length} sample{samples.length === 1 ? '' : 's'}
          </span>
          <span>{device ? `FW ${device.firmware}` : 'No device'}</span>
        </div>
        <div className="meter">
          <div className={overCapacity ? 'over' : ''} style={{ width: `${usageRatio * 100}%` }} />
        </div>
        {busy && progress > 0 ? (
          <>
            <div className="transfer" style={{ width: `${progress * 100}%` }} />
            {transferText ? <div className="transfer-detail">{transferText}</div> : null}
          </>
        ) : null}
      </section>

      {debugOpen ? (
        <section className="debug-log">
          <div className="debug-title">Serial debug</div>
          {debugLog.length > 0 ? (
            <textarea
              className="debug-output"
              readOnly
              spellCheck={false}
              value={debugLog.join('\n')}
              onFocus={pauseDebugLog}
              onMouseDown={pauseDebugLog}
              onTouchStart={pauseDebugLog}
              onBlur={resumeDebugLog}
              title="Serial debug messages"
              aria-label="Serial debug messages"
            />
          ) : (
            <div className="debug-empty">No serial messages yet</div>
          )}
        </section>
      ) : null}

      <section
        className="sample-list"
        onDragOver={(event) => event.preventDefault()}
        onDrop={(event) => {
          event.preventDefault();
          if (!connected) {
            setStatus({
              text: incompatibleDevice ? 'Firmware update required before adding audio' : 'Connect to a pikocore before adding audio',
              kind: 'warn',
            });
            return;
          }
          void addFiles(event.dataTransfer.files);
        }}
      >
        {samples.length === 0 ? (
          <div className="empty">
            <FolderOpen size={28} />
            <span>No samples loaded</span>
          </div>
        ) : (
          samples.map((sample, index) => (
            <SampleRow
              key={sample.id}
              sample={sample}
              index={index}
              playing={playingId === sample.id}
              playheadFrame={playingId === sample.id ? playheadFrame : null}
              theme={theme}
              onUpdate={(patch) => updateSample(sample.id, patch)}
              onRemove={() => removeSample(sample.id)}
              onMove={(direction) => moveSample(index, direction)}
              onPreview={() => void togglePreview(sample)}
            />
          ))
        )}
      </section>

      <footer className="site-footer">
        made by{' '}
        <a href="https://infinitedigits.co/products/" title="Open Infinite Digits products">
          Infinite Digits
        </a>
        , inspired by{' '}
        <a href="https://github.com/dessertplanet/MLRws-web/" title="Open dessertplanet/MLRws-web on GitHub">
          MLRws-web
        </a>
      </footer>
    </main>
  );
}

function SampleRow({
  sample,
  index,
  playing,
  playheadFrame,
  theme,
  onUpdate,
  onRemove,
  onMove,
  onPreview,
}: {
  sample: BankSample;
  index: number;
  playing: boolean;
  playheadFrame: number | null;
  theme: Theme;
  onUpdate: (patch: Partial<BankSample>) => void;
  onRemove: () => void;
  onMove: (direction: -1 | 1) => void;
  onPreview: () => void;
}) {
  const length = croppedPcm(sample).length;
  return (
    <article className="sample-row">
      <div className="sample-meta">
        <span className="slot">{index + 1}</span>
        <input
          className="name"
          value={sample.name}
          maxLength={47}
          onChange={(event) => onUpdate({ name: event.target.value })}
        />
        <div className="sample-stats">
          <label>
            BPM
            <input
              className="bpm"
              type="number"
              min={1}
              max={65535}
              value={sample.bpm}
              onChange={(event) => onUpdate({ bpm: Number(event.target.value) || 1 })}
            />
          </label>
          <label>
            Beats
            <input
              className="bpm"
              type="number"
              min={1}
              max={65535}
              value={sample.beats}
              onChange={(event) => onUpdate({ beats: Number(event.target.value) || 1 })}
            />
          </label>
          <span>{formatDuration(length)}</span>
          <span>{formatBytes(length)}</span>
        </div>
      </div>
      <Waveform sample={sample} playheadFrame={playheadFrame} theme={theme} />
      <div className="row-actions">
        <button
          onClick={onPreview}
          title={playing ? 'Stop preview playback' : 'Preview this sample'}
          aria-label={playing ? 'Stop preview playback' : 'Preview this sample'}
        >
          {playing ? <Pause size={16} /> : <Play size={16} />}
        </button>
        <button
          onClick={() => onMove(-1)}
          disabled={index === 0}
          title="Move this sample up in the bank"
          aria-label="Move this sample up in the bank"
        >
          <ArrowUp size={16} />
        </button>
        <button onClick={() => onMove(1)} title="Move this sample down in the bank" aria-label="Move this sample down in the bank">
          <ArrowDown size={16} />
        </button>
        <button onClick={onRemove} title="Remove this sample from the bank" aria-label="Remove this sample from the bank">
          <Trash2 size={16} />
        </button>
      </div>
    </article>
  );
}

function Waveform({
  sample,
  playheadFrame,
  theme,
}: {
  sample: BankSample;
  playheadFrame: number | null;
  theme: Theme;
}) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  function draw(canvas: HTMLCanvasElement) {
    const rect = canvas.getBoundingClientRect();
    const scale = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.floor(rect.width * scale));
    const height = Math.max(1, Math.floor(rect.height * scale));
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const waveBackground = theme === 'dark' ? '#191918' : '#f7f7f4';
    const waveLine = theme === 'dark' ? '#f1f0ea' : '#202020';
    ctx.clearRect(0, 0, width, height);
    ctx.fillStyle = waveBackground;
    ctx.fillRect(0, 0, width, height);
    ctx.strokeStyle = waveLine;
    ctx.lineWidth = Math.max(1, scale);
    ctx.beginPath();
    const mid = height / 2;
    for (let x = 0; x < width; x++) {
      const start = Math.floor((x / width) * sample.pcm.length);
      const end = Math.max(start + 1, Math.floor(((x + 1) / width) * sample.pcm.length));
      let min = 0;
      let max = 0;
      for (let i = start; i < end; i++) {
        const value = (sample.pcm[i] - 128) / 128;
        min = Math.min(min, value);
        max = Math.max(max, value);
      }
      ctx.moveTo(x, mid + min * mid * 0.9);
      ctx.lineTo(x, mid + max * mid * 0.9);
    }
    ctx.stroke();

    if (playheadFrame != null) {
      const playhead = ((sample.cropStart + playheadFrame) / sample.pcm.length) * width;
      ctx.fillStyle = theme === 'dark' ? '#ff7667' : '#d23b2a';
      ctx.fillRect(playhead, 0, Math.max(2, 2 * scale), height);
    }
  }

  useEffect(() => {
    if (canvasRef.current) draw(canvasRef.current);
  }, [sample, playheadFrame, theme]);

  return (
    <canvas
      ref={(node) => {
        canvasRef.current = node;
        if (node) draw(node);
      }}
      className="waveform"
    />
  );
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function formatTransferDetail(detail: TransferDetail): string {
  const eta = detail.etaMs != null ? ` - ${formatEta(detail.etaMs)} remaining` : '';
  if (detail.totalBytes == null) return `${formatBytes(detail.transferredBytes)}${eta}`;
  return `${formatBytes(detail.transferredBytes)} / ${formatBytes(detail.totalBytes)}${eta}`;
}

function parseContentLength(value: string | null): number | null {
  if (!value) return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
}

function saveBlob(blob: Blob, filename: string): void {
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  link.style.display = 'none';
  document.body.append(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

function formatDuration(frames: number): string {
  return `${(frames / BANK_SAMPLE_RATE).toFixed(2)} s`;
}

function pulseDivisionLabel(ppqn: 1 | 2 | 4): string {
  if (ppqn === 1) return 'quarter notes';
  if (ppqn === 4) return 'sixteenth notes';
  return 'eighth notes';
}

function formatEta(ms: number): string {
  const seconds = Math.max(1, Math.ceil(ms / 1000));
  if (seconds < 60) return `${seconds}s`;
  const minutes = Math.floor(seconds / 60);
  const remainingSeconds = seconds % 60;
  if (minutes < 60) return remainingSeconds > 0 ? `${minutes}m ${remainingSeconds}s` : `${minutes}m`;
  const hours = Math.floor(minutes / 60);
  const remainingMinutes = minutes % 60;
  return remainingMinutes > 0 ? `${hours}h ${remainingMinutes}m` : `${hours}h`;
}

function loadTheme(): Theme {
  const stored =
    window.localStorage.getItem('pikocore-theme') ?? window.localStorage.getItem('pikocore-theme');
  if (stored === 'light' || stored === 'dark') return stored;
  return window.matchMedia?.('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
}

function colorWithAlpha(color: string, alpha: number): string {
  if (!color.startsWith('#')) return color;
  const hex = color.slice(1);
  const full =
    hex.length === 3
      ? hex
          .split('')
          .map((part) => part + part)
          .join('')
      : hex;
  const value = Number.parseInt(full, 16);
  if (!Number.isFinite(value)) return color;
  const r = (value >> 16) & 0xff;
  const g = (value >> 8) & 0xff;
  const b = value & 0xff;
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}
