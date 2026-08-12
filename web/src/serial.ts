import { BANK_HEADER_SIZE, BANK_MAX_SAMPLES, BANK_VERSION } from './bank';

export interface DeviceInfo {
  firmware: string;
  reserveBytes: number;
  flashBytes: number;
  settingsOffset: number;
  audioOffset: number;
  capacityBytes: number;
  usedBytes: number;
  sampleRate: number;
  sampleCount: number;
  ittybittymidiMode: boolean;
  pulsePpqn?: 1 | 2 | 4;
  clockSyncVersion?: number;
  protocolVersion?: number;
  bankVersion?: number;
  bankHeaderSize?: number;
  bankMaxSamples?: number;
  raw: string;
}

export interface ClockDiagnostics {
  source: 'INTERNAL' | 'PULSE' | 'MIDI';
  state: 'UNLOCKED' | 'ACQUIRING' | 'LOCKED' | 'HOLDOVER';
  bpmX100: number;
  targetBpmX100: number;
  jitterUs: number;
  phaseErrorUs: number;
  maxPhaseErrorUs: number;
  lastEdgeAgeUs: number;
  ppqn: number;
  accepted: number;
  rejected: number;
  missed: number;
  clockQueueDrops: number;
  midiQueueDrops: number;
  raw: string;
}

export function hasClockSync(info: DeviceInfo | null): boolean {
  return info?.clockSyncVersion === 1;
}

export function shouldPollClockDiagnostics(
  connected: boolean,
  busy: boolean,
  info: DeviceInfo | null,
  requestInFlight: boolean,
): boolean {
  return connected && !busy && hasClockSync(info) && !requestInFlight;
}

export function pulsePpqnCommand(ppqn: number): Uint8Array {
  if (ppqn !== 1 && ppqn !== 2 && ppqn !== 4) throw new Error(`Invalid pulse PPQN ${ppqn}`);
  return new Uint8Array([0x50, ppqn]);
}

export function isCompatibleFirmware(info: DeviceInfo): boolean {
  return (
    info.protocolVersion != null &&
    info.protocolVersion >= 1 &&
    info.bankVersion === BANK_VERSION &&
    info.bankHeaderSize === BANK_HEADER_SIZE &&
    info.bankMaxSamples != null &&
    info.bankMaxSamples >= BANK_MAX_SAMPLES
  );
}

const READ_CHUNK_SIZE = 1024;
const READ_CHUNK_ACK = 0x41;
const TRANSFER_ABORT = 0x51;
const METADATA_TIMEOUT_MS = 3000;
const METADATA_GRACE_MS = 500;
const COMMAND_TIMEOUT_MS = 8000;
const READ_TIMEOUT_MS = 15000;
const LINE_TIMEOUT_MS = 5000;
const WRITE_READY_TIMEOUT_MS = 5000;
const WRITE_DONE_TIMEOUT_MS = 60000;
const ERASE_TIMEOUT_MS = 10000;
const PIKOCORE_USB_VENDOR_ID = 0x2e8a;
const PIKOCORE_USB_PRODUCT_ID = 0x4009;
const BOOTSEL_MAGIC_BAUD_RATE = 1200;
const BOOTSEL_SIGNAL_DELAY_MS = 50;

export class PikocoreSerial {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private readBuffer = new Uint8Array(0);
  private waiters: Array<() => void> = [];
  private closing = false;
  private logger: ((message: string) => void) | null = null;
  private suppressRxLog = false;

  setLogger(logger: ((message: string) => void) | null): void {
    this.logger = logger;
  }

  connected(): boolean {
    return !!this.port && !!this.reader && !!this.writer;
  }

  async connect(): Promise<void> {
    if (!navigator.serial) {
      throw new Error('Web Serial is not available in this browser');
    }
    this.port = await navigator.serial.requestPort({
      filters: [{ usbVendorId: PIKOCORE_USB_VENDOR_ID, usbProductId: PIKOCORE_USB_PRODUCT_ID }],
    });
    const info = this.port.getInfo?.();
    this.log(`selected VID=${hex(info?.usbVendorId)} PID=${hex(info?.usbProductId)}`);
    await this.port.open({ baudRate: 115200, bufferSize: 4096 });
    this.log('port opened');
    if (!this.port.readable || !this.port.writable) throw new Error('Serial port did not open');
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
    this.readLoop(this.reader);
    await this.port.setSignals?.({ dataTerminalReady: false, requestToSend: false });
    await delay(50);
    await this.port.setSignals?.({ dataTerminalReady: true, requestToSend: true });
    this.log('DTR/RTS asserted');
    await delay(250);
  }

