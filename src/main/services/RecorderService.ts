import { spawn, ChildProcess } from 'child_process';
import { BrowserWindow, app } from 'electron';
import readline from 'readline';
import path from 'path';
import fs from 'fs';
import net from 'net';

export interface RecordingConfig {
  resolution: { width: number; height: number };
  fps: number;
  bitrate?: number;
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

export interface InputState {
  anyKeyPressed: boolean;
  mouseButtonPressed: boolean;
  pressedKeyCount: number;
}

// Communication mode
export type CommMode = 'local' | 'remote';

// Recorder configuration
export interface RecorderConfig {
  mode: CommMode;
  nativeCorePath?: string;
  remoteHost?: string;
  remotePort?: number;
}

type RecordingState = 'idle' | 'recording' | 'paused' | 'reconnecting';

// Heartbeat configuration
const HEARTBEAT_INTERVAL = 3000; // 3 seconds
const HEARTBEAT_TIMEOUT = 5000;  // 5 seconds timeout
const RESTART_DELAY = 3000;       // 3 seconds before restart
const DEFAULT_REMOTE_PORT = 8765;

// Pending request type
type PendingResolver = (value: any) => void;
type PendingRejecter = (reason?: any) => void;

interface PendingRequest {
  resolve: PendingResolver;
  reject: PendingRejecter;
  timeout: NodeJS.Timeout;
  expectedType: string;
}

export class RecorderService {
  private child: ChildProcess | null = null;
  private socket: net.Socket | null = null;
  private config: RecorderConfig;
  private state: RecordingState = 'idle';
  private recordingStartTime: number = 0;
  private duration: number = 0;
  private frameCount: number = 0;

  // Heartbeat tracking
  private lastHeartbeat: number = 0;
  private heartbeatTimer: NodeJS.Timeout | null = null;
  private isRestarting: boolean = false;

  // Buffer for socket data
  private socketBuffer: string = '';

  // Pending requests (for request/response pattern)
  private pendingRequests: Map<string, PendingRequest> = new Map();

  // Logging
  private logPath: string;
  private logStream: fs.WriteStream | null = null;

  constructor(config: RecorderConfig) {
    this.config = config;
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

    this.log('INFO', `RecorderService initialized in ${this.config.mode} mode`);
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

  // Send command to native core (handles both local and remote)
  private sendCommand(command: object): void {
    const json = JSON.stringify(command) + '\n';

    if (this.config.mode === 'local' && this.child?.stdin) {
      this.child.stdin.write(json);
    } else if (this.config.mode === 'remote' && this.socket) {
      this.socket.write(json);
    }
  }

  // Generate unique request ID
  private generateRequestId(): string {
    return Math.random().toString(36).substring(2, 11);
  }

  // Send command and wait for response
  private async sendCommandWithResponse(expectedType: string, command: object, timeoutMs: number = 5000): Promise<any> {
    const requestId = this.generateRequestId();

    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pendingRequests.delete(requestId);
        reject(new Error('Request timeout'));
      }, timeoutMs);

      this.pendingRequests.set(requestId, {
        resolve,
        reject,
        timeout,
        expectedType,
      });

