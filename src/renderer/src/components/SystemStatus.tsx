import { SystemInfo } from '../hooks/useRecorder';

interface SystemStatusProps {
  systemInfo: SystemInfo | null;
}

function SystemStatus({ systemInfo }: SystemStatusProps) {
  if (!systemInfo) {
    return (
      <footer className="system-footer">
        <span className="system-badge">
          <span className="badge-label">Loading...</span>
        </span>
      </footer>
    );
  }

  return (
    <footer className="system-footer">
      <span className="system-badge">
        <span className="badge-label">Screen</span>
        <span className="badge-value">{systemInfo.screenWidth}x{systemInfo.screenHeight}</span>
      </span>
      <span className="system-badge">
        <span className="badge-label">CPU</span>
        <span className="badge-value">{systemInfo.cpuName}</span>
      </span>
      <span className="system-badge">
        <span className="badge-label">GPU</span>
        <span className="badge-value">{systemInfo.gpuName}</span>
      </span>
      <span className="system-badge">
        <span className="badge-label">RAM</span>
        <span className="badge-value">{systemInfo.ramGB}GB</span>
      </span>
    </footer>
  );
}

export default SystemStatus;