  async disconnect(): Promise<void> {
    await this.closeCurrentPort(true);
  }

  async resetToBootloader(): Promise<void> {
    if (!navigator.serial) {
      throw new Error('Web Serial is not available in this browser');
    }
    const port = await navigator.serial.requestPort({
      filters: [{ usbVendorId: PIKOCORE_USB_VENDOR_ID, usbProductId: PIKOCORE_USB_PRODUCT_ID }],
    });
    if (this.port) await this.closeCurrentPort(true);
    const info = port.getInfo?.();
    this.log(`bootloader reset target VID=${hex(info?.usbVendorId)} PID=${hex(info?.usbProductId)}`);
    try {
      await port.open({ baudRate: BOOTSEL_MAGIC_BAUD_RATE, bufferSize: 255 });
      this.log(`port opened at ${BOOTSEL_MAGIC_BAUD_RATE} baud for BOOTSEL reset`);
      try {
        await port.setSignals?.({ dataTerminalReady: true, requestToSend: true });
      } catch (_) {}
      await delay(BOOTSEL_SIGNAL_DELAY_MS);
    } finally {
      try {
        await port.close();
      } catch (_) {}
    }
    this.log('BOOTSEL reset signaled');
  }

  private async closeCurrentPort(sendAbort: boolean): Promise<void> {
    this.closing = true;
    const reader = this.reader;
    const writer = this.writer;
    const port = this.port;
    this.reader = null;
    this.writer = null;
    this.port = null;
    this.readBuffer = new Uint8Array(0);
    this.notify();
    if (sendAbort) {
      try {
        await writer?.write(new Uint8Array([TRANSFER_ABORT]));
      } catch (_) {}
    }
    try {
      await reader?.cancel();
    } catch (_) {}
    try {
      reader?.releaseLock();
    } catch (_) {}
    try {
      writer?.releaseLock();
    } catch (_) {}
    try {
      await port?.close();
    } catch (_) {}
    this.closing = false;
  }

  async sync(): Promise<void> {
    const marker = new TextEncoder().encode('SYNC\n');
    for (let attempt = 0; attempt < 4; attempt++) {
      this.readBuffer = new Uint8Array(0);
      this.log(`sync attempt ${attempt + 1}: write X`);
      await this.write(new Uint8Array([TRANSFER_ABORT]));
      await delay(20);
      await this.writeString('X');
      const deadline = Date.now() + 2000;
      while (Date.now() < deadline) {
        const index = findSequence(this.readBuffer, marker);
        if (index >= 0) {
          this.readBuffer = this.readBuffer.slice(index + marker.length);
          await delay(30);
          this.readBuffer = new Uint8Array(0);
          this.log(`sync attempt ${attempt + 1}: got SYNC`);
          return;
        }
        try {
          await this.waitForData(Math.max(1, deadline - Date.now()));
        } catch (_) {
          const lateIndex = findSequence(this.readBuffer, marker);
          if (lateIndex >= 0) {
            this.readBuffer = this.readBuffer.slice(lateIndex + marker.length);
            await delay(30);
            this.readBuffer = new Uint8Array(0);
            this.log(`sync attempt ${attempt + 1}: got SYNC`);
            return;
          }
          break;
        }
      }
      await delay(150);
    }
    throw new Error('Device did not respond to sync');
  }

  async info(skipSync = false): Promise<DeviceInfo> {
    if (!skipSync) await this.sync();
    await this.writeString('I');
    return this.readInfoPayload();
  }

  async stopAndInfo(skipSync = false): Promise<DeviceInfo> {
    if (!skipSync) await this.sync();
    await this.writeString('B');
    return this.readInfoPayload();
  }

  async stopAudio(skipSync = false): Promise<void> {
    if (!skipSync) await this.sync();
    await this.writeString('S');
    const line = await this.waitForLine(COMMAND_TIMEOUT_MS);
    if (line !== 'OK') throw new Error(`Stop rejected: ${line}`);
  }

  async setClockInputMode(ittybittymidiMode: boolean): Promise<void> {
    await this.sync();
    await this.writeString('C');
    await this.write(new Uint8Array([ittybittymidiMode ? 1 : 0]));
    const line = await this.waitForLine(COMMAND_TIMEOUT_MS);
    if (line !== 'OK') throw new Error(`Clock input setting rejected: ${line}`);
  }

