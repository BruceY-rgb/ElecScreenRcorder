import { app, BrowserWindow, globalShortcut, ipcMain } from 'electron';
import path from 'path';
import fs from 'fs';
import { spawn } from 'child_process';
import { registerHandlers } from './ipc/handlers';
import { RecorderService, RecorderConfig } from './services/RecorderService';
import { createMainWindow, createOverlayWindow } from './windows/mainWindow';

// Add FFmpeg DLL directory to PATH at app startup
function setupFFmpegPath() {
  const ffmpegDir = app.isPackaged
    ? path.join(process.resourcesPath, 'native/dist')
    : path.join(__dirname, '../../native/dist');

  // Add to process PATH (for current process)
  const currentPath = process.env.PATH || '';
  if (!currentPath.includes(ffmpegDir)) {
    process.env.PATH = ffmpegDir + path.delimiter + currentPath;
    console.log(`[Main] Added FFmpeg path to PATH: ${ffmpegDir}`);
  }
}

// Call early before any other initialization
setupFFmpegPath();

let mainWindow: BrowserWindow | null = null;
let overlayWindow: BrowserWindow | null = null;
let recorderService: RecorderService | null = null;

// Preferences path
const prefsPath = path.join(app.getPath('userData'), 'preferences.json');

interface Preferences {
  defaultSavePath: string;
  autoCollapseOverlay?: boolean;
  hotkeys?: {
    start?: string;
    pause?: string;
    stop?: string;
  };
}

function loadPreferences(): Preferences {
  try {
    if (fs.existsSync(prefsPath)) {
      return JSON.parse(fs.readFileSync(prefsPath, 'utf-8'));
    }
  } catch (e) {
    console.error('[Preferences] Failed to load:', e);
  }
  return { defaultSavePath: '' };
}

// Register global shortcuts
function registerGlobalShortcuts() {
  const prefs = loadPreferences();
  const hotkeys = prefs.hotkeys || { start: 'F9', pause: 'F10', stop: 'F11' };

  // Unregister all first
  globalShortcut.unregisterAll();

  // Register start hotkey
  if (hotkeys.start) {
    try {
      const success = globalShortcut.register(hotkeys.start, () => {
        console.log('[Hotkey] Start pressed');
        if (mainWindow && !mainWindow.isDestroyed()) {
          mainWindow.webContents.send('hotkey:pressed', 'start');
        }
      });
      if (success) {
        console.log(`[Hotkey] Registered start: ${hotkeys.start}`);
      } else {
        console.warn(`[Hotkey] Failed to register start: ${hotkeys.start}`);
      }
    } catch (e) {
      console.warn(`[Hotkey] Error registering start: ${e}`);
    }
  }

  // Register pause hotkey
  if (hotkeys.pause) {
    try {
      const success = globalShortcut.register(hotkeys.pause, () => {
        console.log('[Hotkey] Pause pressed');
        if (mainWindow && !mainWindow.isDestroyed()) {
          mainWindow.webContents.send('hotkey:pressed', 'pause');
        }
      });
      if (success) {
        console.log(`[Hotkey] Registered pause: ${hotkeys.pause}`);
      } else {
        console.warn(`[Hotkey] Failed to register pause: ${hotkeys.pause}`);
      }
    } catch (e) {
      console.warn(`[Hotkey] Error registering pause: ${e}`);
    }
  }

  // Register stop hotkey
  if (hotkeys.stop) {
    try {
      const success = globalShortcut.register(hotkeys.stop, () => {
        console.log('[Hotkey] Stop pressed');
        if (mainWindow && !mainWindow.isDestroyed()) {
          mainWindow.webContents.send('hotkey:pressed', 'stop');
        }
      });
      if (success) {
        console.log(`[Hotkey] Registered stop: ${hotkeys.stop}`);
      } else {
        console.warn(`[Hotkey] Failed to register stop: ${hotkeys.stop}`);
      }
    } catch (e) {
      console.warn(`[Hotkey] Error registering stop: ${e}`);
    }
  }
}

