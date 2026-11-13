// Lets the app see (and open) the Linux Terminal app on Android 11+ (package visibility).
const { withAndroidManifest } = require('expo/config-plugins');

const TERMINALS = ['com.android.virtualization.terminal', 'com.google.android.virtualization.terminal'];

module.exports = function withTerminalQuery(config) {
  return withAndroidManifest(config, (cfg) => {
    const m = cfg.modResults.manifest;
    m.queries = m.queries || [{}];
    const q = m.queries[0];
    q.package = q.package || [];
    for (const name of TERMINALS) {
      if (!q.package.some((p) => p.$['android:name'] === name)) q.package.push({ $: { 'android:name': name } });
    }
    return cfg;
  });
};