  async setPulsePpqn(ppqn: 1 | 2 | 4): Promise<void> {
    await this.sync();
    await this.write(pulsePpqnCommand(ppqn));
    const line = await this.waitForLine(COMMAND_TIMEOUT_MS);
    if (line !== 'OK') throw new Error(`Pulse PPQN setting rejected: ${line}`);
  }

  async clockDiagnostics(skipSync = false): Promise<ClockDiagnostics> {
    if (!skipSync) await this.sync();
    await this.writeString('D');
    const lenBytes = await this.waitForBytes(4, METADATA_TIMEOUT_MS, METADATA_GRACE_MS);
    const len = new DataView(lenBytes.buffer, lenBytes.byteOffset, 4).getUint32(0, true);
    if (len === 0 || len > 4096) throw new Error(`Invalid diagnostics length ${len}`);
    const payload = await this.waitForBytes(len, METADATA_TIMEOUT_MS, METADATA_GRACE_MS);
    return parseClockDiagnostics(new TextDecoder().decode(payload));
  }

  async readBank(progress?: (ratio: number, transferredBytes: number, totalBytes: number) => void): Promise<Uint8Array | null> {
    await this.sync();
    await this.writeString('R');
    const lenBytes = await this.waitForBytes(4, COMMAND_TIMEOUT_MS);
    const totalLen = new DataView(lenBytes.buffer, lenBytes.byteOffset, 4).getUint32(0, true);
    if (totalLen === 0) return null;
    const data = new Uint8Array(totalLen);
    let received = 0;
    let nextAck = Math.min(READ_CHUNK_SIZE, totalLen);
    this.suppressRxLog = true;
    try {
      while (received < totalLen) {
        if (this.readBuffer.length === 0) await this.waitForData(READ_TIMEOUT_MS);
        const chunk = Math.min(this.readBuffer.length, totalLen - received);
        data.set(this.readBuffer.slice(0, chunk), received);
        this.readBuffer = this.readBuffer.slice(chunk);
        received += chunk;
        while (received >= nextAck) {
          await this.write(new Uint8Array([READ_CHUNK_ACK]));
          if (nextAck >= totalLen) break;
          nextAck = Math.min(nextAck + READ_CHUNK_SIZE, totalLen);
        }
        progress?.(received / totalLen, received, totalLen);
      }
    } finally {
      this.suppressRxLog = false;
    }
    const done = await this.waitForLine(LINE_TIMEOUT_MS);
    if (done !== 'DONE') throw new Error(`Unexpected read terminator: ${done}`);
    return data;
  }

  async writeBank(blob: Uint8Array, progress?: (ratio: number, transferredBytes: number, totalBytes: number) => void): Promise<void> {
    await this.sync();
    const len = new Uint8Array(4);
    new DataView(len.buffer).setUint32(0, blob.length, true);
    await this.writeString('W');
    await this.write(len);
    const ready = await this.waitForLine(WRITE_READY_TIMEOUT_MS);
    if (ready !== 'OK') throw new Error(`Write rejected: ${ready}`);
    for (let offset = 0; offset < blob.length; offset += 256) {
      const end = Math.min(blob.length, offset + 256);
      await this.write(blob.slice(offset, end));
      progress?.(end / blob.length, end, blob.length);
      if ((offset & 0x0fff) === 0) await new Promise((resolve) => setTimeout(resolve, 0));
    }
    const done = await this.waitForLine(WRITE_DONE_TIMEOUT_MS);
    if (done !== 'OK') throw new Error(`Write failed: ${done}`);
  }

  async erase(): Promise<void> {
    await this.sync();
    await this.writeString('E');
    const line = await this.waitForLine(ERASE_TIMEOUT_MS);
    if (line !== 'OK') throw new Error(`Erase failed: ${line}`);
  }

  async bootloader(): Promise<void> {
    await this.sync();
    await this.writeString('U');
    const line = await this.waitForLine(COMMAND_TIMEOUT_MS);
    if (line !== 'OK') throw new Error(`Bootloader reset rejected: ${line}`);
  }

