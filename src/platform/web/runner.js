Module.onRuntimeInitialized = function () {
  Module.runnerReady = true;
  var status = document.getElementById('status');
  var picker = document.getElementById('pick-game');
  var gameRoot = '/games';
  var gameStarted = false;
  var saveStorageReady = false;

  try {
    Module.FS.mkdir('/persistent');
    Module.FS.mount(IDBFS, {}, '/persistent');
    Module.FS.mkdir('/persistent/saves');
  } catch (error) {
    if (!error || error.code !== 'EEXIST') {
      console.error('Browser save storage is unavailable:', error);
      status.textContent = 'Save storage unavailable; the game can still run.';
      try { Module.FS.mkdir('/persistent/saves'); } catch (_) {}
    }
  }

  picker.disabled = false;
  if (!status.textContent.startsWith('Save storage unavailable'))
    status.textContent = 'Choose a game folder to start.';

  try {
    Module.FS.syncfs(true, function (error) {
      if (error) {
        console.error('Could not open browser save storage:', error);
        if (!gameStarted)
          status.textContent = 'Save storage unavailable; the game can still run.';
        return;
      }

      saveStorageReady = true;
      if (!gameStarted)
        status.textContent = 'Choose a game folder to start.';
      window.setInterval(function () {
        if (!saveStorageReady) return;
        Module.FS.syncfs(false, function (syncError) {
          if (syncError) console.error('Could not save browser data:', syncError);
        });
      }, 5000);
    });
  } catch (error) {
    console.error('Could not open browser save storage:', error);
    status.textContent = 'Save storage unavailable; the game can still run.';
  }

  picker.addEventListener('change', async function () {
    var files = Array.from(picker.files || []);
    var packageFile = files.find(function (file) {
      return file.name.toLowerCase() === 'data.win';
    });
    if (!packageFile) {
      status.textContent = 'The selected folder does not contain data.win.';
      return;
    }

    status.textContent = 'Copying game files…';
    try {
      try { Module.FS.rmdir(gameRoot); } catch (_) {}
      Module.FS.mkdir(gameRoot);
      var totalBytes = files.reduce(function (total, file) { return total + file.size; }, 0);
      var copiedBytes = 0;
      for (var file of files) {
        var relative = file.webkitRelativePath || file.name;
        var parts = relative.split('/').filter(Boolean);
        if (parts.length > 1) parts.shift();
        var parent = gameRoot;
        for (var i = 0; i < parts.length - 1; i++) {
          parent += '/' + parts[i];
          try { Module.FS.mkdir(parent); } catch (_) {}
        }
        var bytes = new Uint8Array(await file.arrayBuffer());
        Module.FS.writeFile(parent + '/' + parts[parts.length - 1], bytes);
        copiedBytes += file.size;
        var percent = totalBytes ? Math.floor(copiedBytes * 100 / totalBytes) : 100;
        status.textContent = 'Copying game files… ' + percent + '%';
      }

      var packageParts = (packageFile.webkitRelativePath || packageFile.name)
        .split('/').filter(Boolean);
      if (packageParts.length > 1) packageParts.shift();
      var packagePath = gameRoot + '/' + packageParts.join('/');
      status.textContent = 'Starting game…';
      gameStarted = true;
      Module.gameStarted = true;
      Module.reportedError = false;
      picker.disabled = true;
      var exitCode = Module.callMain(['run', packagePath, '--save-dir', '/persistent/saves']);
      if (exitCode !== undefined && exitCode !== 0) {
        gameStarted = false;
        Module.gameStarted = false;
        picker.disabled = false;
        if (!Module.reportedError)
          status.textContent = 'Game exited with code ' + exitCode + '.';
      }
      document.getElementById('canvas').focus();
    } catch (error) {
      gameStarted = false;
      Module.gameStarted = false;
      if (!Module.reportedError) {
        var message = error && error.message ? error.message : String(error);
        status.textContent = 'Could not start game: ' + message;
      }
      picker.disabled = false;
    }
  });
};
