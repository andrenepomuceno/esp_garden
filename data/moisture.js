// ESP Garden — the moisture classifier, its inference and its parameters.
//
// The parameters are the point. A three-band badge with no numbers behind it
// is a threshold nobody can argue with, and this device already produced one
// set of confident bands that turned out to be an artefact. So every mean,
// spread, prior and gate is on the page, and a probe that reports nothing says
// which gate refused it.
(function () {
  var CLASSES = ['dry', 'humid', 'wet'];
  var LABEL = { dry: 'Dry', humid: 'Humid', wet: 'Wet' };
  // Sequential single hue, light to dark: these three are ORDERED, so they get
  // a ramp rather than three unrelated colours.
  var COLOR = { dry: '#b4c6e7', humid: '#6f9bd8', wet: '#2d5fa8' };

  function esc(v) {
    return espUI.escapeHtml(v === undefined || v === null ? '' : v);
  }

  function fixed(v, digits) {
    return (typeof v === 'number' && isFinite(v)) ? v.toFixed(digits) : '—';
  }

  function ago(epoch) {
    if (!epoch) return 'never';
    var seconds = Math.max(0, Math.floor(Date.now() / 1000) - epoch);
    if (seconds < 3600) return Math.floor(seconds / 60) + ' min ago';
    if (seconds < 86400) return Math.floor(seconds / 3600) + ' h ago';
    return Math.floor(seconds / 86400) + ' d ago';
  }

  // A band strip drawn from the fitted parameters, with the live reading
  // marked on it. This is the one view that makes "the bands overlap" visible
  // rather than a number someone has to interpret.
  function bandStrip(probe) {
    var means = [], spread = 0, weight = 0;
    for (var i = 0; i < CLASSES.length; i++) {
      var c = probe.classes[CLASSES[i]];
      if (!c || !isFinite(c.mean)) return '';
      means.push(c.mean);
      spread = Math.max(spread, c.sd);
      weight += (c.weight || 0);
    }

    // An untrained probe has all three means at 0 and the variance floor for a
    // spread, which drew a confident three-colour ramp across a range of ±0.2
    // with the marker pinned at one end. It looked exactly like a fitted model,
    // which is the one thing this page must never do.
    if (weight <= 0) {
      return '<div class="alert alert-secondary hint mb-0">No bands to draw ' +
             'yet — this probe has no fitted parameters.</div>';
    }

    var lo = Math.min.apply(null, means) - 2 * spread;
    var hi = Math.max.apply(null, means) + 2 * spread;
    if (!(hi > lo)) return '';

    // Sample the posterior across the range by nearest-mean weighting. Not the
    // real posterior — that lives on the device — but the same ordering, which
    // is what the strip is for.
    var steps = 60, html = '';
    for (var s = 0; s < steps; s++) {
      var x = lo + ((hi - lo) * s) / (steps - 1);
      var best = null, bestZ = Infinity;
      for (var k = 0; k < CLASSES.length; k++) {
        var cls = probe.classes[CLASSES[k]];
        var z = Math.abs(x - cls.mean) / Math.max(cls.sd, 1e-6);
        if (z < bestZ) { bestZ = z; best = CLASSES[k]; }
      }
      html += '<div style="width:' + (100 / steps) + '%;background:' +
              COLOR[best] + '"></div>';
    }

    var markerPercent = null;
    if (typeof probe.reading === 'number') {
      markerPercent = ((probe.reading - lo) / (hi - lo)) * 100;
      markerPercent = Math.max(0, Math.min(100, markerPercent));
    }

    return '<div class="position-relative">' +
           '<div class="bands">' + html + '</div>' +
           (markerPercent === null ? '' :
             '<div class="marker position-absolute top-0" style="height:26px;left:' +
             markerPercent.toFixed(1) + '%"></div>') +
           '</div>' +
           '<div class="d-flex justify-content-between hint text-muted mt-1">' +
           '<span>' + fixed(lo, 1) + '</span>' +
           '<span>' + fixed(hi, 1) + '</span></div>';
  }

  function classTable(probe) {
    var rows = '';
    for (var i = 0; i < CLASSES.length; i++) {
      var key = CLASSES[i], c = probe.classes[key] || {};
      rows += '<tr>' +
        '<td><span class="swatch" style="background:' + COLOR[key] + '"></span>' +
          esc(LABEL[key]) + '</td>' +
        '<td class="num">' + fixed(c.mean, 2) + '</td>' +
        '<td class="num">' + fixed(c.sd, 2) + '</td>' +
        '<td class="num">' + fixed(c.weight, 1) + '</td>' +
        '<td class="num">' + fixed((c.prior || 0) * 100, 1) + '%</td>' +
        '</tr>';
    }
    return '<table class="table table-sm mb-0">' +
      '<thead><tr><th>Class</th><th class="num">Mean &mu;</th>' +
      '<th class="num">SD &sigma;</th><th class="num">Weight</th>' +
      '<th class="num">Prior</th></tr></thead>' +
      '<tbody>' + rows + '</tbody></table>';
  }

  function probeCard(probe, gates) {
    var badge;
    if (probe.inferred && probe.source === 'model') {
      badge = '<span class="badge text-bg-primary">' + esc(probe.inferred) +
              '</span> <span class="text-muted hint">' +
              fixed((probe.confidence || 0) * 100, 0) + '% confident</span>';
    } else if (probe.inferred) {
      badge = '<span class="badge text-bg-secondary">' + esc(probe.inferred) +
              '</span> <span class="text-muted hint">from the two-point ' +
              'calibration, not the model</span>';
    } else {
      badge = '<span class="badge text-bg-dark">no classification</span>';
    }

    var why = '';
    if (probe.blockedBy) {
      why = '<div class="alert alert-warning hint mt-2 mb-0">' +
            '<strong>Model not in use:</strong> ' + esc(probe.blockedBy) +
            '.</div>';
    }

    var evidence =
      '<div class="row g-2 hint text-muted mt-1">' +
      '<div class="col-auto">Watering events: <strong>' +
        esc(probe.wateringEvents) + '</strong> / ' + esc(gates.minEvents) +
        ' needed</div>' +
      '<div class="col-auto">Separation J: <strong>' +
        fixed(probe.separation, 1) + '</strong> / ' +
        fixed(gates.minSeparation, 1) + ' needed</div>' +
      '<div class="col-auto">Fed by relay: <strong>' +
        (probe.relay < 0 ? 'none' : esc(probe.relay)) + '</strong></div>' +
      '<div class="col-auto">Response to watering: <strong>' +
        fixed(probe.response, 2) + '</strong>' +
        (typeof probe.response === 'number' && Math.abs(probe.response) < 0.5 &&
         probe.wateringEvents >= 2
           ? ' <span class="badge text-bg-warning">no response</span>' : '') +
        '</div>' +
      '<div class="col-auto">Wiring: <strong>' +
        esc((probe.health && probe.health.verdict) || 'unknown') + '</strong>' +
        (probe.health && probe.health.verdict === 'connected'
          ? '' : ' <span class="badge text-bg-warning">check</span>') +
        (probe.health && typeof probe.health.couplingSlope === 'number'
          ? ' <span class="hint">(' +
            fixed(probe.health.couplingSlope * 100, 1) + '% coupling)</span>'
          : '') +
        '</div>' +
      '<div class="col-auto">Absorption &tau;: <strong>' +
        (probe.tauSec > 0 ? fixed(probe.tauSec / 60, 1) + ' min' : 'unmeasured') +
        '</strong></div>' +
      '</div>';

    return '<div class="card">' +
      '<div class="card-header d-flex justify-content-between align-items-center">' +
        '<span>' + esc(probe.name) + '</span>' + badge +
      '</div>' +
      '<div class="card-body">' +
        (typeof probe.reading === 'number'
          ? '<div class="hint text-muted mb-2">Current reading <strong>' +
            fixed(probe.reading, 2) + '</strong></div>'
          : '') +
        bandStrip(probe) +
        '<div class="mt-3">' + classTable(probe) + '</div>' +
        evidence + why +
      '</div></div>';
  }

  function render(data) {
    var probes = data.probes || [];
    if (!probes.length) {
      $('#probes').html('<div class="alert alert-secondary">' +
        'No soil moisture probes are configured on this device.</div>');
    } else {
      var html = '';
      for (var i = 0; i < probes.length; i++) {
        html += probeCard(probes[i], data.gates || {});
      }
      $('#probes').html(html);
    }

    $('#run').html(
      'Last trained <strong>' + esc(ago(data.trainedAt)) + '</strong>' +
      (data.trainedAt ? '' : ' — the first run happens a few minutes after boot') +
      '.<br>Scanned <strong>' + esc(data.recordsScanned) + '</strong> history ' +
      'records, used <strong>' + esc(data.samplesUsed) + '</strong> labelled ' +
      'samples, dropped <strong>' + esc(data.outliersDropped) + '</strong> as ' +
      'outliers beyond 3&sigma;.');

    var g = data.gates || {};
    $('#gates').html(
      'A probe is classified only when all three hold:<ul class="mb-2">' +
      '<li><strong>' + esc(g.minEvents) + '</strong> watering events seen ' +
        '(one cycle describes that cycle, not the soil)</li>' +
      '<li><strong>' + fixed(g.minWeightPerClass, 0) + '</strong> accumulated ' +
        'weight in every class</li>' +
      '<li>Fisher separation <strong>J &ge; ' + fixed(g.minSeparation, 1) +
        '</strong>, i.e. the dry and wet means at least two pooled standard ' +
        'deviations apart</li>' +
      '</ul>and the class means must be ordered dry &rarr; humid &rarr; wet ' +
      '(in either direction — the probe polarity is not assumed). Evidence is ' +
      'multiplied by <strong>' + fixed(g.decayPerRun, 2) + '</strong> each ' +
      'run, giving it a half-life of about ' +
      esc(Math.round(Math.log(0.5) / Math.log(g.decayPerRun || 0.93))) +
      ' days.');
  }

  function load() {
    espUI.setStatus('info', 'Loading…');
    $.getJSON('/moisture.json')
      .done(function (data) {
        render(data);
        $('#status').empty();
      })
      .fail(function (xhr) {
        espUI.setStatus('danger', xhr.status === 401
          ? 'Session expired — sign in again.'
          : 'Could not read the model (' + (xhr.status || 'no response') + ').');
      });
  }

  $(function () {
    if (!espAuth.requireToken()) return;
    load();
    $('#button-reload').on('click', load);
    $('#a-logout').on('click', function (event) {
      event.preventDefault();
      espAuth.logout();
    });
  });
})();
