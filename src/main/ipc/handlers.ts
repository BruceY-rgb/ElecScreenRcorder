import { ipcMain, BrowserWindow, shell, dialog, app } from 'electron';
import { RecorderService, RecordingConfig } from '../services/RecorderService';
import fs from 'fs';
import path from 'path';

interface WindowRefs {
  mainWindow: BrowserWindow | null;
  overlayWindow: BrowserWindow | null;
}

// Simple preference storage
const prefsPath = path.join(app.getPath('userData'), 'preferences.json');

function loadPreferences(): { defaultSavePath: string } {
  try {
    if (fs.existsSync(prefsPath)) {
      return JSON.parse(fs.readFileSync(prefsPath, 'utf-8'));
    }
  } catch (e) {
    console.error('[Preferences] Failed to load:', e);
  }
  return { defaultSavePath: '' };
}

function savePreferences(prefs: { defaultSavePath: string }): void {
  try {
    fs.writeFileSync(prefsPath, JSON.stringify(prefs, null, 2));
  } catch (e) {
    console.error('[Preferences] Failed to save:', e);
  }
}

export function registerHandlers(recorderService: RecorderService, getWindows: () => WindowRefs): void {
  // Select save directory dialog
  ipcMain.handle('dialog:selectDirectory', async (): Promise<string | null> => {
    const prefs = loadPreferences();
    const result = await dialog.showOpenDialog({
      defaultPath: prefs.defaultSavePath || app.getPath('videos'),
      properties: ['openDirectory', 'createDirectory']
    });
    return result.canceled ? null : result.filePaths[0];
  });

  // Get default save path
  ipcMain.handle('dialog:getDefaultPath', async (): Promise<string> => {
    const prefs = loadPreferences();
    return prefs.defaultSavePath || app.getPath('videos');
  });

  // Set default save path
  ipcMain.handle('dialog:setDefaultPath', async (_, savePath: string): Promise<void> => {
    const prefs = loadPreferences();
    prefs.defaultSavePath = savePath;
    savePreferences(prefs);
  });

  // Open video file in default player, ensuring it appears on top
  ipcMain.handle('recorder:openVideo', async (_, videoPath: string): Promise<void> => {
    if (videoPath) {
      // Blur our window so the OS gives focus to the newly opened player
      const windows = getWindows();
      if (windows.mainWindow && !windows.mainWindow.isDestroyed()) {
        windows.mainWindow.blur();
      }
      await shell.openPath(videoPath);
    }
  });
  ipcMain.handle('recorder:start', async (_, config: RecordingConfig): Promise<void> => {
    await recorderService.start(config);
    // Recording started successfully — switch to overlay mode
    const windows = getWindows();
    console.log('[IPC] recorder:start success, switching to overlay. mainWindow:', !!windows.mainWindow, 'overlayWindow:', !!windows.overlayWindow);
    if (windows.mainWindow && !windows.mainWindow.isDestroyed()) {
      windows.mainWindow.hide();
    }
    if (windows.overlayWindow && !windows.overlayWindow.isDestroyed()) {
      const bounds = windows.overlayWindow.getBounds();
      console.log('[IPC] overlay bounds:', JSON.stringify(bounds));
      console.log('[IPC] overlay isVisible before show:', windows.overlayWindow.isVisible());
      windows.overlayWindow.showInactive();
      console.log('[IPC] overlay isVisible after show:', windows.overlayWindow.isVisible());
    }
  });

  ipcMain.handle('recorder:stop', async () => {
    const result = await recorderService.stop();
    // Recording stopped — restore main window
    const windows = getWindows();
    console.log('[IPC] recorder:stop success, restoring main window');
    if (windows.overlayWindow && !windows.overlayWindow.isDestroyed()) {
      windows.overlayWindow.hide();
    }
    if (windows.mainWindow && !windows.mainWindow.isDestroyed()) {
      // Send recording-finished event before showing the window so the
      // renderer has the result ready when it becomes visible.
      windows.mainWindow.webContents.send('recording-finished', result);
      windows.mainWindow.show();
      windows.mainWindow.focus();
    }
    return result;
  });

  ipcMain.handle('recorder:pause', async () => {
    await recorderService.pause();
  });

  ipcMain.handle('recorder:resume', async () => {
    await recorderService.resume();
  });

  ipcMain.handle('system:info', async () => {
    return await recorderService.getSystemInfo();
  });

  ipcMain.handle('recorder:checkInput', async () => {
    return await recorderService.checkInputState();
  });
}
