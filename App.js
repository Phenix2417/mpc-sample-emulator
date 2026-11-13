// MPC Remote: full-screen control surface for the MPC Sample program running in the Pixel's Linux Terminal.
// The kit's helper (mpcctl.py, started by ./4-run.sh) serves the control surface on port 8765 and passes
// pads, buttons and knobs to the MPC; this app shows it full screen, clear of the camera.
import { useCallback, useRef, useState } from 'react';
import { Pressable, StyleSheet, Text, TextInput, View } from 'react-native';
import { StatusBar } from 'expo-status-bar';
import { NavigationBar } from 'expo-navigation-bar';
import { useKeepAwake } from 'expo-keep-awake';
import { SafeAreaProvider, useSafeAreaInsets } from 'react-native-safe-area-context';
import { WebView } from 'react-native-webview';

const DEFAULT_ADDRESS = '127.0.0.1:8765';

function Remote() {
  useKeepAwake();
  const insets = useSafeAreaInsets();
  const web = useRef(null);
  const [address, setAddress] = useState(DEFAULT_ADDRESS);
  const [draft, setDraft] = useState(DEFAULT_ADDRESS);
  const [failed, setFailed] = useState(false);
  const [key, setKey] = useState(0);

  const retry = useCallback(() => { setFailed(false); setKey((k) => k + 1); }, []);
  const connectTo = useCallback(() => { setAddress(draft.trim() || DEFAULT_ADDRESS); retry(); }, [draft, retry]);

  return (
    // the top inset keeps the MPC screen clear of the phone's camera
    <View style={[styles.root, { paddingTop: insets.top }]}>
      <StatusBar hidden style="light" />
      <NavigationBar hidden style="light" />
      {failed ? (
        <View style={styles.offline}>
          <Text style={styles.title}>Can’t reach the MPC</Text>
          <Text style={styles.text}>
            Start it in the Linux Terminal app:{'\n'}
            <Text style={styles.code}>cd ~/mpc-pixel-kit && ./4-run.sh</Text>
            {'\n\n'}If the Terminal asks whether to allow port 8765, allow it.
          </Text>
          <TextInput style={styles.input} value={draft} onChangeText={setDraft} autoCapitalize="none"
                     autoCorrect={false} keyboardType="url" placeholder={DEFAULT_ADDRESS} placeholderTextColor="#666" />
          <Pressable style={styles.button} onPress={connectTo}><Text style={styles.buttonText}>Connect</Text></Pressable>
        </View>
      ) : (
        <WebView
          key={key}
          ref={web}
          source={{ uri: `http://${address}/` }}
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
          onError={() => { setFailed(true); setTimeout(retry, 2000); }}
          onHttpError={() => { setFailed(true); }}
          onRenderProcessGone={retry}
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
  buttonText: { color: '#fff', fontSize: 16, fontWeight: '700' },
});
