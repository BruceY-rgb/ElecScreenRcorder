import { ipcMain } from 'electron';
import { RecorderService, RecordingConfig } from '../services/RecorderService';

export function registerHandlers(recorderService: RecorderService): void {
  ipcMain.handle('recorder:start', async (_, config: RecordingConfig): Promise<void> => {
    await recorderService.start(config);
  });

  ipcMain.handle('recorder:stop', async () => {
    return await recorderService.stop();
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
}
