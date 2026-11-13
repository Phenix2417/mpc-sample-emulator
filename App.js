// MPC Remote: full-screen control surface for the MPC Sample program running in the Pixel's Linux Terminal.
// The kit's helper (mpcctl.py, started by ./4-run.sh) serves the control surface on port 8765 and passes
// pads, buttons and knobs to the MPC; this app shows it full screen, clear of the camera.
// When the MPC isn't answering, the app opens the Linux Terminal once (with ./autostart.sh on, opening the
// Terminal starts the MPC) and keeps checking until it answers.
import { useCallback, useEffect, useRef, useState } from 'react';
import { ActivityIndicator, AppState, Pressable, StyleSheet, Text, TextInput, View } from 'react-native';
import * as IntentLauncher from 'expo-intent-launcher';
import { StatusBar } from 'expo-status-bar';
import { NavigationBar } from 'expo-navigation-bar';
import { useKeepAwake } from 'expo-keep-awake';
import { SafeAreaProvider, useSafeAreaInsets } from 'react-native-safe-area-context';
import { WebView } from 'react-native-webview';

const DEFAULT_ADDRESS = '127.0.0.1:8765';
const TERMINALS = ['com.android.virtualization.terminal', 'com.google.android.virtualization.terminal'];

async function reachable(address) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), 1500);
  try { const r = await fetch(`http://${address}/`, { signal: ctl.signal, cache: 'no-store' }); return r.ok; }
  catch { return false; }
  finally { clearTimeout(t); }
}

function openTerminal() {
  for (const pkg of TERMINALS) {
    try { IntentLauncher.openApplication(pkg); return true; } catch {}
  }
  for (const pkg of TERMINALS) {   // fallback: the Terminal's main screen by name
    try {
      IntentLauncher.startActivityAsync('android.intent.action.MAIN', {
        packageName: pkg, className: 'com.android.virtualization.terminal.MainActivity',
        category: 'android.intent.category.LAUNCHER', flags: 0x10000000,
      }).catch(() => {});
      return true;
    } catch {}
  }
  return false;
}

// The page keeps itself clear of the camera with env(safe-area-inset-top), and the app used to add its own
// gap as well (twice the space, and the pads shrank to fit). Now only the app's measured camera gap is used.
const fullScreen = (top) => `(function () {
  var s = document.getElementById('app-fs') || document.createElement('style');
  s.id = 'app-fs';
  s.textContent = '#app { padding-top: ${Math.round(top)}px !important; }';
  document.head.appendChild(s);
  window.dispatchEvent(new Event('resize'));
  setTimeout(function () { window.dispatchEvent(new Event('resize')); }, 300);
})(); true;`;

