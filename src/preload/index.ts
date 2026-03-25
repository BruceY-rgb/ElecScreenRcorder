import { contextBridge, ipcRenderer } from 'electron';

export interface RecordingConfig {
  resolution: { width: number; height: number };
  fps: number;
  bitrate?: number;
  savePath: string;
  separateAudio: boolean;
  remuxToMp4: boolean;
  captureAudio?: boolean;
  captureMicrophone?: boolean;
  microphoneDevice?: string;
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
    pressedKeys?: string[];
    pressedMouseButtons?: number[];
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
  micAudioPath: string;
  duration: number;
  recordingFolder: string;
}

export interface Preferences {
  defaultSavePath: string;
  autoCollapseOverlay?: boolean;
  hotkeys?: {
    start?: string;
    pause?: string;
    stop?: string;
  };
}

export interface InputState {
  anyKeyPressed: boolean;
  mouseButtonPressed: boolean;
  pressedKeyCount: number;
  pressedKeys?: string[];
  pressedMouseButtons?: number[];
}

export interface NativeLogEntry {
  level: string;
  message: string;
}

const electronAPI = {
  selectSaveDirectory: (): Promise<string | null> =>
    ipcRenderer.invoke('dialog:selectDirectory'),

  getDefaultSavePath: (): Promise<string> =>
    ipcRenderer.invoke('dialog:getDefaultPath'),

  setDefaultSavePath: (savePath: string): Promise<void> =>
    ipcRenderer.invoke('dialog:setDefaultPath', savePath),

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

  checkInputState: (): Promise<InputState> =>
    ipcRenderer.invoke('recorder:checkInput'),

  getAudioDevices: (): Promise<string[]> =>
    ipcRenderer.invoke('recorder:getAudioDevices'),

  openVideo: (videoPath: string): Promise<void> =>
    ipcRenderer.invoke('recorder:openVideo', videoPath),

  // Preferences
  getPreferences: (): Promise<Preferences> =>
    ipcRenderer.invoke('preferences:get'),

  setPreferences: (prefs: Partial<Preferences>): Promise<void> =>
    ipcRenderer.invoke('preferences:set', prefs),

  // Hotkeys
  getHotkeys: (): Promise<Record<string, string>> =>
    ipcRenderer.invoke('hotkeys:get'),

  setHotkeys: (hotkeys: Record<string, string>): Promise<void> =>
    ipcRenderer.invoke('hotkeys:set', hotkeys),

  reRegisterHotkeys: (): void =>
    ipcRenderer.send('hotkeys:register'),

  // Overlay mode control
  setOverlayMode: (mode: 'expanded' | 'collapsed'): Promise<void> =>
    ipcRenderer.invoke('overlay:setMode', mode),

  setOverlayHoverSize: (expanded: boolean): Promise<void> =>
    ipcRenderer.invoke('overlay:setHoverSize', expanded),

  onOverlayMode: (callback: (data: { mode: 'expanded' | 'collapsed' }) => void) => {
    ipcRenderer.on('overlay:mode', (_, data) => callback(data));
  },

  onRecordingStatus: (callback: (status: RecordingStatus) => void) => {
    ipcRenderer.on('recording-status', (_, data) => callback(data));
  },

  onRecordingError: (callback: (error: string) => void) => {
    ipcRenderer.on('recording-error', (_, data) => callback(data));
  },

  onRecordingFinish: (callback: (result: FinishResult) => void) => {
    ipcRenderer.on('recording-finished', (_, data) => callback(data));
  },

  // Hotkey events
  onHotkeyPressed: (callback: (action: string) => void) => {
    ipcRenderer.on('hotkey:pressed', (_, action) => callback(action));
  },

  // Native log stream (native core stderr output)
  onNativeLog: (callback: (data: { level: string; message: string }) => void) => {
    ipcRenderer.on('native:log', (_, data) => callback(data));
  },
};

contextBridge.exposeInMainWorld('electronAPI', electronAPI);

declare global {
  interface Window {
    electronAPI: typeof electronAPI;
  }
}
