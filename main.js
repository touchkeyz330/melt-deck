// Melt Deck - Electron main process.
// Opens the app window. All the DJ/audio work happens in src/ (the window).
const { app, BrowserWindow, session, shell, protocol, net } = require('electron');
const path = require('path');
const { pathToFileURL } = require('url');

// Serve the app's screens from app://meltdeck/ so audio recording and
// background track analysis are allowed (plain file:// blocks them).
protocol.registerSchemesAsPrivileged([
  { scheme: 'app', privileges: { standard: true, secure: true, supportFetchAPI: true, stream: true } },
]);
const SRC_DIR = path.join(__dirname, 'src');

function createWindow() {
  const win = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 1100,
    minHeight: 700,
    backgroundColor: '#0b0a0c',
    title: 'Melt Deck',
    autoHideMenuBar: true,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      // Lets audio start without an extra click first.
      autoplayPolicy: 'no-user-gesture-required',
    },
  });

  win.loadURL('app://meltdeck/index.html');

  // Open any web links in the normal browser, not inside the app.
  win.webContents.setWindowOpenHandler(({ url }) => {
    shell.openExternal(url);
    return { action: 'deny' };
  });
}

app.whenReady().then(() => {
  protocol.handle('app', (request) => {
    const { pathname } = new URL(request.url);
    const file = path.normalize(path.join(SRC_DIR, decodeURIComponent(pathname)));
    if (!file.startsWith(SRC_DIR)) return new Response('Not found', { status: 404 });
    return net.fetch(pathToFileURL(file).toString());
  });

  // When you stop a mix recording, ask where to save it.
  session.defaultSession.on('will-download', (_event, item) => {
    item.setSaveDialogOptions({
      title: 'Save your mix',
      defaultPath: path.join(app.getPath('music'), item.getFilename()),
    });
  });

  createWindow();

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') app.quit();
});
