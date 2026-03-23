import { app, BrowserWindow, screen } from 'electron';
import path from 'path';

function getIconPath(): string {
  return app.isPackaged
    ? path.join(process.resourcesPath, 'icon.png')
    : path.join(__dirname, '../../build/icon.png');
}

export function createMainWindow(): BrowserWindow {
  const { width, height } = screen.getPrimaryDisplay().workAreaSize;

  const mainWindow = new BrowserWindow({
    width: Math.min(1200, width * 0.8),
    height: Math.min(800, height * 0.8),
    minWidth: 800,
    minHeight: 600,
    title: 'ScreenCraft',
    icon: getIconPath(),
    webPreferences: {
      preload: path.join(__dirname, '../preload/index.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });

  // Load the app
  if (process.env.VITE_DEV_SERVER_URL) {
    mainWindow.loadURL(process.env.VITE_DEV_SERVER_URL);
    mainWindow.webContents.openDevTools();
  } else {
    mainWindow.loadFile(path.join(__dirname, '../renderer/index.html'));
  }

  return mainWindow;
}

export function createOverlayWindow(): BrowserWindow {
  const { width: screenWidth } = screen.getPrimaryDisplay().workAreaSize;

  const overlayWidth = 300;
  const overlayHeight = 90;

  const overlayWindow = new BrowserWindow({
    width: overlayWidth,
    height: overlayHeight,
    x: screenWidth - overlayWidth - 20,
    y: 20,
    frame: false,
    transparent: true,
    alwaysOnTop: true,
    skipTaskbar: true,
    resizable: true,
    movable: true,
    minimizable: false,
    maximizable: false,
    show: false,
    webPreferences: {
      preload: path.join(__dirname, '../preload/index.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  });

  // Make window draggable via CSS -webkit-app-region

  // Load the overlay
  if (process.env.VITE_DEV_SERVER_URL) {
    overlayWindow.loadURL(process.env.VITE_DEV_SERVER_URL + '/overlay.html');
    overlayWindow.webContents.openDevTools({ mode: 'detach' });
  } else {
    overlayWindow.loadFile(path.join(__dirname, '../renderer/overlay.html'));
  }

  // Exclude overlay from screen capture using Electron's built-in API.
  // setContentProtection(true) calls SetWindowDisplayAffinity(WDA_MONITOR) internally.
  // Combined with ddagrab (DXGI Desktop Duplication), the overlay becomes invisible in recordings.
  // This is more reliable than the PowerShell approach which fails with ACCESS_DENIED on layered windows.
  overlayWindow.once('ready-to-show', () => {
    try {
      overlayWindow.setContentProtection(true);
      console.log('[Overlay] Content protection enabled (setContentProtection)');
    } catch (err) {
      console.error('[Overlay] setContentProtection failed:', err);
    }
  });

  return overlayWindow;
}
