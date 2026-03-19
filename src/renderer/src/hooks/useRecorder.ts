import { useState, useEffect, useCallback } from 'react';

export interface RecordingConfig {
  resolution: { width: number; height: number };
  fps: number;
  bitrate: number;
  savePath: string;
  separateAudio: boolean;
  remuxToMp4: boolean;
  captureAudio?: boolean;
  captureMicrophone?: boolean;
  microphoneDevice?: string;  // 麦克风设备名称
  organizeByTimestamp?: boolean;
}

export interface RecordingStatus {
  state: 'idle' | 'recording' | 'paused';
  duration: number;
  frameCount: number;
  resolution?: { width: number; height: number };
  fps?: number;
  mouseActivity?: number;
  inputState?: {
    anyKeyPressed: boolean;
    mouseButtonPressed: boolean;
    pressedKeyCount: number;
  };
}

export interface SystemInfo {
  screenWidth: number;
  screenHeight: number;
  scalingFactor: number;
  refreshRate: number;
  cpuName: string;
  gpuName: string;
  ramGB: number;
  mousePollingRate: number;
}

export interface FinishResult {
  videoPath: string;
  actionsPath: string;
  movementsPath: string;
  duration: number;
  recordingFolder: string;
}

export interface InputState {
  anyKeyPressed: boolean;
  mouseButtonPressed: boolean;
  pressedKeyCount: number;
  pressedVKs?: number[];
  pressedMouseBtns?: number[];
}

export function useRecorder() {
  const [status, setStatus] = useState<RecordingStatus>({
    state: 'idle',
    duration: 0,
    frameCount: 0,
  });
  const [error, setError] = useState<string | null>(null);
  const [systemInfo, setSystemInfo] = useState<SystemInfo | null>(null);
  const [lastResult, setLastResult] = useState<FinishResult | null>(null);

  // Subscribe to status updates from main process
  useEffect(() => {
    window.electronAPI.onRecordingStatus(setStatus);
    window.electronAPI.onRecordingError(setError);
    window.electronAPI.onRecordingFinish((result) => {
      setLastResult(result);
      setStatus({ state: 'idle', duration: 0, frameCount: 0 });
    });
  }, []);

  // Load system info on mount
  useEffect(() => {
    window.electronAPI.getSystemInfo().then(setSystemInfo).catch(console.error);
  }, []);

  // Control functions
  const startRecording = useCallback(async (config: RecordingConfig) => {
    try {
      setError(null);
      await window.electronAPI.startRecording(config);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to start recording');
    }
  }, []);

  const stopRecording = useCallback(async () => {
    try {
      const result = await window.electronAPI.stopRecording();
      setLastResult(result);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to stop recording');
    }
  }, []);

  const pauseRecording = useCallback(async () => {
    try {
      await window.electronAPI.pauseRecording();
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to pause recording');
    }
  }, []);

  const resumeRecording = useCallback(async () => {
    try {
      await window.electronAPI.resumeRecording();
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to resume recording');
    }
  }, []);

  const checkInputState = useCallback(async (): Promise<InputState> => {
    try {
      return await window.electronAPI.checkInputState();
    } catch (err) {
      console.error('Failed to check input state:', err);
      return { anyKeyPressed: false, mouseButtonPressed: false, pressedKeyCount: 0 };
    }
  }, []);

  // Derived state
  const isRecording = status.state === 'recording';
  const isPaused = status.state === 'paused';
  const isIdle = status.state === 'idle';

  const canStart = isIdle;
  const canPause = isRecording;
  const canResume = isPaused;
  const canStop = isRecording || isPaused;

  return {
    // State
    status,
    error,
    systemInfo,
    lastResult,

    // Derived state
    isRecording,
    isPaused,
    isIdle,
    canStart,
    canPause,
    canResume,
    canStop,

    // Actions
    startRecording,
    stopRecording,
    pauseRecording,
    resumeRecording,
    checkInputState,
  };
}
