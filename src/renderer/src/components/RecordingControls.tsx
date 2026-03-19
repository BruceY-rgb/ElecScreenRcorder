import { useState, useCallback, useEffect, useMemo, useRef } from 'react';
import { useRecorder, RecordingConfig, SystemInfo } from '../hooks/useRecorder';

function formatDuration(ms: number): string {
  const totalSec = Math.floor(ms / 1000);
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
}

interface RecordingControlsProps {
  systemInfo: SystemInfo | null;
}

function RecordingControls({ systemInfo }: RecordingControlsProps) {
  // Get default resolution from systemInfo
  const getDefaultResolution = () => {
    if (systemInfo?.screenWidth && systemInfo?.screenHeight) {
      return { width: systemInfo.screenWidth, height: systemInfo.screenHeight };
    }
    return { width: 1920, height: 1080 }; // Fallback
  };

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
    resolution: getDefaultResolution(),
    fps: 60,
    bitrate: 15000,
    savePath: '',
    separateAudio: true,
    remuxToMp4: false,
    captureAudio: true,
    captureMicrophone: false,
    microphoneDevice: '外部麦克风 (Realtek(R) Audio)',
    organizeByTimestamp: true,
  });

  // 可用的麦克风设备列表
  const microphoneOptions = [
    { value: '外部麦克风 (Realtek(R) Audio)', label: '外部麦克风 (Realtek(R) Audio)' },
    { value: '麦克风阵列 (适用于数字麦克风的英特尔® 智音技术)', label: '麦克风阵列 (英特尔® 智音技术)' },
  ];

  const [savePath, setSavePath] = useState('');
  const [setAsDefault, setSetAsDefault] = useState(false);
  const [autoCollapseOverlay, setAutoCollapseOverlay] = useState(false);
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
    // Load preferences
    window.electronAPI.getPreferences().then((prefs) => {
      if (prefs.autoCollapseOverlay !== undefined) {
        setAutoCollapseOverlay(prefs.autoCollapseOverlay);
      }
    });
  }, []);

  // Dynamic resolution options based on screen size
  const resolutionOptions = useMemo(() => {
    const screenWidth = systemInfo?.screenWidth || 1920;
    const screenHeight = systemInfo?.screenHeight || 1080;

    const options = [
      { value: '1280x720', label: '1280×720 (720p)', width: 1280, height: 720 },
      { value: '1920x1080', label: '1920×1080 (1080p)', width: 1920, height: 1080 },
      { value: '2560x1440', label: '2560×1440 (2K)', width: 2560, height: 1440 },
      { value: '3840x2160', label: '3840×2160 (4K)', width: 3840, height: 2160 },
      // Add current screen size as an option
      { value: `${screenWidth}x${screenHeight}`, label: `${screenWidth}×${screenHeight} (当前屏幕)`, width: screenWidth, height: screenHeight },
    ];

    // Deduplicate and filter out options larger than screen
    return options.filter((opt, index, self) =>
      self.findIndex(o => o.width === opt.width && o.height === opt.height) === index
    ).filter(opt => opt.width <= screenWidth && opt.height <= screenHeight);
  }, [systemInfo?.screenWidth, systemInfo?.screenHeight]);

  // Auto-adjust resolution if current selection exceeds screen or use screen size as default
  useEffect(() => {
    if (systemInfo && resolutionOptions.length > 0) {
      const screenWidth = systemInfo.screenWidth;
      const screenHeight = systemInfo.screenHeight;

      // Check if screen size is available in options (it should be now)
      const screenOption = resolutionOptions.find(
        opt => opt.width === screenWidth && opt.height === screenHeight
      );

      // Default to current screen size if available
      if (screenOption) {
        setConfig(prev => ({
          ...prev,
          resolution: { width: screenWidth, height: screenHeight }
        }));
      }
    }
  }, [systemInfo, resolutionOptions]);

  // Hotkey configuration state
  const [hotkeys, setHotkeys] = useState<Record<string, string>>({
    start: 'F9',
    pause: 'F10',
    stop: 'F11',
  });

  // Load hotkeys from preferences on mount
  useEffect(() => {
    window.electronAPI.getHotkeys().then((savedHotkeys) => {
      if (savedHotkeys && Object.keys(savedHotkeys).length > 0) {
        setHotkeys(savedHotkeys);
      }
    });
  }, []);

  // Save hotkeys when they change and re-register shortcuts
  const updateHotkey = useCallback((action: string, hotkey: string) => {
    setHotkeys(prev => {
      const updated = { ...prev, [action]: hotkey };
      window.electronAPI.setHotkeys(updated).then(() => {
        // Re-register shortcuts in main process
        window.electronAPI.reRegisterHotkeys();
      });
      return updated;
    });
  }, []);

  // Ref to always access the latest handleStartRecording callback
  const handleStartRecordingRef = useRef<((skipInputCheck: boolean) => Promise<void>) | null>(null);
  // Listen for hotkey events from main process
  useEffect(() => {
    window.electronAPI.onHotkeyPressed((action: string) => {
      console.log('[RecordingControls] Hotkey pressed:', action);
      if (action === 'start') {
        // Skip input check when triggered by hotkey (the hotkey press is expected)
        handleStartRecordingRef.current?.(true);
      } else if (action === 'pause') {
        if (isPaused) {
          resumeRecording();
        } else if (isRecording) {
          pauseRecording();
        }
      } else if (action === 'stop') {
        stopRecording();
      }
    });
  }, [isRecording, isPaused, resumeRecording, pauseRecording, stopRecording]);

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

  const handleStartRecording = useCallback(async (skipInputCheck: boolean = false) => {
    setIsCheckingInput(true);
    setInputWarning(null);
    setLastVideoPath(null);

    const finalConfig = {
      ...config,
      savePath: savePath || '',
      organizeByTimestamp: config.organizeByTimestamp ?? true,
    };

    // Debug: log the config being used
    console.log('[DEBUG handleStartRecording] Resolution:', finalConfig.resolution);
    console.log('[DEBUG handleStartRecording] Config source:', skipInputCheck ? 'hotkey' : 'button');

    if (setAsDefault && savePath) {
      await window.electronAPI.setDefaultSavePath(savePath);
    }

    try {
      // Skip input check when triggered by hotkey (the hotkey press is expected)
      if (!skipInputCheck) {
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
      }

      await startRecording(finalConfig);
    } catch (err) {
      console.error('Failed to start recording:', err);
    } finally {
      setIsCheckingInput(false);
    }
  }, [checkInputState, config, startRecording, savePath, setAsDefault]);

  // Keep ref in sync every render (no useEffect needed, avoids TDZ)
  handleStartRecordingRef.current = handleStartRecording;

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
            <div className="hero-mouse-activity" style={{ color: 'red', fontWeight: 'bold' }}>
              Mouse: {status.mouseActivity || 0} events/s
              {console.log('[DEBUG] Mouse Activity:', status.mouseActivity)}
            </div>
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
            <div className="hero-mouse-activity" style={{ color: 'red', fontWeight: 'bold' }}>
              Mouse: {status.mouseActivity || 0} events/s
              {console.log('[DEBUG PAUSED] Mouse Activity:', status.mouseActivity)}
            </div>
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
                {resolutionOptions.map((opt) => (
                  <option key={opt.value} value={opt.value}>{opt.label}</option>
                ))}
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
                checked={config.captureMicrophone ?? false}
                onChange={(e) => setConfig({ ...config, captureMicrophone: e.target.checked })}
              />
              Microphone
            </label>

            {/* 麦克风设备选择 - 当启用麦克风时显示 */}
            {config.captureMicrophone && (
              <div className="setting-row" style={{ marginLeft: '20px', marginTop: '8px' }}>
                <span className="setting-label">Microphone Device</span>
                <select
                  className="setting-select"
                  value={config.microphoneDevice || microphoneOptions[0].value}
                  onChange={(e) => setConfig({ ...config, microphoneDevice: e.target.value })}
                >
                  {microphoneOptions.map((opt) => (
                    <option key={opt.value} value={opt.value}>{opt.label}</option>
                  ))}
                </select>
              </div>
            )}

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

            <label className="setting-checkbox">
              <input
                type="checkbox"
                checked={autoCollapseOverlay}
                onChange={(e) => {
                  const checked = e.target.checked;
                  setAutoCollapseOverlay(checked);
                  window.electronAPI.setPreferences({ autoCollapseOverlay: checked });
                }}
              />
              Auto-collapse overlay when recording
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
                value={hotkeys.start || ''}
                readOnly
                placeholder="Click to set"
                onClick={() => setHotkeys(prev => ({ ...prev, start: 'Press key...' }))}
                onKeyDown={(e) => {
                  e.preventDefault();
                  if (e.key === 'Escape') {
                    updateHotkey('start', '');
                    return;
                  }

                  // Build modifier key string
                  const parts: string[] = [];
                  if (e.ctrlKey) parts.push('Ctrl');
                  if (e.altKey) parts.push('Alt');
                  if (e.shiftKey) parts.push('Shift');
                  if (e.metaKey) parts.push('Meta');

                  // Add main key
                  const key = e.key.length === 1 ? e.key.toUpperCase() : e.key;
                  if (key !== 'Control' && key !== 'Alt' && key !== 'Shift' && key !== 'Meta') {
                    parts.push(key);
                  }

                  if (parts.length > 0) {
                    updateHotkey('start', parts.join('+'));
                  }
                }}
              />
            </div>

            <div className="hotkey-row">
              <span className="hotkey-label">Pause</span>
              <input
                className="hotkey-badge"
                type="text"
                value={hotkeys.pause || ''}
                readOnly
                placeholder="Click to set"
                onClick={() => setHotkeys(prev => ({ ...prev, pause: 'Press key...' }))}
                onKeyDown={(e) => {
                  e.preventDefault();
                  if (e.key === 'Escape') {
                    updateHotkey('pause', '');
                    return;
                  }

                  // Build modifier key string
                  const parts: string[] = [];
                  if (e.ctrlKey) parts.push('Ctrl');
                  if (e.altKey) parts.push('Alt');
                  if (e.shiftKey) parts.push('Shift');
                  if (e.metaKey) parts.push('Meta');

                  // Add main key
                  const key = e.key.length === 1 ? e.key.toUpperCase() : e.key;
                  if (key !== 'Control' && key !== 'Alt' && key !== 'Shift' && key !== 'Meta') {
                    parts.push(key);
                  }

                  if (parts.length > 0) {
                    updateHotkey('pause', parts.join('+'));
                  }
                }}
              />
            </div>

            <div className="hotkey-row">
              <span className="hotkey-label">Stop</span>
              <input
                className="hotkey-badge"
                type="text"
                value={hotkeys.stop || ''}
                readOnly
                placeholder="Click to set"
                onClick={() => setHotkeys(prev => ({ ...prev, stop: 'Press key...' }))}
                onKeyDown={(e) => {
                  e.preventDefault();
                  if (e.key === 'Escape') {
                    updateHotkey('stop', '');
                    return;
                  }

                  // Build modifier key string
                  const parts: string[] = [];
                  if (e.ctrlKey) parts.push('Ctrl');
                  if (e.altKey) parts.push('Alt');
                  if (e.shiftKey) parts.push('Shift');
                  if (e.metaKey) parts.push('Meta');

                  // Add main key
                  const key = e.key.length === 1 ? e.key.toUpperCase() : e.key;
                  if (key !== 'Control' && key !== 'Alt' && key !== 'Shift' && key !== 'Meta') {
                    parts.push(key);
                  }

                  if (parts.length > 0) {
                    updateHotkey('stop', parts.join('+'));
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
