import { useState, useCallback, useEffect } from 'react';
import { useRecorder, RecordingConfig } from '../hooks/useRecorder';

function formatDuration(ms: number): string {
  const totalSec = Math.floor(ms / 1000);
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}

function RecordingControls() {
  const {
    status,
    isIdle,
    isRecording,
    isPaused,
    canStart,
    canPause,
    canResume,
    canStop,
    startRecording,
    pauseRecording,
    resumeRecording,
    stopRecording,
    checkInputState,
  } = useRecorder();

  const [config, setConfig] = useState<RecordingConfig>({
    resolution: { width: 2560, height: 1440 },
    fps: 60,
    bitrate: 15000,
    savePath: '',
    separateAudio: true,
    remuxToMp4: false,
    captureAudio: true,
    organizeByTimestamp: true,
  });

  const [savePath, setSavePath] = useState('');
  const [setAsDefault, setSetAsDefault] = useState(false);
  const [isCheckingInput, setIsCheckingInput] = useState(false);
  const [inputWarning, setInputWarning] = useState<string | null>(null);
  const [lastVideoPath, setLastVideoPath] = useState<string | null>(null);

  // Collapsible panel states
  const [recordingSettingsOpen, setRecordingSettingsOpen] = useState(true);
  const [hotkeySettingsOpen, setHotkeySettingsOpen] = useState(true);

  // Auto-collapse when recording, auto-expand when idle
  useEffect(() => {
    if (isRecording || isPaused) {
      setRecordingSettingsOpen(false);
      setHotkeySettingsOpen(false);
    } else if (isIdle) {
      setRecordingSettingsOpen(true);
      setHotkeySettingsOpen(true);
    }
  }, [isRecording, isPaused, isIdle]);

  // Load default save path on mount
  useEffect(() => {
    window.electronAPI.getDefaultSavePath().then((defaultPath) => {
      if (defaultPath) {
        setSavePath(defaultPath);
        setSetAsDefault(true);
      }
    });
  }, []);

  // Hotkey configuration state
  const [hotkeys, setHotkeys] = useState({
    start: 'F9',
    pause: 'F10',
    stop: 'F11',
  });

  // Listen for recording finish to capture video path
  useEffect(() => {
    window.electronAPI.onRecordingFinish((result) => {
      if (result.videoPath) {
        setLastVideoPath(result.videoPath);
      }
    });
  }, []);

  const handleOpenVideo = useCallback(() => {
    if (lastVideoPath) {
      window.electronAPI.openVideo(lastVideoPath);
    }
  }, [lastVideoPath]);

  const handleBrowse = useCallback(async () => {
    const path = await window.electronAPI.selectSaveDirectory();
    if (path) {
      setSavePath(path);
    }
  }, []);

  const handleSetAsDefaultChange = useCallback((checked: boolean) => {
    setSetAsDefault(checked);
    if (checked && savePath) {
      window.electronAPI.setDefaultSavePath(savePath);
    }
  }, [savePath]);

  const handleStartRecording = useCallback(async () => {
    setIsCheckingInput(true);
    setInputWarning(null);
    setLastVideoPath(null);

    const finalConfig = {
      ...config,
      savePath: savePath || '',
      organizeByTimestamp: config.organizeByTimestamp ?? true,
    };

    if (setAsDefault && savePath) {
      await window.electronAPI.setDefaultSavePath(savePath);
    }

    try {
      const inputState = await checkInputState();

      // DEBUG: log full input state to DevTools console
      console.log('[DEBUG] checkInputState result:', JSON.stringify(inputState, null, 2));

      // Only block on keyboard keys — mouse buttons are expected to be
      // pressed because the user just clicked the Start button.
      if (inputState.pressedKeyCount > 0) {
        console.warn('[DEBUG] Blocked! pressedKeyCount:', inputState.pressedKeyCount,
          'pressedVKs:', inputState.pressedVKs,
          'pressedMouseBtns:', inputState.pressedMouseBtns);
        setInputWarning(
          `检测到输入设备异常: ${inputState.pressedKeyCount} 个键盘按键被按下 (VK: ${JSON.stringify(inputState.pressedVKs || [])})。请松开所有按键后再开始录制。`
        );
        setIsCheckingInput(false);
        return;
      }

      await startRecording(finalConfig);
    } catch (err) {
      console.error('Failed to start recording:', err);
    } finally {
      setIsCheckingInput(false);
    }
  }, [checkInputState, config, startRecording, savePath, setAsDefault]);

  // Clear warning after timeout
  useEffect(() => {
    if (inputWarning) {
      const timer = setTimeout(() => setInputWarning(null), 5000);
      return () => clearTimeout(timer);
    }
  }, [inputWarning]);

  // Determine hero button class
  const heroButtonClass = [
    'hero-button',
    isRecording ? 'hero-button--recording' : '',
    isPaused ? 'hero-button--paused' : '',
  ].filter(Boolean).join(' ');

  return (
    <>
      {/* Warning */}
      {inputWarning && <div className="warning-banner">{inputWarning}</div>}

      {/* ── Hero Section ── */}
      <div className="hero">
        {isIdle && !lastVideoPath && (
          <>
            <button
              className="hero-button"
              onClick={handleStartRecording}
              disabled={!canStart || isCheckingInput}
            >
              {isCheckingInput ? '...' : '●'}
            </button>
            <span className="hero-label">
              {isCheckingInput ? 'Checking input...' : 'Start Recording'}
            </span>
          </>
        )}

        {isIdle && lastVideoPath && (
          <>
            <button
              className="hero-button"
              onClick={handleStartRecording}
              disabled={!canStart || isCheckingInput}
            >
              ●
            </button>
            <span className="hero-label">Start New Recording</span>
            <div className="hero-last-recording">
              <p>{lastVideoPath}</p>
              <button onClick={handleOpenVideo}>Open Video</button>
            </div>
          </>
        )}

        {isRecording && (
          <>
            <div className={heroButtonClass}>●</div>
            <div className="hero-timer">{formatDuration(status.duration)}</div>
            <div className="hero-actions">
              <button
                className="hero-action-btn hero-action-btn--pause"
                onClick={pauseRecording}
                disabled={!canPause}
              >
                Pause
              </button>
              <button
                className="hero-action-btn hero-action-btn--stop"
                onClick={stopRecording}
                disabled={!canStop}
              >
                Stop
              </button>
            </div>
          </>
        )}

        {isPaused && (
          <>
            <div className={heroButtonClass}>❚❚</div>
            <div className="hero-timer">{formatDuration(status.duration)}</div>
            <div className="hero-actions">
              <button
                className="hero-action-btn hero-action-btn--resume"
                onClick={resumeRecording}
                disabled={!canResume}
              >
                Resume
              </button>
              <button
                className="hero-action-btn hero-action-btn--stop"
                onClick={stopRecording}
                disabled={!canStop}
              >
                Stop
              </button>
            </div>
          </>
        )}
      </div>

      {/* ── Recording Settings Panel ── */}
      <div className="settings-panel">
        <button
          className="settings-toggle"
          onClick={() => setRecordingSettingsOpen(!recordingSettingsOpen)}
        >
          Recording Settings
          <span className={`chevron ${recordingSettingsOpen ? 'open' : ''}`}>▼</span>
        </button>
        <div className={`settings-content ${recordingSettingsOpen ? 'open' : ''}`}>
          <div className="settings-content-inner">
            <div className="setting-row">
              <span className="setting-label">Resolution</span>
              <select
                className="setting-select"
                value={`${config.resolution.width}x${config.resolution.height}`}
                onChange={(e) => {
                  const [width, height] = e.target.value.split('x').map(Number);
                  setConfig({ ...config, resolution: { width, height } });
                }}
              >
                <option value="1280x720">1280×720 (720p)</option>
                <option value="1920x1080">1920×1080 (1080p)</option>
                <option value="2560x1440">2560×1440 (2K)</option>
                <option value="3840x2160">3840×2160 (4K)</option>
              </select>
            </div>

            <div className="setting-row">
              <span className="setting-label">Frame Rate</span>
              <select
                className="setting-select"
                value={config.fps}
                onChange={(e) => setConfig({ ...config, fps: Number(e.target.value) })}
              >
                <option value="30">30 FPS</option>
                <option value="60">60 FPS</option>
                <option value="120">120 FPS</option>
              </select>
            </div>

            <div className="setting-row">
              <span className="setting-label">Bitrate</span>
              <select
                className="setting-select"
                value={config.bitrate}
                onChange={(e) => setConfig({ ...config, bitrate: Number(e.target.value) })}
              >
                <option value="8000">8 Mbps (1080p Low)</option>
                <option value="10000">10 Mbps (1080p Med)</option>
                <option value="15000">15 Mbps (1080p High / 2K)</option>
                <option value="20000">20 Mbps (2K High)</option>
                <option value="25000">25 Mbps (4K)</option>
                <option value="35000">35 Mbps (4K High)</option>
              </select>
            </div>

            <div className="setting-row">
              <span className="setting-label">Save Location</span>
              <div className="save-path-row">
                <input
                  type="text"
                  value={savePath}
                  placeholder="Default: Videos folder"
                  readOnly
                />
                <button onClick={handleBrowse} type="button">Browse</button>
              </div>
            </div>

            <label className="setting-checkbox">
              <input
                type="checkbox"
                checked={setAsDefault}
                onChange={(e) => handleSetAsDefaultChange(e.target.checked)}
              />
              Set as default
            </label>

            <label className="setting-checkbox">
              <input
                type="checkbox"
                checked={config.captureAudio ?? true}
                onChange={(e) => setConfig({ ...config, captureAudio: e.target.checked })}
              />
              System Audio (Speaker)
            </label>

            <label className="setting-checkbox">
              <input
                type="checkbox"
                checked={config.separateAudio}
                onChange={(e) => setConfig({ ...config, separateAudio: e.target.checked })}
              />
              Separate Audio Tracks
            </label>

            <label className="setting-checkbox">
              <input
                type="checkbox"
                checked={config.remuxToMp4}
                onChange={(e) => setConfig({ ...config, remuxToMp4: e.target.checked })}
              />
              Convert to MP4
            </label>

            <label className="setting-checkbox">
              <input
                type="checkbox"
                checked={config.organizeByTimestamp ?? true}
                onChange={(e) => setConfig({ ...config, organizeByTimestamp: e.target.checked })}
              />
              Organize by timestamp
            </label>
          </div>
        </div>
      </div>

      {/* ── Hotkey Settings Panel ── */}
      <div className="settings-panel">
        <button
          className="settings-toggle"
          onClick={() => setHotkeySettingsOpen(!hotkeySettingsOpen)}
        >
          Hotkey Settings
          <span className={`chevron ${hotkeySettingsOpen ? 'open' : ''}`}>▼</span>
        </button>
        <div className={`settings-content ${hotkeySettingsOpen ? 'open' : ''}`}>
          <div className="settings-content-inner">
            <p className="hotkey-hint">Click to set new hotkey, press ESC to clear</p>

            <div className="hotkey-row">
              <span className="hotkey-label">Start</span>
              <input
                className="hotkey-badge"
                type="text"
                value={hotkeys.start}
                readOnly
                onClick={() => setHotkeys({ ...hotkeys, start: 'Press key...' })}
                onKeyDown={(e) => {
                  e.preventDefault();
                  if (e.key === 'Escape') {
                    setHotkeys({ ...hotkeys, start: '' });
                  } else {
                    const key = e.key.length === 1 ? e.key.toUpperCase() : e.key;
                    setHotkeys({ ...hotkeys, start: key });
                  }
                }}
              />
            </div>

            <div className="hotkey-row">
              <span className="hotkey-label">Pause</span>
              <input
                className="hotkey-badge"
                type="text"
                value={hotkeys.pause}
                readOnly
                onClick={() => setHotkeys({ ...hotkeys, pause: 'Press key...' })}
                onKeyDown={(e) => {
                  e.preventDefault();
                  if (e.key === 'Escape') {
                    setHotkeys({ ...hotkeys, pause: '' });
                  } else {
                    const key = e.key.length === 1 ? e.key.toUpperCase() : e.key;
                    setHotkeys({ ...hotkeys, pause: key });
                  }
                }}
              />
            </div>

            <div className="hotkey-row">
              <span className="hotkey-label">Stop</span>
              <input
                className="hotkey-badge"
                type="text"
                value={hotkeys.stop}
                readOnly
                onClick={() => setHotkeys({ ...hotkeys, stop: 'Press key...' })}
                onKeyDown={(e) => {
                  e.preventDefault();
                  if (e.key === 'Escape') {
                    setHotkeys({ ...hotkeys, stop: '' });
                  } else {
                    const key = e.key.length === 1 ? e.key.toUpperCase() : e.key;
                    setHotkeys({ ...hotkeys, stop: key });
                  }
                }}
              />
            </div>
          </div>
        </div>
      </div>
    </>
  );
}

export default RecordingControls;