// Handle hotkey updates from renderer
function setupHotkeyIPC() {
  ipcMain.on('hotkeys:register', () => {
    console.log('[Hotkey] Re-registering shortcuts...');
    registerGlobalShortcuts();
  });
}

// Load configuration from environment or config file
function loadConfig(): RecorderConfig {
  // Default: local mode
  let mode: 'local' | 'remote' = 'local';
  let nativeCorePath: string | undefined;
  let remoteHost: string | undefined;
  let remotePort: number | undefined;

  // Check environment variables
  const envMode = process.env.RECORDER_MODE;
  if (envMode === 'remote') {
    mode = 'remote';
    remoteHost = process.env.RECORDER_HOST || 'localhost';
    remotePort = parseInt(process.env.RECORDER_PORT || '8765', 10);
  } else {
    // Local mode - get native core path
    nativeCorePath = path.join(
      app.isPackaged
        ? path.join(process.resourcesPath, 'native/dist')
        : path.join(__dirname, '../../native/dist'),
      'recorder_core.exe'
    );
  }

  // Also check for .env file in project root
  const envFile = path.join(process.cwd(), '.env');
  if (fs.existsSync(envFile)) {
    const envContent = fs.readFileSync(envFile, 'utf-8');
    const envVars: Record<string, string> = {};

    envContent.split('\n').forEach(line => {
      const match = line.match(/^([^=]+)=(.*)$/);
      if (match) {
        envVars[match[1].trim()] = match[2].trim();
      }
    });

    // Override with .env file values
    if (envVars.RECORDER_MODE === 'remote') {
      mode = 'remote';
      remoteHost = envVars.RECORDER_HOST || remoteHost || 'localhost';
      remotePort = parseInt(envVars.RECORDER_PORT || String(remotePort || 8765), 10);
    } else if (envVars.RECORDER_MODE === 'local' || !envVars.RECORDER_MODE) {
      mode = 'local';
      if (!nativeCorePath && envVars.RECORDER_CORE_PATH) {
        nativeCorePath = envVars.RECORDER_CORE_PATH;
      } else if (!nativeCorePath) {
        nativeCorePath = path.join(
          app.isPackaged
            ? path.join(process.resourcesPath, 'native/dist')
            : path.join(__dirname, '../../native/dist'),
          'recorder_core.exe'
        );
      }
    }
  }

  console.log(`[Main] Recorder mode: ${mode}`);
  if (mode === 'remote') {
    console.log(`[Main] Remote host: ${remoteHost}:${remotePort}`);
  } else {
    console.log(`[Main] Native core path: ${nativeCorePath}`);
  }

  return {
    mode,
    nativeCorePath,
    remoteHost,
    remotePort,
  };
}

async function createWindows() {
  // Create main window
  mainWindow = createMainWindow();

  // Create overlay window (hidden initially, shown when recording starts)
  overlayWindow = createOverlayWindow();

  // Load configuration
  const config = loadConfig();

  // Initialize recorder service
  recorderService = new RecorderService(config);

  // Register IPC handlers with window getter (always returns current references)
  registerHandlers(recorderService, () => ({ mainWindow, overlayWindow }));

  // Setup hotkey IPC and register shortcuts
  setupHotkeyIPC();
  registerGlobalShortcuts();

  // Handle window close
  mainWindow.on('closed', () => {
    mainWindow = null;
    if (overlayWindow) {
      overlayWindow.close();
    }
  });

  overlayWindow.on('closed', () => {
    overlayWindow = null;
  });
}

// App lifecycle
app.whenReady().then(createWindows);

app.on('window-all-closed', () => {
  // Cleanup recorder service
  if (recorderService) {
    recorderService.destroy();
  }

  if (process.platform !== 'darwin') {
    app.quit();
  }
});

app.on('activate', () => {
  if (BrowserWindow.getAllWindows().length === 0) {
    createWindows();
  }
});

app.on('before-quit', () => {
  // Unregister all global shortcuts
  globalShortcut.unregisterAll();

  if (recorderService) {
    recorderService.destroy();
  }
});
