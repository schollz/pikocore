import { describe, expect, it } from 'vitest';
import { BANK_HEADER_SIZE, BANK_MAX_SAMPLES, BANK_VERSION } from './bank';
import {
  hasClockSync,
  isCompatibleFirmware,
  parseClockDiagnostics,
  parseInfo,
  pulsePpqnCommand,
  shouldPollClockDiagnostics,
} from './serial';

const baseInfo = 'PIKO1 FW 2.2 F 16777216 R 524288 S 520192 A 524288 C 16240640 U 0 SR 24000 N 0 CLOCK_INPUT CLOCK';

describe('serial metadata parsing', () => {
  it('parses firmware compatibility tokens', () => {
    const info = parseInfo(
      `${baseInfo} PROTO 1 BANK_VERSION ${BANK_VERSION} BANK_HEADER_SIZE ${BANK_HEADER_SIZE} BANK_MAX_SAMPLES ${BANK_MAX_SAMPLES}\nEND\n`,
    );

    expect(info.firmware).toBe('2.2');
    expect(info.protocolVersion).toBe(1);
    expect(info.bankVersion).toBe(BANK_VERSION);
    expect(info.bankHeaderSize).toBe(BANK_HEADER_SIZE);
    expect(info.bankMaxSamples).toBe(BANK_MAX_SAMPLES);
  });

  it('parses clock sync and PPQN capability metadata', () => {
    const info = parseInfo(
      `${baseInfo.replace('FW 2.2', 'FW 2.4')} CLOCK_SYNC_VERSION 1 PULSE_PPQN 4 PROTO 1 BANK_VERSION ${BANK_VERSION} BANK_HEADER_SIZE ${BANK_HEADER_SIZE} BANK_MAX_SAMPLES ${BANK_MAX_SAMPLES}\nEND\n`,
    );
    expect(info.clockSyncVersion).toBe(1);
    expect(info.pulsePpqn).toBe(4);
    expect(hasClockSync(info)).toBe(true);
  });

  it('feature-detects older firmware without changing compatibility', () => {
    const info = parseInfo(
      `${baseInfo} PROTO 1 BANK_VERSION ${BANK_VERSION} BANK_HEADER_SIZE ${BANK_HEADER_SIZE} BANK_MAX_SAMPLES ${BANK_MAX_SAMPLES}\nEND\n`,
    );
    expect(info.clockSyncVersion).toBeUndefined();
    expect(info.pulsePpqn).toBeUndefined();
    expect(hasClockSync(info)).toBe(false);
  });
});

describe('clock diagnostics', () => {
  it('parses the length-prefixed payload body tokens', () => {
    const diagnostics = parseClockDiagnostics(
      'CLOCK1 SOURCE PULSE STATE LOCKED BPM_X100 12000 TARGET_BPM_X100 12000 JITTER_US 12 PHASE_ERROR_US -7 MAX_PHASE_ERROR_US 42 LAST_EDGE_AGE_US 100 PPQN 2 ACCEPTED 20 REJECTED 0 MISSED 0 CLOCK_QUEUE_DROPS 0 MIDI_QUEUE_DROPS 0\nEND\n',
    );
    expect(diagnostics.source).toBe('PULSE');
    expect(diagnostics.state).toBe('LOCKED');
    expect(diagnostics.bpmX100).toBe(12000);
    expect(diagnostics.phaseErrorUs).toBe(-7);
    expect(diagnostics.clockQueueDrops).toBe(0);
  });

  it('rejects malformed diagnostics', () => {
    expect(() => parseClockDiagnostics('CLOCK0 STATE LOCKED')).toThrow('Bad clock diagnostics');
  });
});

describe('pulse PPQN command', () => {
  it('encodes supported PPQN settings', () => {
    expect(Array.from(pulsePpqnCommand(1))).toEqual([0x50, 1]);
    expect(Array.from(pulsePpqnCommand(2))).toEqual([0x50, 2]);
    expect(Array.from(pulsePpqnCommand(4))).toEqual([0x50, 4]);
  });

  it('rejects unsupported PPQN settings', () => {
    expect(() => pulsePpqnCommand(3)).toThrow('Invalid pulse PPQN');
  });
});

describe('diagnostic polling suspension', () => {
  const capable = parseInfo(
    `${baseInfo} CLOCK_SYNC_VERSION 1 PULSE_PPQN 2 PROTO 1 BANK_VERSION ${BANK_VERSION} BANK_HEADER_SIZE ${BANK_HEADER_SIZE} BANK_MAX_SAMPLES ${BANK_MAX_SAMPLES}\nEND\n`,
  );

  it('polls only when connected, capable, idle, and without a request in flight', () => {
    expect(shouldPollClockDiagnostics(true, false, capable, false)).toBe(true);
    expect(shouldPollClockDiagnostics(true, true, capable, false)).toBe(false);
    expect(shouldPollClockDiagnostics(true, false, capable, true)).toBe(false);
    expect(shouldPollClockDiagnostics(false, false, capable, false)).toBe(false);
    expect(shouldPollClockDiagnostics(true, false, parseInfo(baseInfo), false)).toBe(false);
  });
});

describe('firmware compatibility', () => {
  it('treats missing metadata as incompatible', () => {
    expect(isCompatibleFirmware(parseInfo(baseInfo))).toBe(false);
  });

  it('treats FW 2.1 without compatibility tokens as incompatible', () => {
    expect(isCompatibleFirmware(parseInfo(baseInfo.replace('FW 2.2', 'FW 2.1')))).toBe(false);
  });

  it('accepts matching v2 bank metadata', () => {
    const info = parseInfo(
      `${baseInfo} PROTO 1 BANK_VERSION ${BANK_VERSION} BANK_HEADER_SIZE ${BANK_HEADER_SIZE} BANK_MAX_SAMPLES ${BANK_MAX_SAMPLES}\nEND\n`,
    );

    expect(isCompatibleFirmware(info)).toBe(true);
  });

  it('rejects wrong bank version', () => {
    const info = parseInfo(`${baseInfo} PROTO 1 BANK_VERSION 1 BANK_HEADER_SIZE ${BANK_HEADER_SIZE} BANK_MAX_SAMPLES ${BANK_MAX_SAMPLES}\nEND\n`);

    expect(isCompatibleFirmware(info)).toBe(false);
  });

  it('rejects wrong header size', () => {
    const info = parseInfo(`${baseInfo} PROTO 1 BANK_VERSION ${BANK_VERSION} BANK_HEADER_SIZE 4096 BANK_MAX_SAMPLES ${BANK_MAX_SAMPLES}\nEND\n`);

    expect(isCompatibleFirmware(info)).toBe(false);
  });

  it('rejects insufficient max sample support', () => {
    const info = parseInfo(`${baseInfo} PROTO 1 BANK_VERSION ${BANK_VERSION} BANK_HEADER_SIZE ${BANK_HEADER_SIZE} BANK_MAX_SAMPLES 32\nEND\n`);

    expect(isCompatibleFirmware(info)).toBe(false);
  });
});
