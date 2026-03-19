import { useState, useEffect, useCallback } from 'react';
import type { RecordingStatus } from '../hooks/useRecorder';

function formatDuration(ms: number): string {
  const seconds = Math.floor(ms / 1000);
  const minutes = Math.floor(seconds / 60);
  const hours = Math.floor(minutes / 60);

  if (hours > 0) {
    return `${hours}:${String(minutes % 60).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`;
  }
  return `${minutes}:${String(seconds % 60).padStart(2, '0')}`;
}

function OverlayDisplay() {
  const [status, setStatus] = useState<RecordingStatus>({
    state: 'idle',
    duration: 0,
    frameCount: 0,
  });

  useEffect(() => {
    window.electronAPI.onRecordingStatus(setStatus);
  }, []);

  const handlePause = useCallback(() => {
    window.electronAPI.pauseRecording().catch(console.error);
  }, []);

  const handleResume = useCallback(() => {
    window.electronAPI.resumeRecording().catch(console.error);
  }, []);

  const handleStop = useCallback(() => {
    window.electronAPI.stopRecording().catch(console.error);
  }, []);

  const isRecording = status.state === 'recording';
  const isPaused = status.state === 'paused';

  return (
    <div className="overlay">
      <div className="overlay-indicator" style={{
        background: isRecording ? '#ff4444' : isPaused ? '#ffaa00' : '#44ff44',
      }} />
      <div className="overlay-info">
        <div className="overlay-status" style={{
          color: isRecording ? '#ff4444' : isPaused ? '#ffaa00' : '#44ff44',
        }}>
          {isRecording ? 'REC' : isPaused ? 'PAUSED' : 'READY'}
        </div>
        <div className="overlay-duration">
          {formatDuration(status.duration)}
        </div>
        {(isRecording || isPaused) && (
          <div className="overlay-mouse">
            {status.mouseActivity || 0} ev/s
          </div>
        )}
      </div>
      <div className="overlay-buttons">
        {isRecording && (
          <button className="overlay-btn pause-btn" onClick={handlePause} title="Pause">
            &#9646;&#9646;
          </button>
        )}
        {isPaused && (
          <button className="overlay-btn resume-btn" onClick={handleResume} title="Resume">
            &#9654;
          </button>
        )}
        <button className="overlay-btn stop-btn" onClick={handleStop} title="Stop">
          &#9632;
        </button>
      </div>
    </div>
  );
}

export default OverlayDisplay;
