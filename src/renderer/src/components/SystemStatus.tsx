import { SystemInfo } from '../hooks/useRecorder';

interface SystemStatusProps {
  systemInfo: SystemInfo | null;
}

function SystemStatus({ systemInfo }: SystemStatusProps) {
  if (!systemInfo) {
    return <div className="system-status">Loading system info...</div>;
  }

  return (
    <div className="system-status">
      <h2>System Information</h2>
      <div className="info-grid">
        <div className="info-item">
          <span className="label">Screen:</span>
          <span className="value">
            {systemInfo.screenWidth}x{systemInfo.screenHeight}
          </span>
        </div>
        <div className="info-item">
          <span className="label">Refresh Rate:</span>
          <span className="value">{systemInfo.refreshRate}Hz</span>
        </div>
        <div className="info-item">
          <span className="label">DPI Scale:</span>
          <span className="value">{systemInfo.scalingFactor}x</span>
        </div>
        <div className="info-item">
          <span className="label">CPU:</span>
          <span className="value">{systemInfo.cpuName}</span>
        </div>
        <div className="info-item">
          <span className="label">GPU:</span>
          <span className="value">{systemInfo.gpuName}</span>
        </div>
        <div className="info-item">
          <span className="label">RAM:</span>
          <span className="value">{systemInfo.ramGB}GB</span>
        </div>
      </div>
    </div>
  );
}

export default SystemStatus;
