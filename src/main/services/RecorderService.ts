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
  captureAudio?: boolean;
  captureMicrophone?: boolean;
  // 新增：是否将文件整理到时间戳文件夹
  organizeByTimestamp?: boolean;
}

export interface RecordingStatus {
  state: 'idle' | 'recording' | 'paused' | 'reconnecting';
  duration: number;
  frameCount: number;
  resolution?: { width: number; height: number };
  fps?: number;
  inputState?: {
    anyKeyPressed: boolean;
    mouseButtonPressed: boolean;
    pressedKeyCount: number;
  };
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
  recordingFolder: string;
}

export interface InputState {
  anyKeyPressed: boolean;
  mouseButtonPressed: boolean;
  pressedKeyCount: number;
  pressedVKs?: number[];
  pressedMouseBtns?: number[];
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

  // Status broadcast timer (for duration updates)
  private statusBroadcastTimer: NodeJS.Timeout | null = null;
  private pausedAt: number = 0;

  // Buffer for socket data
  private socketBuffer: string = '';

  // Pending requests (for request/response pattern)
  private pendingRequests: Map<string, PendingRequest> = new Map();

  // Logging
  private logPath: string = '';
  private logStream: fs.WriteStream | null = null;

  // 录制输出文件夹（时间戳文件夹）
  private recordingFolder: string = '';

  // Remux to MP4 after recording
  private shouldRemuxToMp4: boolean = false;