  private async readLoop(reader: ReadableStreamDefaultReader<Uint8Array>): Promise<void> {
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value) continue;
        if (!this.suppressRxLog) this.log(`rx ${value.length} bytes: ${previewBytes(value)}`);
        const next = new Uint8Array(this.readBuffer.length + value.length);
        next.set(this.readBuffer);
        next.set(value, this.readBuffer.length);
        this.readBuffer = next;
        this.notify();
      }
    } catch (error) {
      if (!this.closing) console.warn(error);
    }
  }

  private async writeString(value: string): Promise<void> {
    await this.write(new TextEncoder().encode(value));
  }

  private async write(value: Uint8Array): Promise<void> {
    if (!this.writer) throw new Error('Not connected');
    this.log(`tx ${value.length} bytes: ${previewBytes(value)}`);
    await this.writer.write(value);
  }

  private notify(): void {
    const waiters = this.waiters;
    this.waiters = [];
    waiters.forEach((waiter) => waiter());
  }

  private async waitForData(timeoutMs: number): Promise<void> {
    if (this.readBuffer.length > 0) return;
    await new Promise<void>((resolve, reject) => {
      const waiter = () => {
        clearTimeout(timer);
        resolve();
      };
      const timer = window.setTimeout(() => {
        this.waiters = this.waiters.filter((entry) => entry !== waiter);
        this.log(`waitForData timeout after ${timeoutMs} ms`);
        reject(new Error('Timed out waiting for serial data'));
      }, timeoutMs);
      this.waiters.push(waiter);
    });
  }

  private async waitForNewData(previousLength: number, timeoutMs: number): Promise<void> {
    if (this.readBuffer.length > previousLength) return;
    await new Promise<void>((resolve, reject) => {
      const waiter = () => {
        clearTimeout(timer);
        resolve();
      };
      const timer = window.setTimeout(() => {
        this.waiters = this.waiters.filter((entry) => entry !== waiter);
        this.log(`waitForData timeout after ${timeoutMs} ms`);
        reject(new Error('Timed out waiting for serial data'));
      }, timeoutMs);
      this.waiters.push(waiter);
    });
  }

  private async waitForBytes(length: number, timeoutMs: number, graceMs = 0): Promise<Uint8Array> {
    let deadline = Date.now() + timeoutMs;
    let finalDeadline = deadline + graceMs;
    let inGrace = false;
    while (this.readBuffer.length < length) {
      const now = Date.now();
      if (now >= finalDeadline) throw new Error('Timed out waiting for serial bytes');
      if (now >= deadline && !inGrace) {
        inGrace = true;
        this.log(`serial byte wait entered ${graceMs} ms grace window`);
      }
      const waitUntil = now < deadline ? deadline : finalDeadline;
      const previousLength = this.readBuffer.length;
      try {
        await this.waitForNewData(previousLength, Math.max(1, waitUntil - now));
      } catch (_) {
        if (this.readBuffer.length >= length) break;
        if (Date.now() < finalDeadline) continue;
        throw new Error('Timed out waiting for serial bytes');
      }
      if (this.readBuffer.length > previousLength) {
        deadline = Date.now() + timeoutMs;
        finalDeadline = deadline + graceMs;
        inGrace = false;
      }
    }
    const result = this.readBuffer.slice(0, length);
    this.readBuffer = this.readBuffer.slice(length);
    return result;
  }

  private async waitForLine(timeoutMs: number): Promise<string> {
    const deadline = Date.now() + timeoutMs;
    while (true) {
      const index = this.readBuffer.indexOf(10);
      if (index >= 0) {
        const line = new TextDecoder().decode(this.readBuffer.slice(0, index)).trim();
        this.readBuffer = this.readBuffer.slice(index + 1);
        return line;
      }
      const remaining = deadline - Date.now();
      if (remaining <= 0) throw new Error('Timed out waiting for serial line');
      await this.waitForData(remaining);
    }
  }

  private async readInfoPayload(): Promise<DeviceInfo> {
    const lenBytes = await this.waitForBytes(4, METADATA_TIMEOUT_MS, METADATA_GRACE_MS);
    const len = new DataView(lenBytes.buffer, lenBytes.byteOffset, 4).getUint32(0, true);
    if (len === 0 || len > 4096) throw new Error(`Invalid metadata length ${len}`);
    const payload = await this.waitForBytes(len, METADATA_TIMEOUT_MS, METADATA_GRACE_MS);
    return parseInfo(new TextDecoder().decode(payload));
  }

  private log(message: string): void {
    this.logger?.(message);
  }
}

function findSequence(haystack: Uint8Array, needle: Uint8Array): number {
  for (let i = 0; i <= haystack.length - needle.length; i++) {
    let match = true;
    for (let j = 0; j < needle.length; j++) {
      if (haystack[i + j] !== needle[j]) {
        match = false;
        break;
      }
    }
    if (match) return i;
  }
  return -1;
}

