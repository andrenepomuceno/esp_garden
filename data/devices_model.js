// ESP Garden — the devices page's model half: what config.json says this board
// has, what a save writes back, and every check that stands between the two.
//
// Split out of devices.js when that file crossed the 1000-line gate. The page
// state stays in devices.js and is read here through the getters handed to
// use(), so a reload that replaces caps/live/doc/model is never seen stale.
(function (global) {
  var ctx = null;   // { caps, live, doc, model } getters, owned by devices.js

  // One entry per single-instance sensor. `role` picks which capability list
  // the pin comes from: an analog sensor must land on ADC1, and everything with
  // a pull-up must not land on an input-only pin.
  var SENSORS = [
    {
      key: 'dht', title: 'DHT11', role: 'digital',
      sub: 'Temperature and air humidity on one pin',
      defaultPin: 23, defaultName: '', namePlaceholder: '(no prefix)',
      nameNote: 'A name here is a PREFIX: one pin produces two channels.',
    },
    {
      key: 'luminosity', title: 'Luminosity', role: 'analog',
      sub: 'LDR, analog', defaultPin: 39, defaultName: 'Luminosity',
    },
    {
      key: 'waterLevel', title: 'Water level', role: 'analog',
      sub: 'Analog', defaultPin: 34, defaultName: 'Water Level',
    },
    {
      key: 'flow', title: 'Flow meter', role: 'digital',
      sub: 'Pulse counter, needs an internal pull-up',
      defaultPin: 27, defaultName: 'Flow',
    },
    {
      key: 'floatSwitch', title: 'Float switch', role: 'digital',
      sub: 'Reservoir level, needs an internal pull-up',
      defaultPin: 26, defaultName: 'Float Switch',
    },
  ];

  // ---------- small helpers ----------

  function isArray(v) {
    return Object.prototype.toString.call(v) === '[object Array]';
  }

  function isPlainObject(v) {
    return v !== null && typeof v === 'object' && !isArray(v);
  }

  function has(obj, key) {
    return obj !== null && typeof obj === 'object' &&
           Object.prototype.hasOwnProperty.call(obj, key);
  }

  function num(v, fallback) {
    return (typeof v === 'number' && isFinite(v)) ? v : fallback;
  }

  function text(v) {
    return (typeof v === 'string') ? v : '';
  }

  // null for "left empty", NaN for "not a number". parseFloat('12abc') is 12,
  // so the field has to match end to end before it is parsed — a pin or a
  // calibration silently truncated to its numeric prefix is worse than an
  // error message.
  function numeric(raw) {
    var s = $.trim(String(raw === undefined || raw === null ? '' : raw));
    if (s === '') return null;
    if (!/^-?[0-9]+(\.[0-9]+)?$/.test(s)) return NaN;
    var v = parseFloat(s);
    return isFinite(v) ? v : NaN;
  }

  function plural(n, word) {
    return n + ' ' + word + (n === 1 ? '' : 's');
  }

  // ---------- capabilities ----------

  function capsUsable(payload) {
    return isPlainObject(payload) &&
           isArray(payload.kinds) &&
           isArray(payload.analogPins) &&
           isArray(payload.outputPins) &&
           isArray(payload.digitalPins) &&
           isArray(payload.strappingPins) &&
           typeof payload.relayMax === 'number' &&
           typeof payload.moistureMax === 'number';
  }

  function hasKind(kind) {
    var caps = ctx.caps();
    return caps !== null && $.inArray(kind, caps.kinds) !== -1;
  }

  function pinList(role) {
    var caps = ctx.caps();
    if (caps === null) return [];
    if (role === 'analog') return caps.analogPins;
    if (role === 'output') return caps.outputPins;
    return caps.digitalPins;
  }

  function pinAllowed(role, pin) {
    return $.inArray(pin, pinList(role)) !== -1;
  }

  function isStrapping(pin) {
    var caps = ctx.caps();
    return caps !== null && $.inArray(pin, caps.strappingPins) !== -1;
  }

  function roleText(role) {
    if (role === 'analog') {
      return 'an ADC1 channel — ADC2 cannot be read while WiFi is on';
    }
    if (role === 'output') return 'output-capable';
    return 'usable as a pulled-up input';
  }

  function sensorSpecs() {
    var out = [];
    $.each(SENSORS, function (_, spec) {
      if (hasKind(spec.key)) out.push(spec);
    });
    return out;
  }

  // ---------- default labels ----------
  //
  // Mirrors the compiled defaults in ConfigFile::ConfigFile(): an entry with no
  // "name" keeps them, so they are shown as placeholders rather than written
  // into the document.

  function relayDefaultName(index) {
    return (index === 0) ? 'Watering' : ('Relay ' + (index + 1));
  }

  function probeDefaultName(index, count) {
    return (count === 1) ? 'Soil Moisture' : ('Soil Moisture ' + (index + 1));
  }

  // The label /data.json keys Inputs by. applySingleProbeLabel() drops the
  // suffix on a one-probe board, including from an explicit "Soil Moisture 1".
  function probeLiveKey(name, index, count) {
    var label = name || probeDefaultName(index, count);
    if (count === 1 && label === 'Soil Moisture 1') return 'Soil Moisture';
    return label;
  }

  function sensorLiveKey(spec, name) {
    if (spec.key === 'dht') {
      return name ? (name + ' Temperature') : 'Temperature';
    }
    return name || spec.defaultName;
  }

  // ---------- /data.json lookups ----------

  function liveInput(key) {
    var live = ctx.live();
    var inputs = (live && isPlainObject(live.Inputs)) ? live.Inputs : {};
    return (key && has(inputs, key)) ? inputs[key] : null;
  }

  function liveRelay(index) {
    var live = ctx.live();
    var relays = (live && isArray(live.Relays)) ? live.Relays : [];
    return (typeof index === 'number' && relays[index]) ? relays[index] : null;
  }

  // ---------- config.json -> model ----------

  // io.dht, io.luminosity and io.waterLevel are a bare pin number on a config
  // written before naming existed, or {pin, name}. loadSensor() still accepts
  // both, so this page has to read both.
  function readNode(node) {
    if (typeof node === 'number') return { pin: node, name: '' };
    if (isPlainObject(node)) {
      return {
        pin: num(node.pin, null),
        name: text(node.name),
        // Optional power gating. -1 and absent both mean "permanently
        // powered", which is what every board did before this existed.
        powerPin: num(node.powerPin, null),
        powerOn: (num(node.powerOn, 1) === 0) ? 0 : 1,
        settleMs: String(num(node.settleMs, 10)),
      };
    }
    return null;
  }

  function buildModel() {
    var doc = ctx.doc();
    var io = (doc && isPlainObject(doc.io)) ? doc.io : {};
    var m = { relays: [], probes: [], sensors: {}, button: num(io.button, null) };

    // `liveIndex` / `liveKey` remember where a row came from, so the Reading
    // and Now columns keep pointing at the running peripheral even after rows
    // above them are deleted. A row added here has neither: nothing is running
    // for it until the device restarts.
    if (isArray(io.relays)) {
      $.each(io.relays, function (i, entry) {
        var node = readNode(entry) || { pin: null, name: '' };
        m.relays.push({
          name: node.name,
          pin: node.pin,
          on: (isPlainObject(entry) && num(entry.on, 0) === 1) ? 1 : 0,
          liveIndex: i,
        });
      });
    } else if (typeof io.watering === 'number') {
      // Pre-2.0 spelling: one relay as io.watering + io.wateringOn, with
      // nowhere to keep a name. Saving migrates it to the array form.
      m.relays.push({
        name: '',
        pin: io.watering,
        on: (num(io.wateringOn, 0) === 1) ? 1 : 0,
        liveIndex: 0,
      });
    }

    var calibration = (doc && isArray(doc.moisture)) ? doc.moisture : [];
    var stored = [];
    if (isArray(io.soilMoisture)) {
      $.each(io.soilMoisture, function (_, entry) {
        stored.push(readNode(entry) || { pin: null, name: '' });
      });
    } else if (readNode(io.soilMoisture)) {
      stored.push(readNode(io.soilMoisture));
    }
    $.each(stored, function (i, node) {
      var cal = isPlainObject(calibration[i]) ? calibration[i] : {};
      m.probes.push({
        name: node.name,
        pin: node.pin,
        // Kept as typed text, not as numbers: a field the user is halfway
        // through editing must not be rounded back at them on every keystroke.
        dry: String(num(cal.dry, 0)),
        wet: String(num(cal.wet, 0)),
        // Which pump feeds this probe. The Bayesian classifier labels a
        // reading by its distance from a watering EVENT, so dropping this on
        // save silently reverted the mapping to probe i -> relay i and
        // retrained the model against the wrong pump.
        relay: (typeof cal.relay === 'number') ? cal.relay : i,
        // Defaults to true because that is what the capacitive v2 modules
        // every existing board carries actually do.
        invert: (cal.invert === false) ? false : true,
        kind: text(cal.kind),
        powerPin: (typeof node.powerPin === 'number' && node.powerPin >= 0)
          ? node.powerPin : null,
        powerOn: (node.powerOn === 0) ? 0 : 1,
        settleMs: (typeof node.settleMs === 'string') ? node.settleMs : '10',
        liveKey: probeLiveKey(node.name, i, stored.length),
      });
    });

    $.each(sensorSpecs(), function (_, spec) {
      var fitted = has(io, spec.key);
      var node = fitted ? (readNode(io[spec.key]) || { pin: null, name: '' })
                        : null;
      // An unfitted sensor still carries a pin — its compiled default, shown
      // greyed out. Dropping it would mean ticking the box twice in a row
      // produces a different pin each time, and the row the user just unticked
      // by accident comes back on some other GPIO.
      var entry = {
        fitted: fitted,
        name: fitted ? node.name : '',
        pin: fitted ? num(node.pin, spec.defaultPin) : spec.defaultPin,
        liveKey: fitted ? sensorLiveKey(spec, node.name) : null,
      };

      if (spec.key === 'flow') {
        var flow = isPlainObject(io.flow) ? io.flow : {};
        entry.pulsesPerLitre = String(num(flow.pulsesPerLitre, 450));
      }
      if (spec.key === 'floatSwitch') {
        var sw = isPlainObject(io.floatSwitch) ? io.floatSwitch : {};
        entry.activeLevel = (num(sw.activeLevel, 0) === 1) ? 1 : 0;
        entry.interlock = (sw.interlock === true);
        entry.fillRelay = num(sw.fillRelay, -1);
      }

      m.sensors[spec.key] = entry;
    });

    return m;
  }

  // Which GPIOs the model already spends, ignoring one sensor so that a sensor
  // being fitted does not count as its own conflict.
  function takenPins(exceptSensorKey) {
    var model = ctx.model();
    var taken = {};
    if (!model) return taken;
    if (typeof model.button === 'number') taken[model.button] = true;
    $.each(model.relays, function (_, r) { if (r.pin !== null) taken[r.pin] = true; });
    $.each(model.probes, function (_, p) { if (p.pin !== null) taken[p.pin] = true; });
    $.each(model.sensors, function (key, s) {
      if (key === exceptSensorKey) return;
      if (s.fitted && s.pin !== null) taken[s.pin] = true;
    });
    return taken;
  }

  // The pin a sensor gets when it is ticked back on: its firmware default,
  // but only while that pin is free and right for the job. Otherwise nothing —
  // a new row asks for the GPIO instead of proposing one.
  //
  // Nothing here invents a preference order over the capability list. The
  // first free entry of caps.outputPins is GPIO 1, the serial TX line: legal
  // for the firmware, and a terrible thing to hand a relay without being
  // asked.
  function suggestedPin(spec, key) {
    var taken = takenPins(key);
    return (pinAllowed(spec.role, spec.defaultPin) && !taken[spec.defaultPin])
      ? spec.defaultPin
      : null;
  }

  // ---------- model -> config.json ----------

  // Starts from the document the device served, so every key this page does not
  // render — including the ******** secrets, which POST /config.json restores
  // from disk — is posted back exactly as it arrived.
  function buildDocument() {
    var doc = ctx.doc();
    var model = ctx.model();
    var out = JSON.parse(JSON.stringify(doc));
    if (!isPlainObject(out.io)) out.io = {};
    var io = out.io;

    if (hasKind('relays')) {
      var relays = [];
      $.each(model.relays, function (_, r) {
        var entry = { pin: r.pin, on: r.on };
        if (r.name) entry.name = r.name;
        relays.push(entry);
      });
      io.relays = relays;
      // loadRelays() only falls back to the pre-2.0 scalar pair when there is
      // no array. Leaving it behind would resurrect a relay the user deleted.
      delete io.watering;
      delete io.wateringOn;
    }

    if (hasKind('soilMoisture')) {
      var probes = [];
      var calibration = [];
      $.each(model.probes, function (_, p) {
        var entry = { pin: p.pin };
        if (p.name) entry.name = p.name;
        if (typeof p.powerPin === 'number' && p.powerPin >= 0) {
          entry.powerPin = p.powerPin;
          entry.powerOn = (p.powerOn === 0) ? 0 : 1;
          var settle = numeric(p.settleMs);
          entry.settleMs = (settle === null) ? 10 : settle;
        }
        probes.push(entry);
        // An empty field is 0, which is the value the firmware ships as
        // "uncalibrated" — dry == wet means "do not classify".
        var cal = {
          dry: numeric(p.dry) === null ? 0 : numeric(p.dry),
          wet: numeric(p.wet) === null ? 0 : numeric(p.wet),
          relay: (typeof p.relay === 'number') ? p.relay : -1,
          invert: (p.invert === false) ? false : true,
        };
        // Only when set: an empty label should not become part of the model's
        // identity and start discarding models on every save.
        if (p.kind) cal.kind = p.kind;
        calibration.push(cal);
      });
      io.soilMoisture = probes;
      // Parallel to io.soilMoisture by construction: written from the same
      // loop, so a deleted probe cannot leave its calibration behind for the
      // next probe to inherit.
      if (calibration.length || has(out, 'moisture')) {
        out.moisture = calibration;
      }
    }

    $.each(sensorSpecs(), function (_, spec) {
      var s = model.sensors[spec.key];
      if (!s.fitted) {
        // A sensor is fitted if and only if its key exists. Deleting it is
        // exactly removing the key — there is no enabled flag to drift.
        delete io[spec.key];
        return;
      }
      var entry = { pin: s.pin };
      if (s.name) entry.name = s.name;
      if (spec.key === 'flow') entry.pulsesPerLitre = numeric(s.pulsesPerLitre);
      if (spec.key === 'floatSwitch') {
        entry.activeLevel = s.activeLevel;
        entry.interlock = !!s.interlock;
        entry.fillRelay = s.fillRelay;
      }
      io[spec.key] = entry;
    });

    return out;
  }

  // ---------- validation ----------
  //
  // Checks the same predicates the firmware does — documentPinsAreUsable()
  // refuses the save, validatePins() logs at boot — before the reboot that
  // would apply them. A probe moved onto ADC2 reads 0 forever rather than
  // failing, and the log that says so is only reachable once the device is back
  // up on the pin that broke it.

  // A probe with no pump gets no model at all — nothing labels its readings.
  // Worth saying out loud, because the page can produce that state with one
  // click on a relay's delete button.
  function warnOrphanedProbes(model, warnings) {
    $.each(model.probes, function (i, probe) {
      if (typeof probe.relay === 'number' && probe.relay < 0) {
        warnings.push('Probe ' + (i + 1) + ' has no feeding relay, so the ' +
                      'moisture model cannot be trained for it. It falls back ' +
                      'to the two-point calibration.');
      }
    });
  }

  function validate() {
    var caps = ctx.caps();
    var doc = ctx.doc();
    var model = ctx.model();
    var problems = [];
    var warnings = [];
    // Keyed by CSS selector rather than by a synthetic id: refresh() then just
    // paints what is in here, with no id grammar for the two halves to
    // disagree about.
    var flagged = {};
    var claims = {};

    function claim(pin, owner, selector) {
      if (typeof pin !== 'number') return;
      if (!claims[pin]) claims[pin] = [];
      claims[pin].push({ owner: owner, selector: selector });
    }

    function checkPin(pin, role, owner, selector) {
      if (pin === null) {
        problems.push(owner + ': no GPIO selected.');
        flagged[selector] = true;
        return;
      }
      if (!pinAllowed(role, pin)) {
        problems.push(owner + ': GPIO ' + pin + ' is not ' + roleText(role) +
                      '. This firmware refuses it.');
        flagged[selector] = true;
      } else if (isStrapping(pin)) {
        // Warned, never refused: the legacy watering relay legitimately sits
        // on GPIO 15, and every board in the field boots with it.
        warnings.push(owner + ': GPIO ' + pin + ' is a strapping pin, sampled ' +
                      'at reset. A pull on one can stop the board booting.');
      }
      claim(pin, owner, selector);
    }

    // io.button is edited in Config, not here, but it holds a GPIO — without
    // this claim a relay parked on it would show no conflict on this page
    // while the device logs one at every boot.
    claim(model.button, 'Button (edited in Config)', null);

    if (model.relays.length > caps.relayMax) {
      problems.push('This firmware drives at most ' + plural(caps.relayMax, 'relay') +
                    '; the rest are ignored.');
    }
    if (model.probes.length > caps.moistureMax) {
      problems.push('This firmware reads at most ' +
                    plural(caps.moistureMax, 'moisture probe') +
                    '; the rest are ignored.');
    }

    $.each(model.relays, function (i, r) {
      checkPin(r.pin, 'output', relayOwner(i),
               '.rl-pin[data-row="' + i + '"]');
    });

    $.each(model.probes, function (i, p) {
      var owner = probeOwner(i);
      checkPin(p.pin, 'analog', owner, '.ms-pin[data-row="' + i + '"]');
      $.each([['dry', p.dry], ['wet', p.wet]], function (_, pair) {
        if (isNaN(numeric(pair[1]))) {
          problems.push(owner + ': ' + pair[0] + ' calibration is not a number.');
          flagged['.ms-' + pair[0] + '[data-row="' + i + '"]'] = true;
        }
      });
    });

    $.each(sensorSpecs(), function (_, spec) {
      var s = model.sensors[spec.key];
      if (!s.fitted) return;
      checkPin(s.pin, spec.role, spec.title,
               '.sn-pin[data-key="' + spec.key + '"]');

      if (spec.key === 'flow') {
        var k = numeric(s.pulsesPerLitre);
        if (k === null || isNaN(k) || k <= 0) {
          problems.push('Flow meter: pulses per litre must be a positive ' +
                        'number. The device ignores anything else and keeps ' +
                        'its default.');
          flagged['.sn-ppl'] = true;
        }
      }
      if (spec.key === 'floatSwitch' && s.fillRelay >= model.relays.length) {
        warnings.push('Float switch: the refill relay is index ' + s.fillRelay +
                      ', which no longer exists. The device keeps no relay ' +
                      'exempt from the interlock.');
      }
    });

    $.each(claims, function (pin, owners) {
      if (owners.length < 2) return;
      var names = [];
      $.each(owners, function (_, entry) {
        names.push(entry.owner);
        if (entry.selector) flagged[entry.selector] = true;
      });
      problems.push('GPIO ' + pin + ' is assigned to ' + names.join(' and ') +
                    '. Two peripherals on one pin is not a fault the device ' +
                    'reports at runtime — one of them just reads or drives ' +
                    'garbage.');
    });

    // Relay rows are addressed by index by TalkBack, by /control and by every
    // schedule, so a shortened or reordered list changes what those mean.
    // Appending does not: index 0..n-1 still point where they pointed.
    var shifted = (model.relays.length < relayCountAtLoad());
    $.each(model.relays, function (i, r) {
      if (r.liveIndex !== null && r.liveIndex !== i) shifted = true;
    });
    if (shifted) {
      warnings.push('Relay indices changed. Index 0 is what TalkBack, the ' +
                    'legacy /control watering parameter and ThingSpeak field 2 ' +
                    'address, and schedules target relays by index.');
    }

    warnOrphanedProbes(model, warnings);

    var schedules = (doc && isArray(doc.schedules)) ? doc.schedules : [];
    $.each(schedules, function (i, s) {
      var target = num(s.relay, 0);
      if (target >= model.relays.length) {
        warnings.push('Schedule "' + (text(s.name) || ('#' + (i + 1))) +
                      '" targets relay ' + target +
                      ', which no longer exists; the device will drop it.');
      }
    });

    return { problems: problems, warnings: warnings, flagged: flagged,
             claims: claims };
  }

  function relayCountAtLoad() {
    var doc = ctx.doc();
    var io = (doc && isPlainObject(doc.io)) ? doc.io : {};
    if (isArray(io.relays)) return io.relays.length;
    return (typeof io.watering === 'number') ? 1 : 0;
  }

  function relayOwner(index) {
    var model = ctx.model();
    var name = model.relays[index].name || relayDefaultName(index);
    return 'Relay ' + index + ' (' + name + ')';
  }

  function probeOwner(index) {
    var model = ctx.model();
    var name = model.probes[index].name ||
               probeDefaultName(index, model.probes.length);
    return 'Probe ' + (index + 1) + ' (' + name + ')';
  }

  function use(context) {
    ctx = context;
  }

  global.espDevicesModel = {
    use: use,
    SENSORS: SENSORS,
    isPlainObject: isPlainObject,
    text: text,
    numeric: numeric,
    plural: plural,
    capsUsable: capsUsable,
    pinList: pinList,
    pinAllowed: pinAllowed,
    isStrapping: isStrapping,
    sensorSpecs: sensorSpecs,
    relayDefaultName: relayDefaultName,
    probeDefaultName: probeDefaultName,
    liveInput: liveInput,
    liveRelay: liveRelay,
    buildModel: buildModel,
    takenPins: takenPins,
    suggestedPin: suggestedPin,
    buildDocument: buildDocument,
    validate: validate,
  };
})(window);
