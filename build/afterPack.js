// Runs during the Mac build. Gives the app a free "ad-hoc" signature so
// Apple Silicon Macs don't call it "damaged". (It is still not notarized,
// so the first launch needs right-click > Open. See README.)
const { execSync } = require('child_process');
const path = require('path');

exports.default = async function afterPack(context) {
  if (context.electronPlatformName !== 'darwin') return;
  const appName = context.packager.appInfo.productFilename;
  const appPath = path.join(context.appOutDir, `${appName}.app`);
  console.log(`Ad-hoc signing ${appPath}`);
  execSync(`codesign --force --deep --sign - "${appPath}"`, { stdio: 'inherit' });
};
