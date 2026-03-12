import { useState, useCallback, useEffect } from 'react';
import { useRecorder, RecordingConfig } from '../hooks/useRecorder';

function RecordingControls() {
  const {
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
  });

  // Hotkey configuration state
  const [hotkeys, setHotkeys] = useState({
    start: 'F9',
    pause: 'F10',
    stop: 'F11',
  });

  const [isCheckingInput, setIsCheckingInput] = useState(false);
  const [inputWarning, setInputWarning] = useState<string | null>(null);

  // Check input state before starting recording
  const handleStartRecording = useCallback(async () => {
    setIsCheckingInput(true);
    setInputWarning(null);

    try {
      const inputState = await checkInputState();

      if (inputState.anyKeyPressed) {
        const warnings = [];
        if (inputState.mouseButtonPressed) {
          warnings.push('鼠标按钮被按下');
        }
        if (inputState.pressedKeyCount > 0) {
          warnings.push(`${inputState.pressedKeyCount} 个键盘按键被按下`);
        }

        setInputWarning(`检测到输入设备异常: ${warnings.join(', ')}。请松开所有按键后再开始录制。`);
        setIsCheckingInput(false);
        return;
      }

      // Input is clean, start recording
      await startRecording(config);
    } catch (err) {
      console.error('Failed to start recording:', err);
    } finally {
      setIsCheckingInput(false);
    }
  }, [checkInputState, config, startRecording]);

  // Clear warning when user interacts
  useEffect(() => {
    if (inputWarning) {
      const timer = setTimeout(() => setInputWarning(null), 5000);
      return () => clearTimeout(timer);
    }
  }, [inputWarning]);

  return (
    <div className="controls">
      {/* Input State Warning */}
      {inputWarning && (
        <div className="warning-banner">
          {inputWarning}
        </div>
      )}

      <div className="config-section">
        <h3>Recording Settings</h3>

        <label>
          Resolution:
          <select
            value={`${config.resolution.width}x${config.resolution.height}`}
            onChange={(e) => {
              const [width, height] = e.target.value.split('x').map(Number);
              setConfig({ ...config, resolution: { width, height } });
            }}
          >
            <option value="1920x1080">1920x1080 (1080p)</option>
            <option value="2560x1440">2560x1440 (2K)</option>
            <option value="3840x2160">3840x2160 (4K)</option>
          </select>
        </label>

        <label>
          Frame Rate:
          <select
            value={config.fps}
            onChange={(e) => setConfig({ ...config, fps: Number(e.target.value) })}
          >
            <option value="30">30 FPS</option>
            <option value="60">60 FPS</option>
            <option value="120">120 FPS</option>
          </select>
        </label>

        <label>
          Bitrate (kbps):
          <select
            value={config.bitrate}
            onChange={(e) => setConfig({ ...config, bitrate: Number(e.target.value) })}
          >
            <option value="8000">8000 (1080p Low)</option>
            <option value="10000">10000 (1080p Medium)</option>
            <option value="15000">15000 (1080p High / 2K)</option>
            <option value="20000">20000 (2K High)</option>
            <option value="25000">25000 (4K)</option>
            <option value="35000">35000 (4K High)</option>
          </select>
        </label>

        <label>
          <input
            type="checkbox"
            checked={config.separateAudio}
            onChange={(e) => setConfig({ ...config, separateAudio: e.target.checked })}
          />
          Separate Audio Tracks (System + Microphone)
        </label>

        <label>
          <input
            type="checkbox"
            checked={config.remuxToMp4}
            onChange={(e) => setConfig({ ...config, remuxToMp4: e.target.checked })}
          />
          Convert to MP4 after recording
        </label>
      </div>

      {/* Hotkey Configuration */}
      <div className="config-section">
        <h3>Hotkey Settings</h3>
        <p className="hint">Click to set new hotkey, press ESC to clear</p>

        <div className="hotkey-config">
          <label>
            Start:
            <input
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
          </label>

          <label>
            Pause:
            <input
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
          </label>

          <label>
            Stop:
            <input
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
          </label>
        </div>
      </div>

      <div className="buttons">
        <button
          onClick={handleStartRecording}
          disabled={!canStart || isCheckingInput}
          className="start-btn"
        >
          {isCheckingInput ? 'Checking...' : 'Start Recording'}
        </button>
        <button onClick={pauseRecording} disabled={!canPause}>
          Pause
        </button>
        <button onClick={resumeRecording} disabled={!canResume}>
          Resume
        </button>
        <button onClick={stopRecording} disabled={!canStop} className="stop-btn">
          Stop
        </button>
      </div>
    </div>
  );
}

export default RecordingControls;
