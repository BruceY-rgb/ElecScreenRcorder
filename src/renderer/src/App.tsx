import { useRecorder } from './hooks/useRecorder';
import RecordingControls from './components/RecordingControls';
import SystemStatus from './components/SystemStatus';

function App() {
  const { status, error, systemInfo } = useRecorder();

  return (
    <div className="app">
      <header className="header">
        <h1>Screen Recording Tool</h1>
      </header>

      <main className="main">
        <SystemStatus systemInfo={systemInfo} />

        <RecordingControls />

        {error && <div className="error">{error}</div>}

        <div className="status">
          Status: {status.state}
          {status.state !== 'idle' && (
            <span> - Duration: {Math.floor(status.duration / 1000)}s</span>
          )}
        </div>
      </main>
    </div>
  );
}

export default App;
