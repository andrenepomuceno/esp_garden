// ESP Garden — sensor and actuator inventory.
//
// Renames and re-pins the devices the running firmware was built with, and
// nothing else. There is deliberately no add or delete: RELAY_COUNT and
// MOISTURE_SENSOR_COUNT are compile-time, so an entry past the compiled count is
// ignored by the device and a missing one silently keeps a compiled default —
// either button would report a change that never happened.
(function () {
  // ADC2 cannot be sampled while WiFi is on, which is every moment of this
  // device's life, so an analog pin has to be an ADC1 channel: GPIO 32-39.
  var ADC1_MIN = 32;
  var ADC1_MAX = 39;
  var GPIO_MAX = 39;
  var TEST_MS = 3000;

  var original = null;  // last GET /config.json — the template every save starts from
  var live = null;      // last GET /data.json — the only source of the compiled counts
  var rows = [];        // device models, rebuilt by render()
  var esc = null;       // bound in $() once auth.js has run

  function isArray(v) {
    return Object.prototype.toString.call(v) === '[object Array]';
  }

  function isPlainObject(v) {
    return v !== null && typeof v === 'object' && !isArray(v);
  }

  function has(obj, key) {
    return Object.prototype.hasOwnProperty.call(obj, key);
  }

  function io() {
    return (original && isPlainObject(original.io)) ? original.io : {};
  }

  // ---------- reading config.json ----------

  // io.dht, io.luminosity and io.waterLevel are a bare pin number on a config
  // written before naming existed, or {pin, name}. loadSensor() in config.cpp
  // still accepts both, so this page has to read both.
  function readSensor(node) {
    if (typeof node === 'number') return { pin: node, name: '' };
    if (isPlainObject(node)) {
      return {
        pin: (typeof node.pin === 'number') ? node.pin : null,
        name: (typeof node.name === 'string') ? node.name : '',
      };
    }
    return null;
  }

  function readRelay(node) {
    if (typeof node === 'number') return { pin: node, name: '', on: 0, entry: {} };
    if (isPlainObject(node)) {
      return {
        pin: (typeof node.pin === 'number') ? node.pin : null,
        name: (typeof node.name === 'string') ? node.name : '',
        on: (typeof node.on === 'number') ? node.on : 0,
        entry: node,
      };
    }
    return null;
  }

  // Mirrors the compiled defaults in ConfigFile::ConfigFile(), so the
  // placeholder shows the label the device actually falls back to.
  function relayDefaultName(index) {
    return (index === 0) ? 'Watering' : ('Relay ' + (index + 1));
  }

  // ---------- reading data.json ----------

  function liveRelays() {
    return (live && isArray(live.Relays)) ? live.Relays : [];
  }

  function liveInputs() {
    return (live && isPlainObject(live.Inputs)) ? live.Inputs : {};
  }

  // A single probe keeps the unsuffixed "Soil Moisture" label, so the compiled
  // probe count is the number of matching keys, not the highest suffix.
  function liveMoistureCount() {
    var n = 0;
    $.each(liveInputs(), function (key) {
      if (key.indexOf('Soil Moisture') === 0) n++;
    });
    return n;
  }

  function moistureLabel(index, count) {
    return (count === 1) ? 'Soil Moisture' : ('Soil Moisture ' + (index + 1));
  }

  // ---------- device models ----------

  // One model per device. `stored` is the config.json entry it came from (null
  // when the firmware has the device but the config never mentioned it) and
  // `live` its /data.json view (null when config.json declares more devices than
  // the firmware was built with). Both directions happen in the field and need
  // different wording, so neither is treated as an error.
  function describeDevices() {
    var conf = io();
    var out = [];
    var i;

    var storedRelays = [];
    if (isArray(conf.relays)) {
      $.each(conf.relays, function (_, entry) { storedRelays.push(readRelay(entry)); });
    } else if (typeof conf.watering === 'number') {
      // Pre-2.0 spelling: one relay as io.watering + io.wateringOn, with nowhere
      // to keep a name. Saving migrates it to the array form loadRelays()
      // prefers; the old keys are left alone, since they are then no longer read.
      storedRelays.push({
        pin: conf.watering,
        name: '',
        on: (typeof conf.wateringOn === 'number') ? conf.wateringOn : 0,
        entry: {},
      });
    }

    var relaysLive = liveRelays();
    var relayCount = Math.max(storedRelays.length, relaysLive.length);
    for (i = 0; i < relayCount; i++) {
      var stored = storedRelays[i] || null;
      var running = relaysLive[i] || null;
      var name = (stored && stored.name) ? stored.name
        : (running ? String(running.name) : '');
      out.push({
        id: 'relay' + i,
        group: 'relay',
        index: i,
        title: name || relayDefaultName(i),
        placeholder: relayDefaultName(i),
        pin: stored ? stored.pin : null,
        name: name,
        on: stored ? stored.on : null,
        stored: stored,
        live: running,
        analog: false,
        output: true,
      });
    }

    var storedProbes = [];
    if (isArray(conf.soilMoisture)) {
      $.each(conf.soilMoisture, function (_, entry) { storedProbes.push(readSensor(entry)); });
    } else if (readSensor(conf.soilMoisture)) {
      storedProbes.push(readSensor(conf.soilMoisture));
    }

    var probesLive = liveMoistureCount();
    var probeCount = Math.max(storedProbes.length, probesLive);
    for (i = 0; i < probeCount; i++) {
      var probe = storedProbes[i] || null;
      var label = moistureLabel(i, probesLive);
      out.push({
        id: 'moisture' + i,
        group: 'moisture',
        index: i,
        title: (probe && probe.name) || moistureLabel(i, probeCount),
        placeholder: moistureLabel(i, probeCount),
        pin: probe ? probe.pin : null,
        name: probe ? probe.name : '',
        stored: probe,
        live: (i < probesLive) ? liveInputs()[label] : null,
        analog: true,
        output: false,
      });
    }

    // A scalar sensor gets a row when config.json carries its key or the running
    // firmware reports its reading. A board built without one therefore shows no
    // row, and the page never invents an io.* key the device would then honour.
    var scalars = [
      { key: 'dht', title: 'DHT11', analog: false, inputs: ['Temperature', 'Air Humidity'] },
      { key: 'luminosity', title: 'Luminosity', analog: true, inputs: ['Luminosity'] },
      { key: 'waterLevel', title: 'Water level', analog: true, inputs: ['Water Level'] },
    ];

    $.each(scalars, function (_, spec) {
      var sensor = readSensor(conf[spec.key]);
      var inputs = liveInputs();
      var reading = null;
      $.each(spec.inputs, function (_, key) {
        if (reading === null && has(inputs, key)) reading = inputs[key];
      });
      if (!sensor && reading === null) return;

      out.push({
        id: spec.key,
        group: spec.key,
        index: null,
        title: (sensor && sensor.name) || spec.title,
        placeholder: spec.title,
        pin: sensor ? sensor.pin : null,
        name: sensor ? sensor.name : '',
        stored: sensor,
        live: reading,
        analog: spec.analog,
        output: false,
      });
    });

    // Not editable here, but it occupies a GPIO: without this row a relay parked
    // on the button's pin would show no conflict on this page while the device
    // logs one at every boot.
    if (typeof conf.button === 'number') {
      out.push({
        id: 'button',
        group: 'button',
        index: null,
        title: 'Button',
        placeholder: 'Button',
        pin: conf.button,
        name: '',
        stored: { pin: conf.button, name: '' },
        live: null,
        analog: false,
        output: false,
        readOnly: true,
      });
    }

    return out;
  }

  // ---------- validation ----------

  function groupValues(entries, group) {
    var list = [];
    $.each(entries, function (_, entry) {
      if (entry.row.group === group) list.push(entry);
    });
    return list;
  }

  // A blank pin means "leave this device alone", never GPIO 0. Which blanks are
  // allowed differs by device: loadSensor() checks for the "pin" key and keeps
  // the compiled default when it is absent, while loadRelays() casts a missing
  // one straight to int and parks the relay on GPIO 0.
  function markBlanks(entries) {
    $.each(entries, function (_, entry) {
      entry.skip = (!entry.row.readOnly && entry.raw === '' && !entry.row.stored);
    });

    // io.relays and io.soilMoisture are written as dense arrays, so only a run
    // of blanks at the END of a group can be left out — the firmware keeps its
    // compiled default for a missing trailing entry. A blank in the middle would
    // be written as a hole and read back as GPIO 0.
    $.each(['relay', 'moisture'], function (_, group) {
      var list = groupValues(entries, group);
      var trailing = true;
      for (var i = list.length - 1; i >= 0; i--) {
        if (trailing && list[i].skip) continue;
        trailing = false;
        list[i].skip = false;
      }
    });
  }

  // The conditions ConfigFile::validatePins() logs at boot, checked before the
  // reboot that would apply them: a probe moved onto ADC2 reads 0 forever rather
  // than failing, and the log that says so is only reachable once the device is
  // back up on the pin that broke it.
  function checkPins(entries) {
    var invalid = [];
    var warnings = [];
    var flagged = {};
    var byPin = {};

    $.each(entries, function (_, entry) {
      entry.pin = null;
      if (entry.skip) return;

      if (entry.raw === '') {
        if (entry.row.output) {
          invalid.push(entry.owner);
          flagged[entry.id] = true;
        }
        return;
      }

      // parseInt('12abc') is 12, and a pin sent as a string is read as 0 by the
      // device — so the field has to be digits end to end before it is parsed.
      if (!/^[0-9]+$/.test(entry.raw)) {
        invalid.push(entry.owner);
        flagged[entry.id] = true;
        return;
      }

      var pin = parseInt(entry.raw, 10);
      if (isNaN(pin) || pin > GPIO_MAX) {
        invalid.push(entry.owner);
        flagged[entry.id] = true;
        return;
      }

      entry.pin = pin;
      if (!byPin[pin]) byPin[pin] = [];
      byPin[pin].push(entry);
    });

    $.each(entries, function (_, entry) {
      if (typeof entry.pin !== 'number') return;

      if (entry.row.output && entry.pin >= 34 && entry.pin <= 39) {
        warnings.push(entry.owner + ': GPIO ' + entry.pin +
                      ' is input-only and cannot drive a relay.');
        flagged[entry.id] = true;
      }
      if (entry.row.analog && (entry.pin < ADC1_MIN || entry.pin > ADC1_MAX)) {
        warnings.push(entry.owner + ': GPIO ' + entry.pin + ' is not an ADC1 ' +
                      'channel. ADC2 cannot be read while WiFi is on, so this ' +
                      'reads 0.');
        flagged[entry.id] = true;
      }
      if (entry.row.analog && (entry.pin === 37 || entry.pin === 38)) {
        warnings.push(entry.owner + ': GPIO ' + entry.pin + ' is an ADC1 channel ' +
                      'on the die but is not bonded out on a WROOM-32 module.');
        flagged[entry.id] = true;
      }
    });

    $.each(byPin, function (pin, group) {
      if (group.length < 2) return;
      var owners = [];
      $.each(group, function (_, entry) {
        owners.push(entry.owner);
        flagged[entry.id] = true;
      });
      warnings.push('GPIO ' + pin + ' is assigned to ' + owners.join(' and ') + '.');
    });

    return { invalid: invalid, warnings: warnings, flagged: flagged };
  }

  // ---------- writing config.json ----------

  // Always the object form. A bare number cannot carry the name, and an entry
  // with no "pin" key is how "keep the compiled default" is spelled — writing
  // pin: 0 instead would move the peripheral to a strapping pin.
  function sensorEntry(entry) {
    var out = {};
    if (typeof entry.pin === 'number') out.pin = entry.pin;
    if (entry.name) out.name = entry.name;
    return out;
  }

  function buildDocument(entries) {
    // Starts from the document the device served, so every key this page does
    // not render — including the ******** secrets, which POST /config.json
    // restores from disk — is posted back exactly as it arrived.
    var doc = JSON.parse(JSON.stringify(original));
    if (!isPlainObject(doc.io)) doc.io = {};

    var relays = [];
    $.each(groupValues(entries, 'relay'), function (_, entry) {
      if (entry.skip) return;
      // Any key this page does not render — today only "on" — is carried over
      // from the stored entry rather than defaulted, so editing a name here
      // cannot quietly re-invert an active-high relay.
      var out = (entry.row.stored && isPlainObject(entry.row.stored.entry))
        ? JSON.parse(JSON.stringify(entry.row.stored.entry))
        : {};
      out.pin = entry.pin;
      if (typeof out.on !== 'number') {
        out.on = (typeof entry.row.on === 'number') ? entry.row.on : 0;
      }
      if (entry.name) out.name = entry.name; else delete out.name;
      relays.push(out);
    });
    if (relays.length) doc.io.relays = relays;

    var probes = [];
    $.each(groupValues(entries, 'moisture'), function (_, entry) {
      if (entry.skip) return;
      probes.push(sensorEntry(entry));
    });
    if (probes.length) doc.io.soilMoisture = probes;

    $.each(['dht', 'luminosity', 'waterLevel'], function (_, key) {
      var list = groupValues(entries, key);
      if (!list.length || list[0].skip) return;
      doc.io[key] = sensorEntry(list[0]);
    });

    return doc;
  }

  // ---------- rendering ----------

  function badge(cls, text) {
    return '<span class="badge ' + cls + '">' + esc(text) + '</span>';
  }

  function nameCell(row, note) {
    if (row.readOnly) {
      return '<td class="text-muted small">&mdash;</td>';
    }
    return '<td><input type="text" class="form-control form-control-sm"' +
           ' id="name-' + row.id + '" value="' + esc(row.name) + '"' +
           ' placeholder="' + esc(row.placeholder) + '">' + note + '</td>';
  }

  // type="text" rather than type="number": a number input hands back an empty
  // string for anything the browser dislikes, which would hide a typo instead of
  // naming the field it came from.
  function pinCell(row) {
    return '<td><input type="text" inputmode="numeric" autocomplete="off"' +
           ' class="form-control form-control-sm pin-input" id="pin-' + row.id +
           '" value="' + esc(row.pin === null ? '' : row.pin) + '"' +
           (row.readOnly ? ' readonly' : '') + '></td>';
  }

  function presenceNote(row) {
    if (row.readOnly) return '';
    if (!row.live && row.stored) {
      return '<div class="form-text hint">In config.json, but this firmware was ' +
             'not built with it — the device ignores this entry.</div>';
    }
    if (row.live && !row.stored) {
      return '<div class="form-text hint">Running on its compiled default; ' +
             'config.json does not list it.</div>';
    }
    return '';
  }

  function relayRowsHtml(models) {
    if (!models.length) {
      return '<tr><td colspan="6" class="text-muted small">No relays.</td></tr>';
    }

    var html = '';
    $.each(models, function (_, row) {
      var active = (typeof row.on === 'number')
        ? (row.on === 0 ? 'low' : 'high') : '&mdash;';
      var running = Number(row.live && row.live.on);
      var now = row.live
        ? badge(running ? 'text-bg-success' : 'text-bg-secondary',
                running ? 'On' : 'Off')
        : '<span class="text-muted">&mdash;</span>';

      html += '<tr>' +
        '<td class="text-muted small">' + row.index + '</td>' +
        nameCell(row, presenceNote(row)) +
        pinCell(row) +
        '<td class="text-muted small">' + active + '</td>' +
        '<td>' + now + '</td>' +
        '<td class="text-end">' +
        '<button type="button" class="btn btn-outline-secondary btn-sm btn-test"' +
        ' data-index="' + row.index + '" data-name="' + esc(row.title) + '"' +
        (row.live ? '' : ' disabled title="Not built into this firmware"') +
        '>Test</button></td></tr>';
    });
    return html;
  }

  function readingCell(row) {
    if (row.readOnly) {
      return '<td class="text-muted small">Edited in Config. Listed here so a ' +
             'pin conflict with it is visible.</td>';
    }
    if (row.live === null || row.live === undefined) {
      return '<td class="text-muted small">' +
             (row.stored ? 'Not built into this firmware.' : 'No reading.') +
             '</td>';
    }

    var reading = isPlainObject(row.live) ? row.live : {};
    var out = '<td>' + esc(reading.val === undefined ? '' : reading.val);
    if (reading.state) out += ' ' + badge('text-bg-secondary', reading.state);
    return out + '</td>';
  }

  function sensorRowsHtml(models) {
    if (!models.length) {
      return '<tr><td colspan="4" class="text-muted small">No sensors.</td></tr>';
    }

    var html = '';
    $.each(models, function (_, row) {
      html += '<tr>' +
        '<td class="text-muted small">' + esc(row.placeholder) + '</td>' +
        nameCell(row, presenceNote(row)) +
        pinCell(row) +
        readingCell(row) + '</tr>';
    });
    return html;
  }

  function renderCounts() {
    if (!live) {
      $('#counts').html('<span class="text-muted">Compiled device counts ' +
                        'unavailable &mdash; /data.json could not be read.</span>');
      return;
    }

    var relays = liveRelays().length;
    var probes = liveMoistureCount();
    $('#counts').html(
      'This firmware was built with <strong>' + relays + '</strong> relay' +
      (relays === 1 ? '' : 's') + ' and <strong>' + probes +
      '</strong> soil moisture probe' + (probes === 1 ? '' : 's') +
      '. Changing either count needs a rebuild and a flash, not an edit here.');
  }

  function render() {
    rows = describeDevices();

    var relayModels = [];
    var sensorModels = [];
    $.each(rows, function (_, row) {
      if (row.group === 'relay') relayModels.push(row); else sensorModels.push(row);
    });

    renderCounts();
    $('#tbody-relays').html(relayRowsHtml(relayModels));
    $('#tbody-sensors').html(sensorRowsHtml(sensorModels));
    refresh();
  }

  // ---------- form state ----------

  function collect() {
    var entries = [];
    $.each(rows, function (_, row) {
      var raw = row.readOnly
        ? String(row.pin)
        : $.trim(String($('#pin-' + row.id).val()));
      var name = row.readOnly
        ? row.name
        : $.trim(String($('#name-' + row.id).val()));

      entries.push({
        id: row.id,
        owner: row.title || row.placeholder,
        raw: raw,
        name: name,
        row: row,
      });
    });

    markBlanks(entries);
    return entries;
  }

  // Runs on every keystroke and returns the report, so the submit handler and
  // the live warning list can never disagree about what is wrong.
  function inspect(entries) {
    var report = checkPins(entries);

    $('#devices-form').find('input').removeClass('is-invalid');
    $.each(report.flagged, function (id) { $('#pin-' + id).addClass('is-invalid'); });

    if (!report.warnings.length) {
      $('#warnings').empty();
      return report;
    }

    var html = '<div class="alert alert-warning hint">' +
               '<strong>These pins will not work as intended.</strong> The device ' +
               'logs the same at every boot; it does not refuse them.<ul class="mb-0">';
    $.each(report.warnings, function (_, warning) {
      html += '<li>' + esc(warning) + '</li>';
    });
    $('#warnings').html(html + '</ul></div>');
    return report;
  }

  function refresh() {
    return inspect(collect());
  }

  // ---------- transport ----------

  function loadConfig(keepStatus) {
    $.getJSON('/config.json')
      .done(function (doc) {
        original = doc;
        render();
        $('#button-save').prop('disabled', false);
        if (!keepStatus) $('#status').empty();
      })
      .fail(function (xhr) {
        original = null;
        rows = [];
        $('#tbody-relays').empty();
        $('#tbody-sensors').empty();
        $('#warnings').empty();
        espUI.setStatus('danger', xhr.status === 403
          ? 'This account is not an administrator.'
          : 'Could not read the configuration.');
      });
  }

  function load(keepStatus) {
    if (!keepStatus) espUI.setStatus('info', 'Loading…');
    $('#button-save').prop('disabled', true);

    // /data.json is fetched first and is never fatal: it only supplies the
    // compiled counts and the live readings, so the page still edits pins and
    // names on the configuration alone.
    $.getJSON('/data.json')
      .done(function (data) { live = data; })
      .fail(function () { live = null; })
      .always(function () { loadConfig(keepStatus); });
  }

  $(function () {
    if (!espAuth.requireToken()) return;
    esc = espUI.escapeHtml;
    load();

    $('#devices-form').on('input', 'input', function () { refresh(); });

    $('#tbody-relays').on('click', '.btn-test', function () {
      var index = $(this).data('index');
      var name = String($(this).data('name'));
      var seconds = TEST_MS / 1000;

      // A relay here is a pump or a valve, so the click that moves water is
      // confirmed, and the burst is a constant rather than something typed.
      if (!confirm('Switch ' + name + ' on for ' + seconds + ' seconds?')) return;

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

    $('#devices-form').on('submit', function (event) {
      event.preventDefault();
      if (original === null) {
        espUI.setStatus('danger', 'Nothing loaded to save.');
        return;
      }

      // One pass: the entries handed to buildDocument() are the ones checkPins()
      // annotated with a parsed pin, so the document can never be built from a
      // value that was not validated.
      var entries = collect();
      var report = inspect(entries);
      if (report.invalid.length) {
        espUI.setStatus('danger', 'Not a usable GPIO number: ' +
                                  report.invalid.join(', ') + '. Nothing was saved.');
        return;
      }

      // Confirmed like the other destructive actions: the write replaces the
      // whole file, and pressing Enter in any field submits this form.
      var question = report.warnings.length
        ? report.warnings.join('\n') + '\n\nSave anyway?'
        : 'Write these pins and names to config.json?';
      if (!confirm(question)) return;

      $('#button-save').prop('disabled', true);
      $.post('/config.json', { config: JSON.stringify(buildDocument(entries)) })
        .done(function () {
          load(true);
          espUI.setStatus('success', 'Saved. Restart the device to apply.');
        })
        .fail(function (xhr) {
          espUI.setStatus('danger', 'Save failed: ' + (xhr.responseText || xhr.status));
        })
        .always(function () { $('#button-save').prop('disabled', false); });
    });

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
