// ESP Garden — sensor and actuator inventory.
//
// A real editor for what this board drives and reads: peripherals are declared
// in config.json and counted at runtime, so adding a relay or a probe is a row
// here plus a restart. There is no RELAY_COUNT or MOISTURE_SENSOR_COUNT to
// rebuild against any more.
//
// Every pin rule and every ceiling comes from GET /capabilities.json rather
// than being restated here. A page that hardcodes "GPIO 32-39 are analog" keeps
// saying so after the firmware changes, and the first sign of the drift is a
// channel reading noise — which is exactly what that endpoint exists to
// prevent.
(function () {
  var TEST_MS = 3000;

  var caps = null;    // GET /capabilities.json — pin rules and per-kind maxima
  var live = null;    // GET /data.json — live readings, matched at load time
  var doc = null;     // GET /config.json — posted back whole, io/moisture apart
  var model = null;   // the editable view of doc.io and doc.moisture
  var esc = null;     // bound in $() once auth.js has run
  var ready = false;  // false blocks every edit and the save button

  // The page state above is the only copy. devices_model.js and
  // devices_render.js read it through these getters rather than holding it,
  // because load() replaces caps, live, doc and model outright and a captured
  // reference would go on painting the previous device.
  var ctx = {
    caps: function () { return caps; },
    live: function () { return live; },
    doc: function () { return doc; },
    model: function () { return model; },
    ready: function () { return ready; },
  };

  // Imported under the names they had while this was one file.
  var M = espDevicesModel;
  var R = espDevicesRender;
  var SENSORS = M.SENSORS;
  var text = M.text;
  var plural = M.plural;
  var capsUsable = M.capsUsable;
  var pinAllowed = M.pinAllowed;
  var relayDefaultName = M.relayDefaultName;
  var probeDefaultName = M.probeDefaultName;
  var buildModel = M.buildModel;
  var takenPins = M.takenPins;
  var suggestedPin = M.suggestedPin;
  var buildDocument = M.buildDocument;
  var validate = M.validate;
  var renderRelays = R.renderRelays;
  var renderProbes = R.renderProbes;
  var renderSensors = R.renderSensors;
  var renderPinMap = R.renderPinMap;

  function render() {
    renderRelays();
    renderProbes();
    renderSensors();
    refresh();
  }

  // ---------- form state ----------

  function pinValue(raw) {
    var s = $.trim(String(raw === undefined || raw === null ? '' : raw));
    if (s === '') return null;
    var v = parseInt(s, 10);
    return isNaN(v) ? null : v;
  }

  // DOM back into the model. Runs before every structural change and before
  // every save, so an edit in progress is never dropped by a re-render.
  function collect() {
    if (!model) return;

    $('.rl-name').each(function () {
      model.relays[$(this).data('row')].name = $.trim(String($(this).val()));
    });
    $('.rl-pin').each(function () {
      model.relays[$(this).data('row')].pin = pinValue($(this).val());
    });
    $('.rl-on').each(function () {
      model.relays[$(this).data('row')].on =
        (parseInt($(this).val(), 10) === 1) ? 1 : 0;
    });

    $('.ms-name').each(function () {
      model.probes[$(this).data('row')].name = $.trim(String($(this).val()));
    });
    $('.ms-pin').each(function () {
      model.probes[$(this).data('row')].pin = pinValue($(this).val());
    });
    $('.ms-dry').each(function () {
      model.probes[$(this).data('row')].dry = $.trim(String($(this).val()));
    });
    $('.ms-wet').each(function () {
      model.probes[$(this).data('row')].wet = $.trim(String($(this).val()));
    });
    $('.ms-kind').each(function () {
      model.probes[$(this).data('row')].kind = $.trim(String($(this).val()));
    });
    $('.ms-invert').each(function () {
      model.probes[$(this).data('row')].invert = $(this).is(':checked');
    });
    $('.ms-power').each(function () {
      model.probes[$(this).data('row')].powerPin = pinValue($(this).val());
    });
    $('.ms-settle').each(function () {
      model.probes[$(this).data('row')].settleMs = $.trim(String($(this).val()));
    });
    $('.ms-relay').each(function () {
      var relay = parseInt($(this).val(), 10);
      model.probes[$(this).data('row')].relay = isNaN(relay) ? -1 : relay;
    });

    $('.sn-fit').each(function () {
      model.sensors[$(this).data('key')].fitted = $(this).prop('checked');
    });
    $('.sn-name').each(function () {
      model.sensors[$(this).data('key')].name = $.trim(String($(this).val()));
    });
    $('.sn-pin').each(function () {
      model.sensors[$(this).data('key')].pin = pinValue($(this).val());
    });
    $('.sn-ppl').each(function () {
      model.sensors.flow.pulsesPerLitre = $.trim(String($(this).val()));
    });
    $('.sn-level').each(function () {
      model.sensors.floatSwitch.activeLevel =
        (parseInt($(this).val(), 10) === 1) ? 1 : 0;
    });
    $('.sn-interlock').each(function () {
      model.sensors.floatSwitch.interlock = $(this).prop('checked');
    });
    $('.sn-fill').each(function () {
      model.sensors.floatSwitch.fillRelay = parseInt($(this).val(), 10);
    });
  }

  // Runs on every edit and returns the report, so the save handler and the
  // warning list can never disagree about what is wrong.
  function refresh() {
    if (!model) return null;
    var report = validate();

    $('#tbody-relays, #tbody-probes, #tbody-sensors')
      .find('.is-invalid').removeClass('is-invalid');
    $.each(report.flagged, function (selector) {
      $(selector).addClass('is-invalid');
    });

    renderPinMap(report);

    var html = '';
    if (report.problems.length) {
      html += '<div class="alert alert-danger hint"><strong>These have to be ' +
              'fixed before saving.</strong><ul class="mb-0">';
      $.each(report.problems, function (_, problem) {
        html += '<li>' + esc(problem) + '</li>';
      });
      html += '</ul></div>';
    }
    if (report.warnings.length) {
      html += '<div class="alert alert-warning hint"><strong>Allowed, but worth ' +
              'reading.</strong><ul class="mb-0">';
      $.each(report.warnings, function (_, warning) {
        html += '<li>' + esc(warning) + '</li>';
      });
      html += '</ul></div>';
    }
    $('#warnings').html(html);

    $('#button-save').prop('disabled', !ready || report.problems.length > 0);
    return report;
  }

  // ---------- transport ----------

  function blockEditing(message) {
    ready = false;
    model = null;
    // 8 spans the widest of the three tables (probes); a short colspan just
    // leaves a ragged cell, which reads as a rendering bug in the one state
    // that is supposed to look deliberate.
    var empty = '<tr><td colspan="8" class="text-muted small">Editing is ' +
                'blocked until the page loads completely.</td></tr>';
    $('#tbody-relays, #tbody-probes, #tbody-sensors').html(empty);
    $('#pin-map').empty();
    $('#warnings').empty();
    $('#caps-note').empty();
    $('#button-save, #button-add-relay, #button-add-probe').prop('disabled', true);
    espUI.setStatus('danger', message);
  }

  // Nothing is editable until all three payloads are in. Rendering the form
  // from half a model is how a save rewrites fields the user never touched: an
  // unread capability list would offer every GPIO, and an unread /data.json
  // would show every peripheral as missing.
  function load(keepStatus) {
    ready = false;
    $('#button-save, #button-add-relay, #button-add-probe').prop('disabled', true);
    if (!keepStatus) espUI.setStatus('info', 'Loading…');

    $.getJSON('/capabilities.json')
      .done(function (payload) {
        if (!capsUsable(payload)) {
          blockEditing('/capabilities.json did not return the pin rules this ' +
                       'page needs. Reload before editing — without them every ' +
                       'GPIO would look valid.');
          return;
        }
        caps = payload;
        loadLive(keepStatus);
      })
      .fail(function (xhr) {
        blockEditing('Could not read /capabilities.json (' +
                     (xhr.status || 'no response') + '). Editing is blocked: ' +
                     'the pin rules come from the device, and guessing them ' +
                     'here is how a probe ends up on ADC2.');
      });
  }

  function loadLive(keepStatus) {
    $.getJSON('/data.json')
      .done(function (payload) {
        live = payload;
        loadConfig(keepStatus);
      })
      .fail(function (xhr) {
        blockEditing('Could not read /data.json (' + (xhr.status || 'no response') +
                     '). Editing is blocked: without it this page cannot tell ' +
                     'which peripherals are actually running.');
      });
  }

  function loadConfig(keepStatus) {
    $.getJSON('/config.json')
      .done(function (payload) {
        doc = payload;
        model = buildModel();
        ready = true;
        $('#caps-note').html(
          'Firmware ' + esc(text(caps.firmware) || '?') + ' &middot; up to ' +
          esc(plural(caps.relayMax, 'relay')) + ' and ' +
          esc(plural(caps.moistureMax, 'moisture probe')) + ' &middot; kinds: ' +
          esc(caps.kinds.join(', ')));
        render();
        if (!keepStatus) $('#status').empty();
      })
      .fail(function (xhr) {
        blockEditing(xhr.status === 403
          ? 'This account is not an administrator, so the configuration cannot be read.'
          : 'Could not read the configuration (' + (xhr.status || 'no response') + ').');
      });
  }

  function save() {
    collect();
    var report = refresh();
    if (report === null) return;
    if (report.problems.length) {
      espUI.setStatus('danger', 'Not saved — ' + report.problems.join(' '));
      return;
    }

    // Confirmed like the other destructive actions: the write replaces the
    // whole file, and the device applies it at the next boot.
    var question = report.warnings.length
      ? report.warnings.join('\n\n') + '\n\nSave anyway?'
      : 'Write this device list to config.json? It takes effect at the next restart.';
    if (!confirm(question)) return;

    $('#button-save').prop('disabled', true);
    $.post('/config.json', { config: JSON.stringify(buildDocument()) })
      .done(function () {
        load(true);
        espUI.setStatus('success', 'Saved. Restart the device to apply.');
      })
      .fail(function (xhr) {
        espUI.setStatus('danger', 'Save failed: ' + (xhr.responseText || xhr.status));
        $('#button-save').prop('disabled', false);
      });
  }

  $(function () {
    if (!espAuth.requireToken()) return;
    esc = espUI.escapeHtml;
    espDevicesModel.use(ctx);
    espDevicesRender.use(ctx);
    load();

    // Every field except the fitted box repaints the warnings in place. A
    // re-render on each keystroke would move the caret out of the field being
    // typed into. `input change` covers both: a checkbox does not fire `input`
    // everywhere, and a <select> does not fire it at all.
    $('#tbody-relays, #tbody-probes, #tbody-sensors')
      .on('input change', 'input:not(.sn-fit), select', function () {
        collect();
        refresh();
      });

    // Fitting or removing a sensor changes which of its fields are live, so
    // that one does re-render — and it lands the new sensor on a free pin
    // rather than on top of something already there.
    $('#tbody-sensors').on('change', '.sn-fit', function () {
      collect();
      var key = $(this).data('key');
      var entry = model.sensors[key];
      var spec = null;
      $.each(SENSORS, function (_, candidate) {
        if (candidate.key === key) spec = candidate;
      });
      if (entry.fitted && spec) {
        var taken = takenPins(key);
        if (entry.pin === null || taken[entry.pin] ||
            !pinAllowed(spec.role, entry.pin)) {
          entry.pin = suggestedPin(spec, key);
        }
      }
      renderSensors();
      refresh();
    });

    $('#button-add-relay').on('click', function () {
      collect();
      if (model.relays.length >= caps.relayMax) return;
      // No pin is proposed: the page cannot know where the relay is wired, and
      // the first free output GPIO is the serial TX line.
      model.relays.push({ name: '', pin: null, on: 0, liveIndex: null });
      render();
    });

    $('#tbody-relays').on('click', '.btn-del-relay', function () {
      var row = $(this).data('row');
      collect();
      var name = model.relays[row].name || relayDefaultName(row);
      if (!confirm('Delete relay ' + row + ' (' + name + ')?\n\n' +
                   'Every relay below it moves up one index, and TalkBack, ' +
                   '/control and the schedules all address relays by index.')) {
        return;
      }
      model.relays.splice(row, 1);

      // Every probe fed by a relay BELOW the deleted one now points at the
      // wrong pump, and the one fed by the deleted relay points at nothing.
      // Left unremapped, the classifier keeps training probe N against
      // waterings of a zone it is not in — silently, because the model is a
      // slow average and nothing about it looks broken.
      $.each(model.probes, function (_, probe) {
        if (typeof probe.relay !== 'number' || probe.relay < 0) return;
        if (probe.relay === row) {
          probe.relay = -1;
        } else if (probe.relay > row) {
          probe.relay -= 1;
        }
      });

      render();
    });

    $('#button-add-probe').on('click', function () {
      collect();
      if (model.probes.length >= caps.moistureMax) return;
      // Its calibration pair is created with it, and stays at its index: the
      // moisture array is rebuilt from this list on every save.
      model.probes.push({
        // -1, not the row index: a probe added before its pump exists would
        // otherwise claim a relay it is not plumbed to, and the classifier
        // would label its readings against the wrong waterings.
        name: '', pin: null, dry: '0', wet: '0', relay: -1, liveKey: null,
      });
      render();
    });

    $('#tbody-probes').on('click', '.btn-del-probe', function () {
      var row = $(this).data('row');
      collect();
      var name = model.probes[row].name ||
                 probeDefaultName(row, model.probes.length);
      if (!confirm('Delete probe ' + (row + 1) + ' (' + name + ')?\n\n' +
                   'Its dry/wet calibration is deleted with it.')) {
        return;
      }
      model.probes.splice(row, 1);
      render();
    });

    $('#tbody-relays').on('click', '.btn-test', function () {
      var index = $(this).data('live');
      var name = String($(this).data('name'));
      var seconds = TEST_MS / 1000;
      if (index === '' || index === undefined) return;

      // A relay here is a pump or a valve, so the click that moves water is
      // confirmed, and the burst is a constant rather than something typed.
      if (!confirm('Switch ' + name + ' on for ' + seconds + ' seconds?\n\n' +
                   'This uses the pin the device is RUNNING on, not the one ' +
                   'selected above.')) {
        return;
      }

      $.post('/control', { relay: index, relayTime: TEST_MS })
        .done(function () {
          espUI.setStatus('success', name + ' switched on for ' + seconds + ' s.');
        })
        .fail(function (xhr) {
          espUI.setStatus('danger', xhr.status === 403
            ? 'Switching a relay needs an OPERATOR or ADMIN session.'
            : 'Relay test failed (' + (xhr.status || 'no response') + ').');
        });
    });

    $('#button-save').on('click', save);
    $('#button-reload').on('click', function () { load(); });

    $('#button-restart').on('click', function () {
      if (!confirm('Restart the device now?')) return;
      espUI.setStatus('info', 'Restarting…');
      $.post('/control', { reset: '1' })
        .done(function () {
          espUI.setStatus('info', 'Restarting… reload the page in a few seconds.');
        })
        .fail(function (xhr) {
          espUI.setStatus('danger', xhr.status === 403
            ? 'Restart needs an OPERATOR or ADMIN session.'
            : 'Restart request failed (' + (xhr.status || 'no response') + ').');
        });
    });

    $('#a-logout').on('click', function (event) {
      event.preventDefault();
      espAuth.logout();
    });
  });
})();
