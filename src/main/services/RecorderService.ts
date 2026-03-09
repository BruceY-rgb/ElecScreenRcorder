import { spawn, ChildProcess } from 'child_process';
import { BrowserWindow, app } from 'electron';
import readline from 'readline';
import path from 'path';
import fs from 'fs';

export interface RecordingConfig {
  resolution: { width: number; height: number };
  fps: number;
  savePath: string;
  separateAudio: boolean;
  remuxToMp4: boolean;
}

export interface RecordingStatus {
  state: 'idle' | 'recording' | 'paused' | 'reconnecting';
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

type RecordingState = 'idle' | 'recording' | 'paused' | 'reconnecting';

// Heartbeat configuration
const HEARTBEAT_INTERVAL = 3000; // 3 seconds
const HEARTBEAT_TIMEOUT = 5000;  // 5 seconds timeout
const RESTART_DELAY = 3000;       // 3 seconds before restart

export class RecorderService {
  private child: ChildProcess | null = null;
  private nativeCorePath: string;
  private state: RecordingState = 'idle';
  private recordingStartTime: number = 0;
  private duration: number = 0;
  private frameCount: number = 0;

  // Heartbeat tracking
  private lastHeartbeat: number = 0;
  private heartbeatTimer: NodeJS.Timeout | null = null;
  private isRestarting: boolean = false;

  // Logging
  private logPath: string;
  private logStream: fs.WriteStream | null = null;

  constructor(nativeCorePath: string) {
    this.nativeCorePath = nativeCorePath;
    this.setupLogging();
  }

  private setupLogging(): void {
    const userDataPath = app.getPath('userData');
    const logsDir = path.join(userDataPath, 'logs');

    // Ensure logs directory exists
    if (!fs.existsSync(logsDir)) {
      fs.mkdirSync(logsDir, { recursive: true });
    }

    const timestamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    this.logPath = path.join(logsDir, `recorder_${timestamp}.log`);
    this.logStream = fs.createWriteStream(this.logPath, { flags: 'a' });

    this.log('INFO', 'RecorderService initialized');
  }

  private log(level: string, message: string): void {
    const timestamp = new Date().toISOString();
    const logMessage = `[${timestamp}] [${level}] ${message}\n`;

    // Write to file
    this.logStream?.write(logMessage);

    // Also write to console
    if (level === 'ERROR') {
      console.error(logMessage);
    } else {
      console.log(logMessage);
    }
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

    this.log('INFO', `Recording started: ${config.resolution.width}x${config.resolution.height} @ ${config.fps}fps`);
    this.broadcastStatus();
    this.startHeartbeat();
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

    this.state = 'idle';
    this.stopHeartbeat();
    this.log('INFO', 'Recording stopped');
    this.broadcastStatus();

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
    this.log('INFO', 'Recording paused');
    this.broadcastStatus();
  }

  async resume(): Promise<void> {
    if (this.state !== 'paused') {
      throw new Error(`Cannot resume: state is ${this.state}`);
    }

    this.child?.stdin?.write(JSON.stringify({ action: 'resume' }) + '\n');
    this.state = 'recording';
    this.log('INFO', 'Recording resumed');
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

    this.log('INFO', `Starting native core: ${this.nativeCorePath}`);

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

    // Setup stderr listener - capture to log file
    this.child.stderr?.on('data', (data) => {
      const message = data.toString().trim();
      if (message) {
        this.log('ERROR', `[Native Core] ${message}`);
      }
    });

    this.child.on('exit', (code, signal) => {
      this.log('INFO', `Native core exited with code ${code}, signal ${signal}`);
      this.handleProcessExit(code, signal);
    });

    this.child.on('error', (err) => {
      this.log('ERROR', `Native core error: ${err.message}`);
      this.handleProcessError(err);
    });
  }