function Remote() {
  useKeepAwake();
  const insets = useSafeAreaInsets();
  const web = useRef(null);
  const [address, setAddress] = useState(DEFAULT_ADDRESS);
  const [draft, setDraft] = useState(DEFAULT_ADDRESS);
  const [state, setState] = useState('checking');   // checking | starting | ready | offline
  const [key, setKey] = useState(0);
  const opened = useRef(false);

  // check until the MPC answers; open the Terminal once if it doesn't
  useEffect(() => {
    if (state === 'ready') return undefined;
    let stop = false; let tries = 0;
    const tick = async () => {
      if (stop) return;
      if (await reachable(address)) { if (!stop) { setState('ready'); setKey((k) => k + 1); } return; }
      tries += 1;
      if (!opened.current && address === DEFAULT_ADDRESS) {
        opened.current = true;
        if (openTerminal()) setState('starting');
      }
      if (tries > 45) setState('offline');   // about 90 s
      if (!stop) setTimeout(tick, 2000);
    };
    tick();
    return () => { stop = true; };
  }, [state === 'ready', address]);

  // coming back to the app: check again straight away
  useEffect(() => {
    const sub = AppState.addEventListener('change', (s) => {
      if (s === 'active') setState((cur) => (cur === 'ready' ? cur : 'checking'));
    });
    return () => sub.remove();
  }, []);

  const lost = useCallback(() => setState('checking'), []);
  const connectTo = useCallback(() => { setAddress(draft.trim() || DEFAULT_ADDRESS); setState('checking'); }, [draft]);
  const startAgain = useCallback(() => { opened.current = false; setState('checking'); }, []);

  return (
    // the page fills the whole display; only its top is kept clear of the camera (set below, once)
    <View style={styles.root}>
      <StatusBar hidden style="light" />
      <NavigationBar hidden style="light" />
      {state !== 'ready' ? (
        <View style={styles.offline}>
          {state === 'offline' ? (
            <>
              <Text style={styles.title}>Can’t reach the MPC</Text>
              <Text style={styles.text}>
                Start it in the Linux Terminal app:{'\n'}
                <Text style={styles.code}>cd ~/mpc-pixel-kit && ./4-run.sh</Text>
                {'\n\n'}To have it start whenever the Terminal opens:{'\n'}
                <Text style={styles.code}>~/mpc-pixel-kit/autostart.sh on</Text>
                {'\n\n'}If the Terminal asks whether to allow port 8765, allow it.
              </Text>
            </>
          ) : (
            <>
              <ActivityIndicator size="large" color="#e8a33a" style={{ marginBottom: 18 }} />
              <Text style={styles.title}>{state === 'starting' ? 'Starting the MPC…' : 'Connecting…'}</Text>
              <Text style={styles.text}>
                {state === 'starting'
                  ? 'The Linux Terminal opened to start it. Come back here; the MPC appears once it is running (about 20 s).'
                  : `Looking for the MPC at ${address}.`}
              </Text>
            </>
          )}
          <TextInput style={styles.input} value={draft} onChangeText={setDraft} autoCapitalize="none"
                     autoCorrect={false} keyboardType="url" placeholder={DEFAULT_ADDRESS} placeholderTextColor="#666" />
          <Pressable style={styles.button} onPress={connectTo}><Text style={styles.buttonText}>Connect</Text></Pressable>
          <Pressable style={[styles.button, styles.second]} onPress={startAgain}><Text style={styles.buttonText}>Open the Terminal</Text></Pressable>
        </View>
      ) : (
        <WebView
          key={key}
          ref={web}
          source={{ uri: `http://${address}/` }}
          injectedJavaScript={fullScreen(insets.top)}
          style={styles.web}
          originWhitelist={['*']}
          javaScriptEnabled
          domStorageEnabled
          mixedContentMode="always"
          overScrollMode="never"
          bounces={false}
          scrollEnabled={false}
          setBuiltInZoomControls={false}
          textZoom={100}
          cacheEnabled={false}
          onError={lost}
          onHttpError={lost}
          onRenderProcessGone={lost}
        />
      )}
    </View>
  );
}

export default function App() {
  return (
    <SafeAreaProvider>
      <Remote />
    </SafeAreaProvider>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#000' },
  web: { flex: 1, backgroundColor: '#000' },
  offline: { flex: 1, justifyContent: 'center', padding: 28, backgroundColor: '#1b1d22' },
  title: { color: '#f2f2f2', fontSize: 22, fontWeight: '700', marginBottom: 14 },
  text: { color: '#9aa0aa', fontSize: 15, lineHeight: 22, marginBottom: 22 },
  code: { color: '#e8a33a', fontFamily: 'monospace' },
  input: { borderWidth: 1, borderColor: '#4a4e57', borderRadius: 8, color: '#f2f2f2', padding: 12, fontSize: 16, marginBottom: 14 },
  button: { backgroundColor: '#3d5aa8', borderRadius: 8, padding: 14, alignItems: 'center' },
  second: { backgroundColor: '#33363d', marginTop: 10 },
  buttonText: { color: '#fff', fontSize: 16, fontWeight: '700' },
});
