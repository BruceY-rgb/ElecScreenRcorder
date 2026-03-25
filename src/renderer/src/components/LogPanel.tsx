import { useState, useEffect, useRef, useCallback } from 'react';

interface LogEntry {
  id: number;
  level: string;
  message: string;
  timestamp: string;
}

export default function LogPanel() {
  const [isOpen, setIsOpen] = useState(false);
  const [autoScroll, setAutoScroll] = useState(true);
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const logIdRef = useRef(0);
  const scrollRef = useRef<HTMLDivElement>(null);

  // Subscribe to native log events
  useEffect(() => {
    const handler = (data: { level: string; message: string }) => {
      const now = new Date();
      const timestamp = now.toLocaleTimeString('en-US', {
        hour12: false,
        hour: '2-digit',
        minute: '2-digit',
        second: '2-digit',
      });
      const id = ++logIdRef.current;
      setLogs((prev) => {
        const next = [...prev, { id, level: data.level, message: data.message, timestamp }];
        // Keep last 500 entries to prevent memory issues
        if (next.length > 500) {
          return next.slice(-500);
        }
        return next;
      });
    };

    window.electronAPI.onNativeLog(handler);
  }, []);

  // Auto-scroll to bottom when new logs arrive
  useEffect(() => {
    if (autoScroll && scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight;
    }
  }, [logs, autoScroll]);

  const handleClear = useCallback(() => {
    setLogs([]);
    logIdRef.current = 0;
  }, []);

  const getLevelClass = (level: string): string => {
    if (level === 'ERROR') return 'log-error';
    if (level === 'WARN') return 'log-warn';
    return 'log-info';
  };

  const errorCount = logs.filter((l) => l.level === 'ERROR').length;
  const warnCount = logs.filter((l) => l.level === 'WARN').length;

  return (
    <div className={`log-panel ${isOpen ? 'log-panel-open' : ''}`}>
      {/* Toggle bar */}
      <div className="log-header" onClick={() => setIsOpen((v) => !v)}>
        <span className="log-toggle-icon">{isOpen ? '▼' : '▶'}</span>
        <span className="log-title">日志</span>
        {errorCount > 0 && (
          <span className="log-badge log-badge-error">{errorCount}</span>
        )}
        {warnCount > 0 && (
          <span className="log-badge log-badge-warn">{warnCount}</span>
        )}
        <span className="log-count">({logs.length})</span>

        {isOpen && (
          <div className="log-controls" onClick={(e) => e.stopPropagation()}>
            <label className="log-checkbox">
              <input
                type="checkbox"
                checked={autoScroll}
                onChange={(e) => setAutoScroll(e.target.checked)}
              />
              自动滚动
            </label>
            <button className="log-btn" onClick={handleClear}>
              清空
            </button>
          </div>
        )}
      </div>

      {/* Log content */}
      {isOpen && (
        <div className="log-content" ref={scrollRef}>
          {logs.length === 0 && (
            <div className="log-empty">等待日志输入...</div>
          )}
          {logs.map((entry) => (
            <div key={entry.id} className={`log-entry ${getLevelClass(entry.level)}`}>
              <span className="log-time">{entry.timestamp}</span>
              <span className={`log-level ${getLevelClass(entry.level)}`}>
                [{entry.level}]
              </span>
              <span className="log-msg">{entry.message}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
