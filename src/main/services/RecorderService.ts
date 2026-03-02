import { spawn, ChildProcess } from 'child_process';
import { BrowserWindow } from 'electron';
import readline from 'readline';
import path from 'path';

export interface RecordingConfig {
  resolution: { width: number; height: number };
  fps: number;
  savePath: string;
  separateAudio: boolean;
  remuxToMp4: boolean;
}

export interface RecordingStatus {
  state: 'idle' | 'recording' | 'paused';
  duration: number;
  frameCount: number;
}

export interface SystemInfo {
  screenWidth: number;
  screenHeight: number;
  scalingFactor: number;
  refreshRate: number;
  cpuName: string;
  gpuName: string;
  ramGB: number;
  mousePollingRate: number;
}

export interface FinishResult {
  videoPath: string;
  actionsPath: string;
  movementsPath: string;
  duration: number;
}

type RecordingState = 'idle' | 'recording' | 'paused';

export class RecorderService {
  private child: ChildProcess | null = null;
  private nativeCorePath: string;
  private state: RecordingState = 'idle';
  private recordingStartTime: number = 0;
  private duration: number = 0;
  private frameCount: number = 0;

  constructor(nativeCorePath: string) {
    this.nativeCorePath = nativeCorePath;
  }

  async start(config: RecordingConfig): Promise<void> {
    if (this.state !== 'idle') {
      throw new Error(`Cannot start recording: already ${this.state}`);
    }

    this.ensureNativeCore();
    if (!this.child) {
      throw new Error('Failed to start native core');
    }

    const command = {
      action: 'start',
      config: {
        width: config.resolution.width,
        height: config.resolution.height,
        fps: config.fps,
        savePath: config.savePath,
        separateAudio: config.separateAudio,
        remuxToMp4: config.remuxToMp4,
      },
    };

    this.child.stdin?.write(JSON.stringify(command) + '\n');
    this.state = 'recording';
    this.recordingStartTime = Date.now();
    this.duration = 0;
    this.frameCount = 0;

    this.broadcastStatus();
  }

  async stop(): Promise<FinishResult> {
    if (this.state === 'idle') {
      throw new Error('Cannot stop: not recording');
    }

    if (!this.child) {
      throw new Error('Native core not running');
    }

    const command = { action: 'stop' };
    this.child.stdin?.write(JSON.stringify(command) + '\n');

    // Wait for finish response (handled by on('line'))

    this.state = 'idle';
    this.broadcastStatus();

    // Return placeholder - actual result comes via message
    return {
      videoPath: '',
      actionsPath: '',
      movementsPath: '',
      duration: this.duration,
    };
  }

  async pause(): Promise<void> {
    if (this.state !== 'recording') {
      throw new Error(`Cannot pause: state is ${this.state}`);
    }

    this.child?.stdin?.write(JSON.stringify({ action: 'pause' }) + '\n');
    this.state = 'paused';
    this.broadcastStatus();
  }

  async resume(): Promise<void> {
    if (this.state !== 'paused') {
      throw new Error(`Cannot resume: state is ${this.state}`);
    }

    this.child?.stdin?.write(JSON.stringify({ action: 'resume' }) + '\n');
    this.state = 'recording';
    this.broadcastStatus();
  }

  async getSystemInfo(): Promise<SystemInfo> {
    this.ensureNativeCore();

    return new Promise((resolve, reject) => {
      if (!this.child) {
        reject(new Error('Native core not running'));
        return;
      }

      const command = { action: 'sysinfo' };
      this.child.stdin?.write(JSON.stringify(command) + '\n');

      // Handle response via message handler
      const timeout = setTimeout(() => {
        reject(new Error('System info request timeout'));
      }, 5000);

      const handler = (line: string) => {
        try {
          const msg = JSON.parse(line);
          if (msg.type === 'sysinfo' && msg.data) {
            clearTimeout(timeout);
            this.child?.stdout?.off('line', handler);
            resolve(msg.data);
          }
        } catch {
          // Ignore parse errors
        }
      };

      this.child.stdout?.on('line', handler);
    });
  }

  private ensureNativeCore(): void {
    if (this.child) {
      return;
    }

    this.child = spawn(this.nativeCorePath, [], {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    });

    // Setup stdout listener
    const rl = readline.createInterface({
      input: this.child.stdout!,
    });

    rl.on('line', (line) => {
      this.handleMessage(line);
    });

    // Setup stderr listener
    this.child.stderr?.on('data', (data) => {
      console.error('[Native Core]', data.toString());
    });

    this.child.on('exit', (code) => {
      console.log(`Native core exited with code ${code}`);
      this.child = null;
      this.state = 'idle';
      this.broadcastStatus();
    });
  }

  private handleMessage(line: string): void {
    try {
      const msg = JSON.parse(line);

      switch (msg.type) {
        case 'status':
          this.state = msg.state;
          this.broadcastStatus();
          break;
        case 'error':
          this.broadcastError(msg.msg);
          break;
        case 'finish':
          this.broadcastFinish(msg);
          break;
      }
    } catch {
      // Ignore parse errors
    }
  }

  private broadcastStatus(): void {
    const status: RecordingStatus = {
      state: this.state,
      duration: this.duration,
      frameCount: this.frameCount,
    };

    BrowserWindow.getAllWindows().forEach((win) => {
      win.webContents.send('recording-status', status);
    });
  }

  private broadcastError(error: string): void {
    BrowserWindow.getAllWindows().forEach((win) => {
      win.webContents.send('recording-error', error);
    });
  }

  private broadcastFinish(data: any): void {
    const result: FinishResult = {
      videoPath: data.videoPath || '',
      actionsPath: data.actionsPath || '',
      movementsPath: data.movementsPath || '',
      duration: data.duration || 0,
    };

    BrowserWindow.getAllWindows().forEach((win) => {
      win.webContents.send('recording-finished', result);
    });
  }

  destroy(): void {
    if (this.child) {
      this.child.stdin?.write(JSON.stringify({ action: 'quit' }) + '\n');

      // Force kill after timeout
      setTimeout(() => {
        if (this.child) {
          this.child.kill();
          this.child = null;
        }
      }, 2000);
    }
  }
}
