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
  const { width: screenWidth, height: screenHeight } = screen.getPrimaryDisplay().workAreaSize;
  console.log('[Overlay] Creating overlay window, screen:', screenWidth, 'x', screenHeight);

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

  // Enhanced always-on-top logic with fullscreen detection and adaptive layer management
  // This prevents the overlay from being hidden by fullscreen applications
  let isOverlayVisible = false;
  
  const topMostCheckInterval = setInterval(() => {
    if (overlayWindow.isDestroyed()) {
      clearInterval(topMostCheckInterval);
      return;
    }

    try {
      // Get the currently focused window
      const focusedWindow = BrowserWindow.getFocusedWindow();
      
      // Check if overlay is visible
      const overlayVisible = overlayWindow.isVisible();
      
      // Update visibility tracking
      isOverlayVisible = overlayVisible;
      
      // Detect if a fullscreen application (not our overlay) is in focus
      const isForeignFullscreen = focusedWindow && focusedWindow !== overlayWindow && focusedWindow.isFullScreen();
      
      if (isForeignFullscreen) {
        // Fullscreen app detected - use higher layer strategy
        // Try to maintain visibility by re-asserting always-on-top with screen-saver level
        overlayWindow.setAlwaysOnTop(true, 'screen-saver');
        
        // If overlay should be visible but isn't, attempt to restore it
        if (overlayVisible && !overlayWindow.isVisible()) {
          overlayWindow.show();
          overlayWindow.focus();
        }
      } else {
        // Normal window layer - standard always-on-top is sufficient
        overlayWindow.setAlwaysOnTop(true);
      }
    } catch (err) {
      console.error('[Overlay] Error in topmost check:', err);
    }
  }, 1000);  // Check every 1 second for faster response to fullscreen transitions

  // Monitor overlay focus loss and attempt recovery
  overlayWindow.on('blur', () => {
    // When overlay loses focus, ensure it stays on top
    if (!overlayWindow.isDestroyed()) {
      try {
        overlayWindow.setAlwaysOnTop(true);
      } catch (err) {
        console.error('[Overlay] Error restoring always-on-top after blur:', err);
      }
    }
  });

  // Clean up interval when window closes
  overlayWindow.on('closed', () => {
    clearInterval(topMostCheckInterval);
  });

  return overlayWindow;
}
