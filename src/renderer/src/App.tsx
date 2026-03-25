import { useRecorder, SystemInfo } from './hooks/useRecorder';
import RecordingControls from './components/RecordingControls';
import SystemStatus from './components/SystemStatus';
import LogPanel from './components/LogPanel';

function App() {
  const { error, systemInfo } = useRecorder();

  return (
    <div className="app">
      <header className="header">
        <h1>Screen Recorder</h1>
      </header>

      <main className="main">
        {error && <div className="error">{error}</div>}
        <RecordingControls systemInfo={systemInfo} />
      </main>

      <SystemStatus systemInfo={systemInfo} />
      <LogPanel />
    </div>
  );
}

export default App;
