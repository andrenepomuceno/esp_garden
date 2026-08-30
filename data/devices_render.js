// ESP Garden — the devices page's HTML builders: the three tables, the GPIO
// pickers and the pin map.
//
// Split out of devices.js when that file crossed the 1000-line gate. Nothing
// here reaches the network or writes the model; it paints what the model half
// already decided, and every value it interpolates goes through
// espUI.escapeHtml.
(function (global) {
  var ctx = null;   // { caps, live, model, ready } getters, owned by devices.js
  var esc = null;   // bound in use(), once auth.js has run

  // Bound to the same names devices.js used, so every call site below is the
  // line it always was.
  var M = global.espDevicesModel;
  var isPlainObject = M.isPlainObject;
  var numeric = M.numeric;
  var pinList = M.pinList;
  var isStrapping = M.isStrapping;
  var sensorSpecs = M.sensorSpecs;
  var relayDefaultName = M.relayDefaultName;
  var probeDefaultName = M.probeDefaultName;
  var liveInput = M.liveInput;
  var liveRelay = M.liveRelay;

  // ---------- rendering ----------

  function badge(cls, label) {
    return '<span class="badge ' + cls + '">' + esc(label) + '</span>';
  }

  function dash() {
    return '<span class="text-muted">&mdash;</span>';
  }

  // A pin this firmware does not offer for the role keeps an option of its own
  // instead of falling through to the first entry: silently moving a live
  // peripheral onto another GPIO is worse than showing the bad value and
  // refusing to save.
  function pinSelect(cls, attributes, role, selected, noneLabel) {
    var options = '';
    var known = false;

    // A power pin is optional in a way a sensor pin is not, so it gets an
    // explicit "none" entry rather than the "select a GPIO" placeholder that
    // reads as something still to be filled in.
    if (noneLabel) {
      options += '<option value=""' + (selected === null ? ' selected' : '') +
                 '>' + esc(noneLabel) + '</option>';
      if (selected === null) known = true;
    }

    $.each(pinList(role), function (_, pin) {
      if (pin === selected) known = true;
      options += '<option value="' + pin + '"' +
                 (pin === selected ? ' selected' : '') + '>GPIO ' + pin +
                 (isStrapping(pin) ? ' — strapping' : '') + '</option>';
    });

    if (!known) {
      var label = (selected === null)
        ? '— select a GPIO —'
        : ('GPIO ' + selected + ' — not usable here');
      options = '<option value="' + esc(selected === null ? '' : selected) +
                '" selected>' + esc(label) + '</option>' + options;
    }

    return '<select class="form-select form-select-sm ' + cls + '" ' +
           attributes + '>' + options + '</select>';
  }

  function readingHtml(entry, missingNote) {
    if (entry === null || entry === undefined) {
      return '<span class="text-muted small">' + esc(missingNote) + '</span>';
    }
    var reading = isPlainObject(entry) ? entry : {};
    var html = esc(reading.val === undefined ? '' : reading.val);
    if (reading.state) html += ' ' + badge('text-bg-secondary', reading.state);
    return html;
  }

  function missingNote(row) {
    return (row.liveKey === null || row.liveIndex === null)
      ? 'added — restart to run it'
      : 'not running';
  }

  function renderRelays() {
    var caps = ctx.caps();
    var model = ctx.model();
    var ready = ctx.ready();
    if (!model.relays.length) {
      $('#tbody-relays').html('<tr><td colspan="6" class="text-muted small">' +
        'No relays. This device drives nothing until one is added.</td></tr>');
    } else {
      var html = '';
      $.each(model.relays, function (i, r) {
        var running = liveRelay(r.liveIndex);
        var now = running
          ? badge(Number(running.on) ? 'text-bg-success' : 'text-bg-secondary',
                  Number(running.on) ? 'On' : 'Off')
          : '<span class="text-muted small">' + esc(missingNote(r)) + '</span>';

        var note = (i === 0)
          ? '<div class="form-text hint">Watering relay: TalkBack, the legacy ' +
            '<code>watering</code> control and ThingSpeak field 2 all address ' +
            'index 0.</div>'
          : '';

        html += '<tr>' +
          '<td class="text-muted small">' + i + '</td>' +
          '<td><input type="text" class="form-control form-control-sm rl-name"' +
            ' data-row="' + i + '" value="' + esc(r.name) + '"' +
            ' placeholder="' + esc(relayDefaultName(i)) + '">' + note + '</td>' +
          '<td>' + pinSelect('rl-pin', 'data-row="' + i + '"', 'output', r.pin) + '</td>' +
          '<td><select class="form-select form-select-sm rl-on" data-row="' + i + '">' +
            '<option value="0"' + (r.on === 0 ? ' selected' : '') + '>0 &mdash; active low</option>' +
            '<option value="1"' + (r.on === 1 ? ' selected' : '') + '>1 &mdash; active high</option>' +
            '</select></td>' +
          '<td>' + now + '</td>' +
          '<td class="text-end col-fixed">' +
            '<button type="button" class="btn btn-outline-secondary btn-sm btn-test"' +
              ' data-live="' + (running ? r.liveIndex : '') + '"' +
              ' data-name="' + esc(r.name || relayDefaultName(i)) + '"' +
              (running ? '' : ' disabled title="Not running on the device yet"') +
              '>Test</button> ' +
            '<button type="button" class="btn btn-outline-danger btn-sm btn-del-relay"' +
              ' data-row="' + i + '" title="Delete this relay">&minus;</button>' +
          '</td></tr>';
      });
      $('#tbody-relays').html(html);
    }

    var count = model.relays.length;
    $('#relay-count').text(count + ' of ' + caps.relayMax + ' declared');
    var full = count >= caps.relayMax;
    $('#button-add-relay').prop('disabled', !ready || full);
    $('#relay-add-note').text(full
      ? ('RELAY_MAX is ' + caps.relayMax + ' in this firmware; more needs a rebuild.')
      : '');
  }

  // One relay picker, used by the float switch's refill relay and by each
  // probe's feeding pump. A stored index the board no longer has keeps its own
  // option rather than falling through to relay 0 — silently retargeting
  // either of those points something at the watering pump.
  function relayPickerOptions(selected) {
  var model = ctx.model();
  var options = '<option value="-1"' +
    (selected === -1 ? ' selected' : '') + '>none</option>';
  var known = (selected === -1);
  $.each(model.relays, function (i, r) {
    if (i === selected) known = true;
    options += '<option value="' + i + '"' +
      (i === selected ? ' selected' : '') + '>' + i + ' &mdash; ' +
      esc(r.name || relayDefaultName(i)) + '</option>';
  });
  if (!known) {
    options += '<option value="' + esc(selected) + '" selected>' +
      esc(selected + ' — removed') + '</option>';
  }
  return options;
  }

  function renderProbes() {
    var caps = ctx.caps();
    var model = ctx.model();
    var ready = ctx.ready();
    if (!model.probes.length) {
      $('#tbody-probes').html('<tr><td colspan="10" class="text-muted small">' +
        'No soil moisture probes.</td></tr>');
    } else {
      var html = '';
      var count = model.probes.length;
      $.each(model.probes, function (i, p) {
        var reading = liveInput(p.liveKey);
        var uncalibrated = (numeric(p.dry) === numeric(p.wet));

        html += '<tr>' +
          '<td class="text-muted small">' + (i + 1) + '</td>' +
          '<td><input type="text" class="form-control form-control-sm ms-name"' +
            ' data-row="' + i + '" value="' + esc(p.name) + '"' +
            ' placeholder="' + esc(probeDefaultName(i, count)) + '"></td>' +
          '<td>' + pinSelect('ms-pin', 'data-row="' + i + '"', 'analog', p.pin) + '</td>' +
          '<td><input type="text" inputmode="decimal" autocomplete="off"' +
            ' class="form-control form-control-sm ms-dry" data-row="' + i + '"' +
            ' value="' + esc(p.dry) + '"></td>' +
          '<td><input type="text" inputmode="decimal" autocomplete="off"' +
            ' class="form-control form-control-sm ms-wet" data-row="' + i + '"' +
            ' value="' + esc(p.wet) + '"></td>' +
          '<td><input type="text" class="form-control form-control-sm ms-kind"' +
            ' data-row="' + i + '" value="' + esc(p.kind || '') + '"' +
            ' placeholder="e.g. wd-38"><div class="form-check form-check-sm">' +
            '<input class="form-check-input ms-invert" type="checkbox"' +
            ' data-row="' + i + '" id="ms-invert-' + i + '"' +
            (p.invert === false ? '' : ' checked') + '>' +
            '<label class="form-check-label hint" for="ms-invert-' + i + '">' +
            'reads lower when wet</label></div></td>' +
          '<td>' + pinSelect('ms-power', 'data-row="' + i + '"', 'output',
                             (typeof p.powerPin === 'number') ? p.powerPin : null,
                             'always on') +
            // The active level, exactly as a relay row offers one: a probe
            // switched by a P-channel MOSFET is energised LOW, and without
            // this the only route to that was the raw JSON editor.
            '<select class="form-select form-select-sm ms-power-on mt-1"' +
            ' data-row="' + i + '" title="the level that energises the probe">' +
            '<option value="1"' + (p.powerOn === 0 ? '' : ' selected') +
              '>on = HIGH</option>' +
            '<option value="0"' + (p.powerOn === 0 ? ' selected' : '') +
              '>on = LOW</option></select>' +
            '<input type="text" inputmode="numeric" autocomplete="off"' +
            ' class="form-control form-control-sm ms-settle mt-1"' +
            ' data-row="' + i + '" value="' + esc(p.settleMs || '10') + '"' +
            ' title="milliseconds to wait after energising, before reading">' +
            '</td>' +
          '<td><select class="form-select form-select-sm ms-relay"' +
            ' data-row="' + i + '">' + relayPickerOptions(
              (typeof p.relay === 'number') ? p.relay : -1) + '</select></td>' +
          '<td>' + readingHtml(reading, missingNote(p)) +
            (uncalibrated
              ? '<div class="form-text hint">Uncalibrated: dry equals wet, so no badge is shown.</div>'
              : '') + '</td>' +
          '<td class="text-end col-fixed">' +
            '<button type="button" class="btn btn-outline-danger btn-sm btn-del-probe"' +
              ' data-row="' + i + '" title="Delete this probe">&minus;</button>' +
          '</td></tr>';
      });
      $('#tbody-probes').html(html);
    }

    var probes = model.probes.length;
    $('#probe-count').text(probes + ' of ' + caps.moistureMax + ' declared');
    var full = probes >= caps.moistureMax;
    $('#button-add-probe').prop('disabled', !ready || full);
    $('#probe-add-note').text(full
      ? ('MOISTURE_MAX is ' + caps.moistureMax + ' in this firmware — the ' +
         'history record has that many slots — so more needs a rebuild.')
      : '');
  }

  function sensorOptionsHtml(spec, s) {
    if (spec.key === 'flow') {
      return '<label class="form-label small mb-0">Pulses per litre</label>' +
             '<input type="text" inputmode="decimal" autocomplete="off"' +
             ' class="form-control form-control-sm sn-ppl" data-key="flow"' +
             ' value="' + esc(s.pulsesPerLitre) + '"' +
             (s.fitted ? '' : ' disabled') + '>' +
             '<div class="form-text hint">450 is the YF-S201 nominal.</div>';
    }


    if (spec.key === 'floatSwitch') {
      var relayOptions = relayPickerOptions(s.fillRelay);

      return '<div class="form-check form-switch mb-1">' +
        '<input class="form-check-input sn-interlock" type="checkbox"' +
        ' data-key="floatSwitch" id="sn-interlock"' + (s.interlock ? ' checked' : '') +
        (s.fitted ? '' : ' disabled') + '>' +
        '<label class="form-check-label small" for="sn-interlock">' +
        'Block pumps while the reservoir reads empty</label></div>' +
        '<label class="form-label small mb-0">Active level</label>' +
        '<select class="form-select form-select-sm sn-level" data-key="floatSwitch"' +
        (s.fitted ? '' : ' disabled') + '>' +
        '<option value="0"' + (s.activeLevel === 0 ? ' selected' : '') + '>0 &mdash; closes to ground</option>' +
        '<option value="1"' + (s.activeLevel === 1 ? ' selected' : '') + '>1 &mdash; closes to 3V3</option>' +
        '</select>' +
        '<label class="form-label small mb-0 mt-1">Refill relay, exempt from the interlock</label>' +
        '<select class="form-select form-select-sm sn-fill" data-key="floatSwitch"' +
        (s.fitted ? '' : ' disabled') + '>' + relayOptions + '</select>';
    }

    return dash();
  }

  function renderSensors() {
    var model = ctx.model();
    var specs = sensorSpecs();
    if (!specs.length) {
      $('#tbody-sensors').html('<tr><td colspan="6" class="text-muted small">' +
        'This firmware has no single-instance sensor drivers.</td></tr>');
      return;
    }

    var html = '';
    $.each(specs, function (_, spec) {
      var s = model.sensors[spec.key];
      var reading = s.fitted ? liveInput(s.liveKey) : null;
      var readingCell = s.fitted
        ? readingHtml(reading, missingNote(s))
        : '<span class="text-muted small">not fitted</span>';

      var namePlaceholder = spec.namePlaceholder || spec.defaultName;
      var nameNote = spec.nameNote
        ? '<div class="form-text hint">' + esc(spec.nameNote) + '</div>' : '';

      html += '<tr>' +
        '<td><input class="form-check-input sn-fit" type="checkbox"' +
          ' data-key="' + esc(spec.key) + '"' + (s.fitted ? ' checked' : '') + '></td>' +
        '<td>' + esc(spec.title) +
          '<div class="form-text hint">' + esc(spec.sub) + '</div>' +
          '<div class="form-text hint"><code>io.' + esc(spec.key) + '</code></div></td>' +
        '<td><input type="text" class="form-control form-control-sm sn-name"' +
          ' data-key="' + esc(spec.key) + '" value="' + esc(s.name) + '"' +
          ' placeholder="' + esc(namePlaceholder) + '"' +
          (s.fitted ? '' : ' disabled') + '>' + nameNote + '</td>' +
        '<td>' + pinSelect('sn-pin', 'data-key="' + esc(spec.key) + '"' +
            (s.fitted ? '' : ' disabled'), spec.role, s.pin) + '</td>' +
        '<td>' + sensorOptionsHtml(spec, s) + '</td>' +
        '<td>' + readingCell + '</td></tr>';
    });
    $('#tbody-sensors').html(html);
  }

  function renderPinMap(report) {
    var pins = [];
    $.each(report.claims, function (pin) { pins.push(parseInt(pin, 10)); });
    pins.sort(function (a, b) { return a - b; });

    if (!pins.length) {
      $('#pin-map').html('<span class="text-muted small">No GPIO is claimed.</span>');
      return;
    }

    var html = '';
    $.each(pins, function (_, pin) {
      var owners = report.claims[pin];
      var names = [];
      $.each(owners, function (_, entry) { names.push(entry.owner); });
      var cls = (owners.length > 1) ? 'text-bg-danger'
        : (isStrapping(pin) ? 'text-bg-warning' : 'text-bg-secondary');
      html += '<span class="pin-chip">' + badge(cls, 'GPIO ' + pin) +
              ' <span class="small">' + esc(names.join(' + ')) + '</span></span>';
    });
    $('#pin-map').html(html + '<div class="hint text-muted mt-2">' +
      'Red is two peripherals on one pin, which is refused. Amber is a ' +
      'strapping pin, which is allowed. Pins not listed are free.</div>');
  }

  function use(context) {
    ctx = context;
    esc = espUI.escapeHtml;
  }

  global.espDevicesRender = {
    use: use,
    renderRelays: renderRelays,
    renderProbes: renderProbes,
    renderSensors: renderSensors,
    renderPinMap: renderPinMap,
  };
})(window);
