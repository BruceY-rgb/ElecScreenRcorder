import { app, BrowserWindow } from 'electron';
import path from 'path';
import fs from 'fs';
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
  if (recorderService) {
    recorderService.destroy();
  }
});
