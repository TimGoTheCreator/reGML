Module.onRuntimeInitialized = function () {
  var status = document.getElementById('status');
  var picker = document.getElementById('pick-game');
  var gameRoot = '/games';

  try {
    Module.FS.mkdir('/persistent');
    Module.FS.mount(Module.IDBFS, {}, '/persistent');
    Module.FS.mkdir('/persistent/saves');
  } catch (error) {
    if (!error || error.code !== 'EEXIST') throw error;
  }

  Module.FS.syncfs(true, function (error) {
    if (error) {
      status.textContent = 'Could not open browser save storage: ' + error.message;
      return;
    }
    status.textContent = 'Choose a game folder to start.';
    picker.disabled = false;
    window.setInterval(function () {
      Module.FS.syncfs(false, function (syncError) {
        if (syncError) console.error('Could not save browser data:', syncError);
      });
    }, 5000);
  });

  picker.disabled = true;
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
      }

      var packageParts = (packageFile.webkitRelativePath || packageFile.name)
        .split('/').filter(Boolean);
      if (packageParts.length > 1) packageParts.shift();
      var packagePath = gameRoot + '/' + packageParts.join('/');
      status.textContent = 'Starting game…';
      picker.disabled = true;
      Module.callMain(['run', packagePath, '--save-dir', '/persistent/saves']);
      document.getElementById('canvas').focus();
    } catch (error) {
      status.textContent = 'Could not start game: ' + error.message;
      picker.disabled = false;
    }
  });
};
