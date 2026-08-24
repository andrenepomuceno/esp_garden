// ESP Garden — history charts.
//
// Hand-drawn inline SVG rather than a charting library: the pages load nothing
// from a CDN, and the smallest usable library is larger than everything else in
// SPIFFS put together.
//
// Small multiples, one plot per measure, all sharing a time axis. Never a
// second y-scale on one plot — temperature in degrees and humidity in percent
// are separate charts precisely because they cannot share an axis honestly.
(function () {
  var SERIES = ['var(--series-1)', 'var(--series-2)', 'var(--series-3)'];
  var records = [];
  var offset = null;      // null = newest page
  var stored = 0;
  // Relay labels come from /data.json's Relays array — the payload CLAUDE.md
  // names as the addressing contract. Hardcoding them mislabels a renamed
  // relay and invents rows on a one-relay board.
  var relayNames = [];
  // The record carries IO_HISTORY_MAX_MOISTURE slots regardless of build, so
  // the column count follows the data rather than a constant that would drop a
  // fourth probe silently.
  var probeCount = 0;

  function esc(s) { return espUI.escapeHtml(s); }

  function fmtTime(unix) {
    var d = new Date(unix * 1000);
    var p = function (n) { return (n < 10 ? '0' : '') + n; };
    return p(d.getHours()) + ':' + p(d.getMinutes());
  }

  function fmtDateTime(unix) {
    var d = new Date(unix * 1000);
    return d.toLocaleString();
  }

  // ---------- scales ----------
  function extent(values) {
    var lo = Infinity, hi = -Infinity;
    for (var i = 0; i < values.length; i++) {
      var v = values[i];
      if (v === null || v === undefined || isNaN(v)) continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (lo === Infinity) return null;
    if (lo === hi) { lo -= 1; hi += 1; }       // a flat series still needs a band
    var pad = (hi - lo) * 0.12;
    return [lo - pad, hi + pad];
  }

  function niceTicks(lo, hi, count) {
    var span = hi - lo;
    var raw = span / count;
    var mag = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10));
    var norm = raw / mag;
    var step = (norm >= 5 ? 10 : norm >= 2 ? 5 : norm >= 1 ? 2 : 1) * mag;
    var ticks = [];
    for (var t = Math.ceil(lo / step) * step; t <= hi; t += step) {
      ticks.push(Math.round(t * 1000) / 1000);
    }
    return ticks;
  }

  // ---------- chart ----------
  // `series` is [{name, values[], color}]. Values may hold nulls: a channel the
  // board does not have, or a gap. Gaps break the path instead of being drawn
  // through, which would invent readings that were never taken.
  function drawChart(container, title, unit, series) {
    var W = 640, H = 150, L = 44, R = 54, T = 12, B = 20;
    var all = [];
    for (var s = 0; s < series.length; s++) all = all.concat(series[s].values);
    var range = extent(all);
    if (!range) return;

    var n = records.length;
    var x = function (i) { return L + (n <= 1 ? 0 : (i * (W - L - R)) / (n - 1)); };
    var y = function (v) {
      return T + (H - T - B) * (1 - (v - range[0]) / (range[1] - range[0]));
    };

    var svg = '<svg class="chart" viewBox="0 0 ' + W + ' ' + H +
              '" preserveAspectRatio="none" role="img" aria-label="' +
              esc(title) + '">';

    var ticks = niceTicks(range[0], range[1], 3);
    for (var t = 0; t < ticks.length; t++) {
      var ty = y(ticks[t]);
      svg += '<line class="grid" x1="' + L + '" y1="' + ty + '" x2="' + (W - R) +
             '" y2="' + ty + '"/>';
      svg += '<text class="axis-text" x="' + (L - 6) + '" y="' + (ty + 3) +
             '" text-anchor="end">' + ticks[t] + '</text>';
    }

    if (n > 1) {
      svg += '<text class="axis-text" x="' + L + '" y="' + (H - 5) + '">' +
             fmtTime(records[0].t) + '</text>';
      svg += '<text class="axis-text" x="' + (W - R) + '" y="' + (H - 5) +
             '" text-anchor="end">' + fmtTime(records[n - 1].t) + '</text>';
    }

    for (var si = 0; si < series.length; si++) {
      var vals = series[si].values, d = '', pen = false, lastI = -1;
      for (var i = 0; i < vals.length; i++) {
        var v = vals[i];
        if (v === null || v === undefined || isNaN(v)) { pen = false; continue; }
        d += (pen ? 'L' : 'M') + x(i).toFixed(1) + ' ' + y(v).toFixed(1) + ' ';
        pen = true; lastI = i;
      }
      if (d) {
        svg += '<path class="line" d="' + d + '" stroke="' + series[si].color + '"/>';
        // Direct label at the last point. This is the "relief" the palette
        // check asks for on low-contrast slots, and it means identity never
        // rests on colour alone.
        if (series.length > 1 && lastI >= 0) {
          svg += '<text class="end-label" x="' + (W - R + 5) + '" y="' +
                 (y(vals[lastI]) + 3) + '" fill="' + series[si].color + '">' +
                 esc(series[si].name) + '</text>';
        }
      }
    }

    svg += '<line class="crosshair" x1="0" y1="' + T + '" x2="0" y2="' +
           (H - B) + '" style="display:none"/>';
    for (var g = 0; g < series.length; g++) {
      svg += '<circle class="hover-dot" r="4" fill="' + series[g].color +
             '" style="display:none"/>';
    }
    svg += '</svg>';

    var legend = '';
    if (series.length > 1) {
      legend = '<div class="legend mt-1">';
      for (var l = 0; l < series.length; l++) {
        legend += '<span><span class="swatch" style="background:' +
                  series[l].color + '"></span>' + esc(series[l].name) + '</span>';
      }
      legend += '</div>';
    }

    var card = $('<div class="card"><div class="card-header"><span>' +
                 esc(title) + '</span><span class="hint">' + esc(unit) +
                 '</span></div><div class="card-body">' + svg + legend +
                 '</div></div>');
    container.append(card);
    attachHover(card.find('svg')[0], series, x, y, W, L, R);
  }

  // Crosshair + tooltip. A chart in a browser is interactive by default; the
  // hit target is the whole plot, not the 2px line.
  function attachHover(svg, series, x, y, W, L, R) {
    var crosshair = svg.querySelector('.crosshair');
    var dots = svg.querySelectorAll('.hover-dot');
    var tip = document.getElementById('tooltip');

    function hide() {
      crosshair.style.display = 'none';
      for (var i = 0; i < dots.length; i++) dots[i].style.display = 'none';
      tip.style.display = 'none';
    }

    function move(event) {
      if (!records.length) return;
      var box = svg.getBoundingClientRect();
      var px = ((event.clientX - box.left) / box.width) * W;
      var frac = (px - L) / (W - L - R);
      var idx = Math.round(frac * (records.length - 1));
      idx = Math.max(0, Math.min(records.length - 1, idx));

      crosshair.setAttribute('x1', x(idx));
      crosshair.setAttribute('x2', x(idx));
      crosshair.style.display = '';

      var html = '<div style="color:var(--text-secondary)">' +
                 esc(fmtDateTime(records[idx].t)) + '</div>';
      for (var s = 0; s < series.length; s++) {
        var v = series[s].values[idx];
        if (v === null || v === undefined || isNaN(v)) {
          dots[s].style.display = 'none';
          continue;
        }
        dots[s].setAttribute('cx', x(idx));
        dots[s].setAttribute('cy', y(v));
        dots[s].style.display = '';
        html += '<div><span class="swatch" style="background:' + series[s].color +
                '"></span>' + esc(series[s].name) + ': <b>' + v.toFixed(2) + '</b></div>';
      }
      tip.innerHTML = html;
      tip.style.display = 'block';
      tip.style.left = Math.min(event.clientX + 12, window.innerWidth - 190) + 'px';
      tip.style.top = (event.clientY + 12) + 'px';
    }

    svg.addEventListener('mousemove', move);
    svg.addEventListener('mouseleave', hide);
    svg.addEventListener('touchmove', function (e) {
      if (e.touches.length) move(e.touches[0]);
    });
    svg.addEventListener('touchend', hide);
  }

  // Relay state is binary, so it is a state strip, not a line: drawing 0/1 as a
  // line implies values in between.
  function drawRelays(container) {
    var n = records.length;
    if (!n || !relayNames.length) return;
    var rows = '';
    for (var r = 0; r < relayNames.length; r++) {
      var bars = '', runStart = -1;
      for (var i = 0; i <= n; i++) {
        var on = i < n && ((records[i].relays >> r) & 1);
        if (on && runStart < 0) runStart = i;
        if (!on && runStart >= 0) {
          var x0 = (runStart / n) * 100;
          var w = Math.max(((i - runStart) / n) * 100, 0.4);
          bars += '<rect x="' + x0 + '%" y="0" width="' + w +
                  '%" height="14" rx="2" fill="var(--series-1)"/>';
          runStart = -1;
        }
      }
      rows += '<div class="relay-row"><span class="relay-name">' +
              esc(relayNames[r]) + '</span>' +
              '<svg class="relay-strip" preserveAspectRatio="none">' +
              '<rect x="0" y="0" width="100%" height="14" rx="2" fill="var(--grid)"/>' +
              bars + '</svg></div>';
    }
    container.append($('<div class="card"><div class="card-header">' +
      '<span>Relays</span><span class="hint">shaded while energised</span></div>' +
      '<div class="card-body">' + rows + '</div></div>'));
  }

  // ---------- table view ----------
  // Not decoration: it is the accessible reading of the same data, and the
  // palette check requires it or direct labels wherever contrast is low.
  function drawTable() {
    var head = '<tr><th>Time</th>';
    for (var m = 0; m < probeCount; m++) head += '<th>Moist ' + (m + 1) + '</th>';
    head += '<th>Lum</th><th>Temp</th><th>Hum</th><th>Water</th><th>Relays</th></tr>';
    $('#thead-history').html(head);

    var rows = '';
    for (var i = records.length - 1; i >= 0; i--) {
      var r = records[i];
      rows += '<tr><td>' + esc(fmtDateTime(r.t)) + '</td>';
      for (var k = 0; k < probeCount; k++) {
        rows += '<td>' + (r.moisture[k] === null ? '—' : r.moisture[k]) + '</td>';
      }
      rows += '<td>' + (r.lum === null ? '—' : r.lum) + '</td>' +
              '<td>' + (r.temp === null ? '—' : r.temp) + '</td>' +
              '<td>' + (r.hum === null ? '—' : r.hum) + '</td>' +
              '<td>' + (r.water === null ? '—' : r.water) + '</td>' +
              '<td>' + ('000' + (r.relays >>> 0).toString(2)).slice(-4) + '</td></tr>';
    }
    $('#tbody-history').html(rows);
  }

  function column(key, index) {
    return records.map(function (r) {
      var v = index === undefined ? r[key] : r[key][index];
      return (v === null || v === undefined) ? null : v;
    });
  }

  function hasData(values) {
    for (var i = 0; i < values.length; i++) {
      if (values[i] !== null && values[i] !== undefined) return true;
    }
    return false;
  }

  function render() {
    var charts = $('#charts').empty();
    if (!records.length) {
      espUI.setStatus('info', 'No records yet — the device writes one per period.');
      return;
    }

    // One plot per measure. Probes share a plot because they share a unit;
    // everything else gets its own because it does not.
    var moisture = [];
    for (var m = 0; m < probeCount; m++) {
      var vals = column('moisture', m);
      if (hasData(vals)) {
        moisture.push({
          name: 'Probe ' + (m + 1),
          values: vals,
          color: SERIES[m % SERIES.length],
        });
      }
    }
    if (moisture.length) drawChart(charts, 'Soil Moisture', '%', moisture);

    var singles = [
      { key: 'lum', title: 'Luminosity', unit: '%' },
      { key: 'temp', title: 'Temperature', unit: '°C' },
      { key: 'hum', title: 'Air Humidity', unit: '%' },
      { key: 'water', title: 'Water Level', unit: 'cm' },
    ];
    for (var s = 0; s < singles.length; s++) {
      var v = column(singles[s].key);
      if (hasData(v)) {
        drawChart(charts, singles[s].title, singles[s].unit,
                  [{ name: singles[s].title, values: v, color: SERIES[0] }]);
      }
    }

    drawRelays(charts);
    drawTable();
  }

  function load() {
    var limit = parseInt($('#select-limit').val(), 10);
    var url = '/history.json?limit=' + limit;
    if (offset !== null) url += '&offset=' + offset;

    espUI.setStatus('info', 'Loading…');
    $.getJSON(url)
      .done(function (data) {
        records = data.records || [];
        stored = data.stored;
        offset = data.offset;
        probeCount = 0;
        for (var i = 0; i < records.length; i++) {
          probeCount = Math.max(probeCount, (records[i].moisture || []).length);
        }
        $('#range-note').text(
          'records ' + (offset + 1) + '–' + (offset + data.returned) +
          ' of ' + stored + ' (capacity ' + data.capacity + ')');
        // Without paging the newest `limit` were the only records reachable,
        // so a 1440-record buffer showed its final 14 % and wrote the rest for
        // nobody.
        $('#button-older').prop('disabled', offset <= 0);
        $('#button-newer').prop('disabled', offset + data.returned >= stored);
        $('#status').empty();
        render();
      })
      .fail(function (xhr) {
        espUI.setStatus('danger', xhr.status === 503
          ? 'The history buffer is disabled or failed to open.'
          : 'Could not read the history.');
      });
  }

  $(function () {
    if (!espAuth.requireToken()) return;

    // Labels and relay count come from the device, not from this file.
    $.getJSON('/data.json').done(function (info) {
      relayNames = (info.Relays || []).map(function (r) { return r.name; });
      if (records.length) render();
    });

    $('#select-limit').on('change', function () { offset = null; load(); });
    $('#button-refresh').on('click', function () { offset = null; load(); });
    $('#button-older').on('click', function () {
      var limit = parseInt($('#select-limit').val(), 10);
      offset = Math.max(0, (offset === null ? stored - limit : offset) - limit);
      load();
    });
    $('#button-newer').on('click', function () {
      var limit = parseInt($('#select-limit').val(), 10);
      offset = Math.min(Math.max(0, stored - limit), (offset || 0) + limit);
      load();
    });
    $('#input-table').on('change', function () {
      $('#card-table').toggle(this.checked);
    });
    $('#a-logout').on('click', function (event) {
      event.preventDefault();
      espAuth.logout();
    });

    load();
  });
})();
