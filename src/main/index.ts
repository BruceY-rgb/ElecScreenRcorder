import { app, BrowserWindow } from 'electron';
import path from 'path';
import { registerHandlers } from './ipc/handlers';
import { RecorderService } from './services/RecorderService';
import { createMainWindow, createOverlayWindow } from './windows/mainWindow';

let mainWindow: BrowserWindow | null = null;
let overlayWindow: BrowserWindow | null = null;
let recorderService: RecorderService | null = null;

async function createWindows() {
  // Create main window
  mainWindow = createMainWindow();

  // Create overlay window
  overlayWindow = createOverlayWindow();

  // Initialize recorder service
  const nativeCorePath = path.join(
    app.isPackaged
      ? path.join(process.resourcesPath, 'native/dist')
      : path.join(__dirname, '../../native/dist'),
    'recorder_core.exe'
  );

  recorderService = new RecorderService(nativeCorePath);

  // Register IPC handlers
  registerHandlers(recorderService);

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