  private handleProcessExit(code: number | null, signal: string | null): void {
    const wasRecording = this.state === 'recording' || this.state === 'paused';
    this.child = null;

    if (wasRecording && !this.isRestarting) {
      // Recording was in progress, trigger restart
      this.state = 'reconnecting';
      this.broadcastStatus();
      this.log('ERROR', 'Native core crashed during recording, attempting restart...');

      // Notify UI
      BrowserWindow.getAllWindows().forEach((win) => {
        win.webContents.send('recording-error', '录制进程异常退出，正在尝试重启...');
      });

      // Schedule restart
      setTimeout(() => {
        this.attemptRestart();
      }, RESTART_DELAY);
    } else {
      this.state = 'idle';
    }

    this.stopHeartbeat();
    this.broadcastStatus();
  }

  private handleProcessError(err: Error): void {
    this.log('ERROR', `Process error: ${err.message}`);

    if (this.state === 'recording' || this.state === 'paused') {
      this.state = 'reconnecting';
      this.broadcastStatus();

      setTimeout(() => {
        this.attemptRestart();
      }, RESTART_DELAY);
    }
  }

  private attemptRestart(): void {
    if (this.isRestarting) {
      return;
    }

    this.isRestarting = true;
    this.log('INFO', 'Attempting to restart native core...');

    try {
      this.ensureNativeCore();

      // Give it time to initialize
      setTimeout(() => {
        this.isRestarting = false;

        if (this.child) {
          this.log('INFO', 'Native core restarted successfully');
          // Send status query to verify it's responsive
          this.child.stdin?.write(JSON.stringify({ action: 'status' }) + '\n');
        } else {
          this.log('ERROR', 'Failed to restart native core');
          this.state = 'idle';
          this.broadcastStatus();
        }
      }, 2000);
    } catch (err) {
      this.isRestarting = false;
      this.log('ERROR', `Restart failed: ${(err as Error).message}`);
      this.state = 'idle';
      this.broadcastStatus();
    }
  }

  // Heartbeat system
  private startHeartbeat(): void {
    this.lastHeartbeat = Date.now();

    this.heartbeatTimer = setInterval(() => {
      this.checkHeartbeat();
    }, HEARTBEAT_INTERVAL);

    this.log('INFO', 'Heartbeat started');
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = null;
      this.log('INFO', 'Heartbeat stopped');
    }
  }

  private checkHeartbeat(): void {
    const now = Date.now();
    const elapsed = now - this.lastHeartbeat;

    if (elapsed > HEARTBEAT_TIMEOUT) {
      this.log('ERROR', `Heartbeat timeout: ${elapsed}ms (expected < ${HEARTBEAT_TIMEOUT}ms)`);

      // Process may be hung, try to kill and restart
      if (this.child && !this.child.killed) {
        this.child.kill();
      }

      this.handleProcessExit(null, 'heartbeat-timeout');
    }
  }

  private handleMessage(line: string): void {
    try {
      const msg = JSON.parse(line);

      // Handle heartbeat from native core
      if (msg.type === 'heartbeat') {
        this.lastHeartbeat = Date.now();
        return;
      }

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
    this.log('ERROR', `Recording error: ${error}`);
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

    this.log('INFO', `Recording finished: ${result.videoPath}`);
    BrowserWindow.getAllWindows().forEach((win) => {
      win.webContents.send('recording-finished', result);
    });
  }

  destroy(): void {
    this.stopHeartbeat();

    if (this.child) {
      this.log('INFO', 'Destroying recorder service');

      try {
        this.child.stdin?.write(JSON.stringify({ action: 'quit' }) + '\n');
      } catch {
        // Ignore errors during shutdown
      }

      // Force kill after timeout
      setTimeout(() => {
        if (this.child && !this.child.killed) {
          this.child.kill();
          this.child = null;
        }
      }, 2000);
    }

    // Close log file
    if (this.logStream) {
      this.logStream.end();
      this.logStream = null;
    }
  }
}
