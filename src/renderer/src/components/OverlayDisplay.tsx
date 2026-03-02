import { useState, useEffect } from 'react';
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

  const getStatusText = () => {
    switch (status.state) {
      case 'recording':
        return 'Recording';
      case 'paused':
        return 'Paused';
      default:
        return 'Ready';
    }
  };

  const getStatusColor = () => {
    switch (status.state) {
      case 'recording':
        return '#ff4444';
      case 'paused':
        return '#ffaa00';
      default:
        return '#44ff44';
    }
  };

  return (
    <div className="overlay">
      <div className="status" style={{ color: getStatusColor() }}>
        {getStatusText()}
      </div>
      {status.state !== 'idle' && (
        <div className="duration">{formatDuration(status.duration)}</div>
      )}
    </div>
  );
}

export default OverlayDisplay;
