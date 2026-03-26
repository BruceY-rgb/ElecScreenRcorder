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

// 系统信息缓存（避免频繁调用超时）
let systemInfoCache: any = null;

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
    console.log('[DEBUG IPC recorder:start] Windows:', !!windows.mainWindow, !!windows.overlayWindow);

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
    let startFailed = false;
    try {
      await recorderService.start(config);
    } catch (err) {
      // 注意：即使启动失败，如果 native core 通过 fallback 实际已经开始录制，
      // 我们仍然需要能够停止它。这里标记一下但继续处理。
      console.log('[IPC] recorderService.start initial error:', err);
      startFailed = true;

      // 检查 native core 的实际状态 - 可能是 ddagrab 失败后 fallback 成功
      try {
        const status = await recorderService.getStatus();
        console.log('[IPC] Native core actual status after start failure:', status);
        if (status.state === 'recording') {
          // Native core 实际在录制，手动设置状态
          recorderService.forceSetState('recording');
          startFailed = false; // 实际上成功了
        }
      } catch (statusErr) {
        console.log('[IPC] Could not get status:', statusErr);
      }
    }
    const serviceDuration = Date.now() - serviceStart;
    writeTimelineLog(`T4: recorderService.start() returned (took ${serviceDuration}ms)`);

    // If recording failed, do NOT show overlay - just restore main window and show error
    if (startFailed) {
      writeTimelineLog('T5: Recording failed, not showing overlay. Restoring main window...');
      // Show error in the main window
      if (windows.mainWindow && !windows.mainWindow.isDestroyed()) {
        windows.mainWindow.webContents.send('recording-error', '无法启动录制，请查看底部日志面板了解原因');
        windows.mainWindow.show();
        windows.mainWindow.focus();
      }
      writeTimelineLog('T5b: Main window restored, overlay NOT shown');
      writeTimelineLog('===== RECORDING START SEQUENCE FAILED =====\n');
      return;
    }

    // Show overlay after recording started
    writeTimelineLog('T5: Showing overlay window...');
    if (windows.overlayWindow && !windows.overlayWindow.isDestroyed()) {
      console.log('[IPC] Overlay window exists, isLoading:', windows.overlayWindow.webContents.isLoading());

      // Check if auto-collapse is enabled
      const prefs = loadPreferences();
      if (prefs.autoCollapseOverlay) {
        windows.overlayWindow.setSize(36, 36);
        windows.overlayWindow.webContents.send('overlay:mode', { mode: 'collapsed' });
      } else {
        windows.overlayWindow.setSize(300, 90);
        windows.overlayWindow.webContents.send('overlay:mode', { mode: 'expanded' });
      }

      // Add small delay to ensure renderer process is ready
      await new Promise(resolve => setTimeout(resolve, 100));

      // Use show() instead of showInactive() to ensure window is displayed
      windows.overlayWindow.show();
      console.log('[IPC] Overlay window shown, isVisible:', windows.overlayWindow.isVisible());
    } else {
      console.log('[IPC] Overlay window NOT available:', !!windows.overlayWindow, windows.overlayWindow?.isDestroyed());
    }
    writeTimelineLog('T6: Overlay window shown');
    writeTimelineLog('===== RECORDING START SEQUENCE COMPLETE =====\n');
  });

  ipcMain.handle('recorder:stop', async () => {
    const windows = getWindows();
    try {
      const result = await recorderService.stop();
      // Recording stopped — restore main window
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
    } catch (err: any) {
      console.log('[IPC] recorder:stop failed:', err.message);
      // 强制设置状态为 idle，让 UI 显示 READY
      recorderService.forceSetState('idle');

      // 检查是否是进程已退出的情况
      const isProcessExited = err.message?.includes('Process exited') ||
                              err.message?.includes('Process closed') ||
                              err.message?.includes('channel');

      if (isProcessExited) {
        // 进程已退出，不需要再停止，直接清理状态
        console.log('[IPC] Native core already exited, cleaning up...');
      }

      // 仍然隐藏悬浮窗
      if (windows.overlayWindow && !windows.overlayWindow.isDestroyed()) {
        windows.overlayWindow.hide();
      }
      // 重新显示主窗口
      if (windows.mainWindow && !windows.mainWindow.isDestroyed()) {
        windows.mainWindow.show();
        windows.mainWindow.focus();
      }
      // 静默处理错误，不抛出
      return null;
    }
  });

  ipcMain.handle('recorder:pause', async () => {
    await recorderService.pause();
  });

  ipcMain.handle('recorder:resume', async () => {
    await recorderService.resume();
  });

  ipcMain.handle('system:info', async () => {
    try {
      // 添加较短的超时，避免长时间阻塞
      const result = await Promise.race([
        recorderService.getSystemInfo(),
        new Promise((resolve) => setTimeout(() => resolve(null), 3000))
      ]);
      // 如果超时，返回缓存的系统信息或默认值
      if (!result) {
        if (systemInfoCache) return systemInfoCache;
        // 返回最小化默认值
        return {
          screenWidth: 1920,
          screenHeight: 1080,
          scalingFactor: 1.0,
          refreshRate: 60,
          cpuName: 'Unknown',
          gpuName: 'Unknown',
          ramGB: 8,
          mousePollingRate: 125
        };
      }
      // 缓存成功的系统信息
      systemInfoCache = result;
      return result;
    } catch (err) {
      const windows = getWindows();
      const msg = err instanceof Error ? err.message : String(err);
      // 不再发送错误到前端，避免日志噪音
      console.error(`[system:info] 获取失败: ${msg}`);
      // 返回缓存或默认值
      if (systemInfoCache) return systemInfoCache;
      return {
        screenWidth: 1920,
        screenHeight: 1080,
        scalingFactor: 1.0,
        refreshRate: 60,
        cpuName: 'Unknown',
        gpuName: 'Unknown',
        ramGB: 8,
        mousePollingRate: 125
      };
    }
  });

  ipcMain.handle('recorder:checkInput', async () => {
    return await recorderService.checkInputState();
  });

  // Get available audio input devices (microphones)
  ipcMain.handle('recorder:getAudioDevices', async () => {
    try {
      return await recorderService.getAudioDevices();
    } catch (err) {
      const windows = getWindows();
      const msg = err instanceof Error ? err.message : String(err);
      windows.mainWindow?.webContents.send('native:log', {
        level: 'ERROR',
        message: `getAudioDevices 失败: ${msg}`,
      });
      throw err;
    }
  });
}
