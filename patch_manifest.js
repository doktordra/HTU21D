const fs = require('fs');
let manifest = fs.readFileSync('app/android/app/src/main/AndroidManifest.xml', 'utf8');

const perms = `
    <!-- Permissions for BLE -->
    <uses-permission android:name="android.permission.BLUETOOTH" />
    <uses-permission android:name="android.permission.BLUETOOTH_ADMIN" />
    <uses-permission android:name="android.permission.ACCESS_COARSE_LOCATION" />
    <uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />
    <!-- For Android 12+ -->
    <uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation" />
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
`;

manifest = manifest.replace('</manifest>', perms + '\n</manifest>');
fs.writeFileSync('app/android/app/src/main/AndroidManifest.xml', manifest);
