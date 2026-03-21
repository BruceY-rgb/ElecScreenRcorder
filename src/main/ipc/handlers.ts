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

function savePreferences(prefs: Preferences): void {
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

  // Get preferences
  ipcMain.handle('preferences:get', async (): Promise<Preferences> => {
    return loadPreferences();
  });

  // Set preferences
  ipcMain.handle('preferences:set', async (_, prefs: Partial<Preferences>): Promise<void> => {
    const current = loadPreferences();
    savePreferences({ ...current, ...prefs });
  });

  // Get hotkeys
  ipcMain.handle('hotkeys:get', async (): Promise<Record<string, string>> => {
    const prefs = loadPreferences();
    return prefs.hotkeys || { start: 'F9', pause: 'F10', stop: 'F11' };
  });

  // Set hotkeys
  ipcMain.handle('hotkeys:set', async (_, hotkeys: Record<string, string>): Promise<void> => {
    const prefs = loadPreferences();
    prefs.hotkeys = hotkeys;
    savePreferences(prefs);
  });

  // Overlay mode control (expanded/collapsed)
  ipcMain.handle('overlay:setMode', async (_, mode: 'expanded' | 'collapsed'): Promise<void> => {
    const windows = getWindows();
    if (windows.overlayWindow && !windows.overlayWindow.isDestroyed()) {
      if (mode === 'collapsed') {
        windows.overlayWindow.setSize(36, 36);
      } else {
        windows.overlayWindow.setSize(300, 90);
      }
      // Notify overlay of mode change
      windows.overlayWindow.webContents.send('overlay:mode', { mode });
    }
  });

  // Overlay hover expand/collapse (36x36 orb <-> 300x90 full overlay)
  ipcMain.handle('overlay:setHoverSize', async (_, expanded: boolean): Promise<void> => {
    const windows = getWindows();
    if (windows.overlayWindow && !windows.overlayWindow.isDestroyed()) {
      const [x, y] = windows.overlayWindow.getPosition();
      if (expanded) {
        // Expand leftward to full 300x90 overlay
        windows.overlayWindow.setBounds({ x: x - (300 - 36), y, width: 300, height: 90 });
      } else {
        // Collapse back to 36x36 orb
        windows.overlayWindow.setBounds({ x: x + (300 - 36), y, width: 36, height: 36 });
      }
    }
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

    // Debug: log config received
    console.log('[DEBUG IPC recorder:start] Resolution:', config.resolution);

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
      // Check if auto-collapse is enabled
      const prefs = loadPreferences();
      if (prefs.autoCollapseOverlay) {
        windows.overlayWindow.setSize(36, 36);
        windows.overlayWindow.webContents.send('overlay:mode', { mode: 'collapsed' });
      } else {
        windows.overlayWindow.setSize(300, 90);
        windows.overlayWindow.webContents.send('overlay:mode', { mode: 'expanded' });
      }
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

  // Get available audio input devices (microphones)
  ipcMain.handle('recorder:getAudioDevices', async () => {
    return await recorderService.getAudioDevices();
  });
}