  // Current recording config (for broadcasting to overlay)
  private currentConfig: RecordingConfig | null = null;

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
        // Also remove the error listener
        this.pendingRequests.delete(requestId + '_err');
        reject(new Error('Request timeout'));
      }, timeoutMs);

      // Listen for expected response type
      this.pendingRequests.set(requestId, {
        resolve: (value: any) => {
          this.pendingRequests.delete(requestId + '_err');
          resolve(value);
        },
        reject,
        timeout,
        expectedType,
      });

      // Also listen for error response (native core returns {"type":"error"} on failure)
      this.pendingRequests.set(requestId + '_err', {
        resolve: (value: any) => {
          clearTimeout(timeout);
          this.pendingRequests.delete(requestId);
          reject(new Error(value.msg || value.message || 'Native core error'));
        },
        reject,
        timeout: null as any, // managed by the primary request's timeout
        expectedType: 'error',
      });

      this.sendCommand(command);
    });
  }

  async start(config: RecordingConfig): Promise<void> {
    if (this.state !== 'idle') {
      throw new Error(`Cannot start recording: already ${this.state}`);
    }

    // Generate default save path if not provided
    let savePath = config.savePath;
    const organizeByTimestamp = config.organizeByTimestamp !== false; // 默认启用

    this.log('INFO', `[DEBUG] start() called. config.savePath="${config.savePath}" organizeByTimestamp=${organizeByTimestamp}`);

    if (!savePath) {
      const videosDir = app.getPath('videos');
      const timestamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);

      if (organizeByTimestamp) {
        // 创建以时间戳命名的文件夹
        this.recordingFolder = path.join(videosDir, timestamp);
        if (!fs.existsSync(this.recordingFolder)) {
          fs.mkdirSync(this.recordingFolder, { recursive: true });
        }
        savePath = path.join(this.recordingFolder, `recording.mkv`);
      } else {
        // 保持原有的扁平结构
        this.recordingFolder = '';
        savePath = path.join(videosDir, `recording_${timestamp}.mkv`);
      }
    } else {
      // savePath is a directory chosen by user — need to append filename
      const timestamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);

      if (organizeByTimestamp) {
        // 在用户选择的目录下创建时间戳子文件夹
        this.recordingFolder = path.join(savePath, timestamp);
        if (!fs.existsSync(this.recordingFolder)) {
          fs.mkdirSync(this.recordingFolder, { recursive: true });
        }
        savePath = path.join(this.recordingFolder, `recording.mkv`);
      } else {
        this.recordingFolder = '';
        savePath = path.join(savePath, `recording_${timestamp}.mkv`);
      }
    }

    this.log('INFO', `[DEBUG] Final savePath="${savePath}" recordingFolder="${this.recordingFolder}"`);

    // Ensure save directory exists
    const saveDir = path.dirname(savePath);
    if (!fs.existsSync(saveDir)) {
      fs.mkdirSync(saveDir, { recursive: true });
    }

    await this.connect();

    const command = {
      action: 'start',
      config: {
        width: config.resolution.width,
        height: config.resolution.height,
        fps: config.fps,
        bitrate: config.bitrate || 15000,
        savePath,
        separateAudio: config.separateAudio,
        remuxToMp4: config.remuxToMp4,
        captureAudio: config.captureAudio ?? true,
        captureMicrophone: config.captureMicrophone ?? false,
      },
    };

    // Send start command and wait for native core response
    const response = await this.sendCommandWithResponse('status', command, 10000);

    // Native core responds with {"state":"recording","type":"status"} on success
    if (response.state !== 'recording') {
      throw new Error(`Failed to start recording: native core returned state '${response.state}'`);
    }

    this.state = 'recording';
    this.recordingStartTime = Date.now();
    this.duration = 0;
    this.frameCount = 0;
    this.shouldRemuxToMp4 = config.remuxToMp4;
    this.currentConfig = config;

    this.log('INFO', `Recording started: ${config.resolution.width}x${config.resolution.height} @ ${config.fps}fps`);
    this.broadcastStatus();
    this.startHeartbeat();
    this.startStatusBroadcast();
  }

  async stop(): Promise<FinishResult> {
    if (this.state === 'idle') {
      throw new Error('Cannot stop: not recording');
    }

    const response = await this.sendCommandWithResponse('finish', { action: 'stop' }, 10000);

    this.state = 'idle';
    this.stopHeartbeat();
    this.stopStatusBroadcast();
    this.currentConfig = null;
    this.log('INFO', 'Recording stopped');
    this.broadcastStatus();

    let videoPath: string = response.videoPath || '';

    // Remux MKV to MP4 if requested
    if (this.shouldRemuxToMp4 && videoPath && /\.mkv$/i.test(videoPath)) {
      videoPath = await this.remuxMkvToMp4(videoPath);
    }

    return {
      videoPath,
      actionsPath: response.actionsPath || '',
      movementsPath: response.movementsPath || '',
      duration: response.duration || this.duration,
      recordingFolder: this.recordingFolder,
    };
  }

  async pause(): Promise<void> {
    if (this.state !== 'recording') {
      throw new Error(`Cannot pause: state is ${this.state}`);
    }

    await this.sendCommandWithResponse('status', { action: 'pause' }, 5000);
    this.state = 'paused';
    this.duration = Date.now() - this.recordingStartTime;
    this.pausedAt = Date.now();
    this.stopStatusBroadcast();
    this.log('INFO', 'Recording paused');
    this.broadcastStatus();
  }

  async resume(): Promise<void> {
    if (this.state !== 'paused') {
      throw new Error(`Cannot resume: state is ${this.state}`);
    }

    await this.sendCommandWithResponse('status', { action: 'resume' }, 5000);
    this.state = 'recording';
    // Adjust start time to exclude paused duration
    this.recordingStartTime += Date.now() - this.pausedAt;
    this.startStatusBroadcast();
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
    console.log('[DEBUG RecorderService] raw input_state from native:', JSON.stringify(result));
    return {
      anyKeyPressed: result.anyKeyPressed,
      mouseButtonPressed: result.mouseButtonPressed,
      pressedKeyCount: result.pressedKeyCount,
      pressedVKs: result.pressedVKs || [],
      pressedMouseBtns: result.pressedMouseBtns || [],
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
        const level = this.classifyStderrMessage(message);
        this.log(level, `[Native Core] ${message}`);
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
    this.stopStatusBroadcast();
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
    this.stopStatusBroadcast();
    this.broadcastStatus();
  }

  private handleProcessError(err: Error): void {
    this.log('ERROR', `Process error: ${err.message}`);

    // Always clear the child reference on error so connect() can retry
    this.child = null;

    // Clear all pending requests
    this.pendingRequests.forEach((request) => {
      clearTimeout(request.timeout);
      request.reject(new Error(`Process error: ${err.message}`));
    });
    this.pendingRequests.clear();

    if (this.state === 'recording' || this.state === 'paused') {
      this.state = 'reconnecting';
      this.stopStatusBroadcast();
      this.broadcastStatus();

      setTimeout(() => {
        this.attemptRestart();
      }, RESTART_DELAY);
    } else {
      this.state = 'idle';
      this.broadcastStatus();
    }
  }

  private remuxMkvToMp4(mkvPath: string): Promise<string> {
    const mp4Path = mkvPath.replace(/\.mkv$/i, '.mp4');
    const ffmpegPath = path.join(path.dirname(this.config.nativeCorePath!), 'ffmpeg.exe');

    this.log('INFO', `Remuxing MKV to MP4: ${mkvPath} -> ${mp4Path}`);

    return new Promise((resolve) => {
      const proc = spawn(ffmpegPath, [
        '-y', '-i', mkvPath, '-c', 'copy', mp4Path,
      ], { windowsHide: true });

      let stderr = '';
      proc.stderr?.on('data', (data) => {
        stderr += data.toString();
      });

      proc.on('error', (err) => {
        this.log('WARN', `Remux failed to start: ${err.message}`);
        resolve(mkvPath);
      });

      proc.on('exit', (code) => {
        if (code === 0 && fs.existsSync(mp4Path)) {
          this.log('INFO', `Remux completed: ${mp4Path}`);
          resolve(mp4Path);
        } else {
          this.log('WARN', `Remux failed (exit code ${code}): ${stderr.slice(-500)}`);
          resolve(mkvPath);
        }
      });
    });
  }

  private classifyStderrMessage(message: string): string {
    // Native core recorder messages
    if (message.includes('[RECORDER]')) {
      if (/error|Error|FAILED|failed/.test(message)) {
        return 'ERROR';
      }
      return 'INFO';
    }
    // FFmpeg progress output (frame= ... fps= ... )
    if (/^frame=/.test(message)) return 'DEBUG';
    // FFmpeg informational output
    if (/^(ffmpeg version|built with|configuration:|lib(av|sw)|Input #|Output #|Stream |Duration:|Press \[q\]|Stream mapping)/.test(message)) return 'DEBUG';
    // FFmpeg codec/filter messages (e.g. [libx264 @ ...], [gdigrab @ ...])
    if (/^\[[\w]+ @/.test(message)) return 'DEBUG';
    // Metadata lines
    if (/^\s*(Metadata:|encoder\s|Side data:|cpb:)/.test(message)) return 'DEBUG';
    // FFmpeg profile/options lines
    if (/^\s*\d+ fps,/.test(message)) return 'DEBUG';
    // Default to WARN for unclassified stderr
    return 'WARN';
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

  private startStatusBroadcast(): void {
    this.stopStatusBroadcast();
    this.statusBroadcastTimer = setInterval(async () => {
      this.updateDuration();
      // Poll input state during recording
      if (this.state === 'recording') {
        try {
          const inputState = await this.checkInputState();
          this.broadcastStatusWithInput(inputState);
        } catch {
          this.broadcastStatus();
        }
      } else {
        this.broadcastStatus();
      }
    }, 1000);
  }

  private stopStatusBroadcast(): void {
    if (this.statusBroadcastTimer) {
      clearInterval(this.statusBroadcastTimer);
      this.statusBroadcastTimer = null;
    }
  }

  private updateDuration(): void {
    if (this.state === 'recording' && this.recordingStartTime > 0) {
      this.duration = Date.now() - this.recordingStartTime;
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
      // Send a lightweight command to check connection is alive
      this.sendCommand({ action: 'check_input' });
    }
  }

  private handleMessage(line: string): void {
    try {
      const msg = JSON.parse(line);
      const msgType = msg.type;

      // Any message from native core proves it's alive
      this.lastHeartbeat = Date.now();

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
          // Already handled above
          break;
        case 'status': {
          // Map native core states to valid RecordingState
          const nativeState = msg.state;
          if (nativeState === 'ready' || nativeState === 'idle') {
            this.state = 'idle';
          } else if (nativeState === 'recording') {
            this.state = 'recording';
          } else if (nativeState === 'paused') {
            this.state = 'paused';
          }
          this.broadcastStatus();
          break;
        }
        case 'error':
          this.broadcastError(msg.msg);
          break;
        case 'finish':
          this.broadcastFinish(msg);
          break;
        case 'sysinfo':
          // Handled by pending request above
          break;
        case 'input_state':
          // Handled by pending request above (heartbeat ping responses land here too)
          break;
      }
    } catch {
      // Ignore parse errors
    }
  }

  private broadcastStatus(): void {
    this.broadcastStatusWithInput(undefined);
  }

  private broadcastStatusWithInput(inputState?: InputState): void {
    // Map 'reconnecting' to 'idle' for the renderer (it doesn't know about reconnecting)
    const frontendState = this.state === 'reconnecting' ? 'idle' : this.state;
    const status: RecordingStatus = {
      state: frontendState,
      duration: this.duration,
      frameCount: this.frameCount,
      resolution: this.currentConfig?.resolution,
      fps: this.currentConfig?.fps,
    };

    if (inputState) {
      status.inputState = {
        anyKeyPressed: inputState.anyKeyPressed,
        mouseButtonPressed: inputState.mouseButtonPressed,
        pressedKeyCount: inputState.pressedKeyCount,
      };
    }

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
      recordingFolder: this.recordingFolder || '',
    };

    this.log('INFO', `Recording finished: ${result.videoPath}`);
    BrowserWindow.getAllWindows().forEach((win) => {
      win.webContents.send('recording-finished', result);
    });
  }

  destroy(): void {
    this.stopHeartbeat();
    this.stopStatusBroadcast();

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
