import { ipcMain, BrowserWindow, shell, dialog, app } from 'electron';
import { RecorderService, RecordingConfig } from '../services/RecorderService';
import fs from 'fs';
import path from 'path';

interface WindowRefs {
  mainWindow: BrowserWindow | null;
  overlayWindow: BrowserWindow | null;
}

// Timeline log file path
const timelineLogPath = path.join(app.getPath('userData'), 'recording_timeline.log');

function writeTimelineLog(message: string): void {
  const timestamp = new Date().toISOString();
  const logLine = `[${timestamp}] ${message}\n`;
  try {
    fs.appendFileSync(timelineLogPath, logLine);
  } catch (e) {
    // Ignore file write errors
  }
  console.log(logLine.trim());
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
    const windows = getWindows();

    writeTimelineLog('===== RECORDING START SEQUENCE =====');
    writeTimelineLog('T0: User clicked start button');

    // Hide main window BEFORE starting recording
    writeTimelineLog('T1: Hiding main window...');
    if (windows.mainWindow && !windows.mainWindow.isDestroyed()) {
      windows.mainWindow.hide();
    }
    writeTimelineLog('T2: Main window hidden, waiting 200ms...');

    // Small delay to ensure window is fully hidden by OS
    await new Promise(resolve => setTimeout(resolve, 200));
    writeTimelineLog('T3: Delay complete, calling recorderService.start()...');

    // Start recording - main window is now hidden
    const serviceStart = Date.now();
    await recorderService.start(config);
    const serviceDuration = Date.now() - serviceStart;
    writeTimelineLog(`T4: recorderService.start() returned (took ${serviceDuration}ms)`);

    // Show overlay after recording started
    writeTimelineLog('T5: Showing overlay window...');
    if (windows.overlayWindow && !windows.overlayWindow.isDestroyed()) {
      windows.overlayWindow.showInactive();
    }
    writeTimelineLog('T6: Overlay window shown');
    writeTimelineLog('===== RECORDING START SEQUENCE COMPLETE =====\n');
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