export function parseInfo(text: string): DeviceInfo {
  const first = text.trim().split('\n')[0] ?? '';
  const parts = first.split(/\s+/);
  if (parts[0] !== 'PIKO1') throw new Error(`Bad metadata: ${first}`);
  const token = (names: string | string[], fallback: string) => {
    const candidates = Array.isArray(names) ? names : [names];
    for (const name of candidates) {
      const index = parts.indexOf(name);
      if (index >= 0 && parts[index + 1]) return parts[index + 1];
    }
    return fallback;
  };
  const numberToken = (names: string | string[]) => {
    const value = token(names, '');
    if (!value) return undefined;
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : undefined;
  };
  return {
    firmware: token('FW', '--'),
    flashBytes: Number(token(['FLASH', 'F'], '0')),
    reserveBytes: Number(token(['RESERVE', 'R'], '0')),
    settingsOffset: Number(token(['SETTINGS', 'S'], '0')),
    audioOffset: Number(token(['AUDIO_OFFSET', 'A'], '0')),
    capacityBytes: Number(token(['CAPACITY', 'C'], '0')),
    usedBytes: Number(token(['USED', 'U'], '0')),
    sampleRate: Number(token(['RATE', 'SR'], '24000')),
    sampleCount: Number(token(['COUNT', 'N'], '0')),
    ittybittymidiMode: token(['CLOCK_INPUT', 'CI'], 'CLOCK') === 'MIDI',
    pulsePpqn: parsePulsePpqn(numberToken('PULSE_PPQN')),
    clockSyncVersion: numberToken('CLOCK_SYNC_VERSION'),
    protocolVersion: numberToken('PROTO'),
    bankVersion: numberToken('BANK_VERSION'),
    bankHeaderSize: numberToken('BANK_HEADER_SIZE'),
    bankMaxSamples: numberToken('BANK_MAX_SAMPLES'),
    raw: text,
  };
}

function parsePulsePpqn(value: number | undefined): 1 | 2 | 4 | undefined {
  return value === 1 || value === 2 || value === 4 ? value : undefined;
}

export function parseClockDiagnostics(text: string): ClockDiagnostics {
  const first = text.trim().split('\n')[0] ?? '';
  const parts = first.split(/\s+/);
  if (parts[0] !== 'CLOCK1') throw new Error(`Bad clock diagnostics: ${first}`);
  const token = (name: string) => {
    const index = parts.indexOf(name);
    if (index < 0 || !parts[index + 1]) throw new Error(`Missing ${name} in clock diagnostics`);
    return parts[index + 1];
  };
  const numberToken = (name: string) => {
    const value = Number(token(name));
    if (!Number.isFinite(value)) throw new Error(`Invalid ${name} in clock diagnostics`);
    return value;
  };
  const source = token('SOURCE');
  const state = token('STATE');
  if (source !== 'INTERNAL' && source !== 'PULSE' && source !== 'MIDI') {
    throw new Error(`Invalid clock source ${source}`);
  }
  if (state !== 'UNLOCKED' && state !== 'ACQUIRING' && state !== 'LOCKED' && state !== 'HOLDOVER') {
    throw new Error(`Invalid clock state ${state}`);
  }
  return {
    source,
    state,
    bpmX100: numberToken('BPM_X100'),
    targetBpmX100: numberToken('TARGET_BPM_X100'),
    jitterUs: numberToken('JITTER_US'),
    phaseErrorUs: numberToken('PHASE_ERROR_US'),
    maxPhaseErrorUs: numberToken('MAX_PHASE_ERROR_US'),
    lastEdgeAgeUs: numberToken('LAST_EDGE_AGE_US'),
    ppqn: numberToken('PPQN'),
    accepted: numberToken('ACCEPTED'),
    rejected: numberToken('REJECTED'),
    missed: numberToken('MISSED'),
    clockQueueDrops: numberToken('CLOCK_QUEUE_DROPS'),
    midiQueueDrops: numberToken('MIDI_QUEUE_DROPS'),
    raw: text,
  };
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function hex(value?: number): string {
  return value == null ? '--' : `0x${value.toString(16).padStart(4, '0')}`;
}

function previewBytes(value: Uint8Array): string {
  const text = new TextDecoder().decode(value.slice(0, 32)).replace(/[^\x20-\x7e\n\r]/g, '.');
  const hexBytes = Array.from(value.slice(0, 12), (byte) => byte.toString(16).padStart(2, '0')).join(' ');
  return `${JSON.stringify(text)} [${hexBytes}${value.length > 12 ? ' ...' : ''}]`;
}
