import { useState } from 'react';
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
  } = useRecorder();

  const [config, setConfig] = useState<RecordingConfig>({
    resolution: { width: 2560, height: 1440 },
    fps: 60,
    savePath: '',
    separateAudio: true,
    remuxToMp4: false,
  });

  return (
    <div className="controls">
      <div className="config-section">
        <label>
          Resolution:
          <select
            value={`${config.resolution.width}x${config.resolution.height}`}
            onChange={(e) => {
              const [width, height] = e.target.value.split('x').map(Number);
              setConfig({ ...config, resolution: { width, height } });
            }}
          >
            <option value="1920x1080">1920x1080</option>
            <option value="2560x1440">2560x1440</option>
            <option value="3840x2160">3840x2160</option>
          </select>
        </label>

        <label>
          FPS:
          <select
            value={config.fps}
            onChange={(e) => setConfig({ ...config, fps: Number(e.target.value) })}
          >
            <option value="30">30</option>
            <option value="60">60</option>
            <option value="120">120</option>
          </select>
        </label>

        <label>
          <input
            type="checkbox"
            checked={config.separateAudio}
            onChange={(e) => setConfig({ ...config, separateAudio: e.target.checked })}
          />
          Separate Audio Track
        </label>
      </div>

      <div className="buttons">
        <button onClick={() => startRecording(config)} disabled={!canStart}>
          Start Recording
        </button>
        <button onClick={pauseRecording} disabled={!canPause}>
          Pause
        </button>
        <button onClick={resumeRecording} disabled={!canResume}>
          Resume
        </button>
        <button onClick={stopRecording} disabled={!canStop}>
          Stop
        </button>
      </div>
    </div>
  );
}

export default RecordingControls;
