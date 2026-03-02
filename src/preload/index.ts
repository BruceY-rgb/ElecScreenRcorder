import { contextBridge, ipcRenderer } from 'electron';

export interface RecordingConfig {
  resolution: { width: number; height: number };
  fps: number;
  savePath: string;
  separateAudio: boolean;
  remuxToMp4: boolean;
}

export interface RecordingStatus {
  state: 'idle' | 'recording' | 'paused';
  duration: number;
  frameCount: number;
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
}

const electronAPI = {
  startRecording: (config: RecordingConfig): Promise<void> =>
    ipcRenderer.invoke('recorder:start', config),

  stopRecording: (): Promise<FinishResult> =>
    ipcRenderer.invoke('recorder:stop'),

  pauseRecording: (): Promise<void> =>
    ipcRenderer.invoke('recorder:pause'),

  resumeRecording: (): Promise<void> =>
    ipcRenderer.invoke('recorder:resume'),

  getSystemInfo: (): Promise<SystemInfo> =>
    ipcRenderer.invoke('system:info'),

  onRecordingStatus: (callback: (status: RecordingStatus) => void) => {
    ipcRenderer.on('recording-status', (_, data) => callback(data));
  },

  onRecordingError: (callback: (error: string) => void) => {
    ipcRenderer.on('recording-error', (_, data) => callback(data));
  },

  onRecordingFinish: (callback: (result: FinishResult) => void) => {
    ipcRenderer.on('recording-finished', (_, data) => callback(data));
  },
};

contextBridge.exposeInMainWorld('electronAPI', electronAPI);

declare global {
  interface Window {
    electronAPI: typeof electronAPI;
  }
}