      this.sendCommand(command);
    });
  }

  async start(config: RecordingConfig): Promise<void> {
    if (this.state !== 'idle') {
      throw new Error(`Cannot start recording: already ${this.state}`);
    }

    await this.connect();

    const command = {
      action: 'start',
      config: {
        width: config.resolution.width,
        height: config.resolution.height,
        fps: config.fps,
        bitrate: config.bitrate || 15000,
        savePath: config.savePath,
        separateAudio: config.separateAudio,
        remuxToMp4: config.remuxToMp4,
      },
    };

    this.sendCommand(command);
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

    this.sendCommand({ action: 'stop' });

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

    this.sendCommand({ action: 'pause' });
    this.state = 'paused';
    this.log('INFO', 'Recording paused');
    this.broadcastStatus();
  }

  async resume(): Promise<void> {
    if (this.state !== 'paused') {
      throw new Error(`Cannot resume: state is ${this.state}`);
    }

    this.sendCommand({ action: 'resume' });
    this.state = 'recording';
    this.log('INFO', 'Recording resumed');
    this.broadcastStatus();
  }

  async getSystemInfo(): Promise<SystemInfo> {
    await this.connect();
    return this.sendCommandWithResponse('sysinfo', { action: 'sysinfo' });
  }

  async checkInputState(): Promise<InputState> {
    await this.connect();
    const result = await this.sendCommandWithResponse('input_state', { action: 'check_input' });
    return {
      anyKeyPressed: result.anyKeyPressed,
      mouseButtonPressed: result.mouseButtonPressed,
      pressedKeyCount: result.pressedKeyCount,
    };
  }

  // Connect to native core (local process or remote socket)
  private async connect(): Promise<void> {
    if (this.config.mode === 'local') {
      if (!this.child) {
        this.ensureLocalCore();
      }
    } else {
      if (!this.socket || !this.isSocketConnected()) {
        await this.connectToRemote();
      }
    }
  }

  private isSocketConnected(): boolean {
    return this.socket !== null && !this.socket.destroyed;
  }

  private ensureLocalCore(): void {
    if (this.child) {
      return;
    }

    if (!this.config.nativeCorePath) {
      throw new Error('Native core path not configured');
    }

    this.log('INFO', `Starting native core: ${this.config.nativeCorePath}`);

    this.child = spawn(this.config.nativeCorePath, [], {
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

  private connectToRemote(): Promise<void> {
    return new Promise((resolve, reject) => {
      const host = this.config.remoteHost || 'localhost';
      const port = this.config.remotePort || DEFAULT_REMOTE_PORT;

      this.log('INFO', `Connecting to remote recorder at ${host}:${port}`);

      this.socket = net.createConnection({ host, port }, () => {
        this.log('INFO', 'Connected to remote recorder');
        this.startSocketListeners();
        resolve();
      });

      this.socket.on('error', (err) => {
        this.log('ERROR', `Socket error: ${err.message}`);
        reject(err);
      });

      this.socket.on('close', () => {
        this.log('INFO', 'Socket closed');
        this.handleSocketClose();
      });
    });
  }

  private startSocketListeners(): void {
    if (!this.socket) return;

    this.socket.on('data', (data: Buffer) => {
      this.socketBuffer += data.toString();
      // Process complete lines
      const lines = this.socketBuffer.split('\n');
      this.socketBuffer = lines.pop() || '';
      lines.forEach(line => {
        if (line.trim()) {
          this.handleMessage(line);
        }
      });
    });
  }

  private handleSocketClose(): void {
    const wasRecording = this.state === 'recording' || this.state === 'paused';

    // Clear all pending requests
    this.pendingRequests.forEach((request) => {
      clearTimeout(request.timeout);
      request.reject(new Error('Connection closed'));
    });
    this.pendingRequests.clear();

    if (wasRecording && !this.isRestarting) {
      this.state = 'reconnecting';
      this.broadcastStatus();
      this.log('ERROR', 'Remote connection lost during recording');

      BrowserWindow.getAllWindows().forEach((win) => {
        win.webContents.send('recording-error', '远程连接中断');
      });
    } else {
      this.state = 'idle';
    }

    this.socket = null;
    this.stopHeartbeat();
    this.broadcastStatus();
  }

  private handleProcessExit(code: number | null, signal: string | null): void {
    const wasRecording = this.state === 'recording' || this.state === 'paused';
    this.child = null;

    // Clear all pending requests
    this.pendingRequests.forEach((request) => {
      clearTimeout(request.timeout);
      request.reject(new Error('Process exited'));
    });
    this.pendingRequests.clear();

    if (wasRecording && !this.isRestarting) {
      this.state = 'reconnecting';
      this.broadcastStatus();
      this.log('ERROR', 'Native core crashed during recording');

      BrowserWindow.getAllWindows().forEach((win) => {
        win.webContents.send('recording-error', '录制进程异常退出');
      });

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
    this.log('INFO', 'Attempting to restart...');

    setTimeout(() => {
      this.isRestarting = false;

      if (this.config.mode === 'local') {
        this.ensureLocalCore();
        if (this.child) {
          this.log('INFO', 'Native core restarted successfully');
        } else {
          this.log('ERROR', 'Failed to restart native core');
          this.state = 'idle';
          this.broadcastStatus();
        }
      } else {
        this.connectToRemote().then(() => {
          this.log('INFO', 'Remote connection restored');
        }).catch((err) => {
          this.log('ERROR', `Failed to reconnect: ${err.message}`);
          this.state = 'idle';
          this.broadcastStatus();
        });
      }
    }, 2000);
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
      this.log('ERROR', `Heartbeat timeout: ${elapsed}ms`);

      if (this.config.mode === 'local' && this.child && !this.child.killed) {
        this.child.kill();
      } else if (this.config.mode === 'remote' && this.socket) {
        this.socket.destroy();
      }

      this.handleProcessExit(null, 'heartbeat-timeout');
    } else {
      // Send a simple command to check connection is alive
      this.sendCommand({ action: 'status' });
    }
  }

  private handleMessage(line: string): void {
    try {
      const msg = JSON.parse(line);
      const msgType = msg.type;

      // Check if there's a pending request waiting for this type
      if (this.pendingRequests.size > 0) {
        for (const [id, request] of this.pendingRequests) {
          if (request.expectedType === msgType) {
            clearTimeout(request.timeout);
            this.pendingRequests.delete(id);
            request.resolve(msg.data || msg);
            return;
          }
        }
      }

      // Handle other message types (broadcasts)
      switch (msgType) {
        case 'heartbeat':
          this.lastHeartbeat = Date.now();
          break;
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
        case 'sysinfo':
          // Sysinfo message is handled by the one-time listener in getSystemInfo
          break;
        case 'input_state':
          // Input state message is handled by the one-time listener in checkInputState
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

    // Clear pending requests
    this.pendingRequests.forEach((request) => {
      clearTimeout(request.timeout);
      request.reject(new Error('Service destroyed'));
    });
    this.pendingRequests.clear();

    if (this.config.mode === 'local' && this.child) {
      this.log('INFO', 'Destroying recorder service');
      try {
        this.child.stdin?.write(JSON.stringify({ action: 'quit' }) + '\n');
      } catch {
        // Ignore errors during shutdown
      }

      setTimeout(() => {
        if (this.child && !this.child.killed) {
          this.child.kill();
          this.child = null;
        }
      }, 2000);
    } else if (this.config.mode === 'remote' && this.socket) {
      this.log('INFO', 'Closing remote connection');
      try {
        this.socket.write(JSON.stringify({ action: 'quit' }) + '\n');
      } catch {
        // Ignore errors during shutdown
      }

      setTimeout(() => {
        if (this.socket && !this.socket.destroyed) {
          this.socket.destroy();
          this.socket = null;
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
