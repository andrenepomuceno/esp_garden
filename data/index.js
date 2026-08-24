// ESP Garden — dashboard UI
(function () {
  // Every label below comes from /data.json, and several of them are config
  // strings an admin typed: the sensor and relay names, the hostname. Building
  // a row by string concatenation means a name containing markup executes in
  // every dashboard session, including another admin's. escapeHtml is the same
  // helper auth.js uses for device response bodies.
  function esc(value) {
    return espUI.escapeHtml(value === undefined || value === null ? '' : value);
  }

  var POLL_INTERVAL_MS = 1000;
  var REQUEST_TIMEOUT_MS = 1500;
  var pollTimer = null;
  var polling = false;
  var pollGeneration = 0;
  var pageVisible = true;

  // ---------- helpers ----------
  function statusBadge(value) {
    var v = String(value).toLowerCase();
    var cls = 'text-bg-secondary';
    if (v === 'online' || v === 'enabled') cls = 'text-bg-success';
    else if (v === 'offline' || v === 'disabled') cls = 'text-bg-danger';
    return '<span class="badge ' + cls + '">' + esc(value) + '</span>';
  }

  function formatStatusValue(key, value) {
    if (key === 'Internet' || key === 'MQTT') return statusBadge(value);
    if (key === 'Signal Strength') {
      var pct = parseInt(value, 10);
      var cls = 'text-bg-success';
      if (isNaN(pct)) cls = 'text-bg-secondary';
      else if (pct < 30) cls = 'text-bg-danger';
      else if (pct < 60) cls = 'text-bg-warning';
      return '<span class="badge num-badge ' + cls + '">' + esc(value) + '</span>';
    }
    return '<span class="num-badge">' + esc(value) + '</span>';
  }

  function fillStatus(data) {
    var rows = '';
    for (var key in data) {
      rows += '<tr><th scope="row" class="fw-normal text-muted">' + esc(key) + '</th>' +
              '<td class="text-end">' + formatStatusValue(key, data[key]) + '</td></tr>';
    }
    $('#tbody-status').html(rows);
  }

  function fillInputs(data) {
    var rows = '';
    for (var key in data) {
      var d = data[key] || {};
      rows += '<tr>' +
              '<th scope="row" class="fw-normal">' + esc(key) + '</th>' +
              '<td class="text-end num-badge">' + esc(d.val || '') +
              (d.state ? ' <span class="badge text-bg-secondary">' + esc(d.state) + '</span>' : '') +
              '</td>' +
              '<td class="text-end num-badge text-muted">' + esc(d.avg || '') + '</td>' +
              '<td class="text-end num-badge text-muted">' + esc(d.var || '') + '</td>' +
              '</tr>';
    }
    $('#tbody-inputs').html(rows);
  }

  function fillOutputs(data) {
    var rows = '';
    for (var key in data) {
      var v = data[key];
      var label = (v == 1 || v === '1') ? '<span class="badge text-bg-info">ON</span>'
                                        : '<span class="badge text-bg-secondary">OFF</span>';
      rows += '<tr><th scope="row" class="fw-normal">' + esc(key) + '</th>' +
              '<td class="text-end">' + label + '</td></tr>';
    }
    $('#tbody-outputs').html(rows);
  }

  // The button set is rebuilt only when the relay list itself changes: polling
  // runs once a second and re-rendering every tick would fight the user's click.
  var relaySignature = '';

  function renderRelays(relays) {
    if (!relays || !relays.length) return;

    var signature = relays.map(function (r) { return r.index + ':' + r.name; }).join('|');
    if (signature !== relaySignature) {
      relaySignature = signature;
      var html = '';
      for (var i = 0; i < relays.length; i++) {
        html += '<div><button type="button" class="btn btn-outline-primary btn-sm relay-btn"' +
                ' data-index="' + relays[i].index + '"' +
                ' data-name="' + esc(relays[i].name) + '">' + esc(relays[i].name) + '</button></div>';
      }
      $('#relay-buttons').html(html);
    }

    for (var j = 0; j < relays.length; j++) {
      var relay = relays[j];
      var btn = $('.relay-btn[data-index="' + relay.index + '"]');
      var active = (relay.on == 1 || relay.on === '1');
      btn.toggleClass('btn-outline-primary', !active).toggleClass('btn-info', active);
      if (active) {
        btn.text(relay.name + ' — ' + Math.ceil((relay.remaining || 0) / 1000) + 's');
      } else {
        btn.text(relay.name);
      }
    }
  }

  function setConnection(online) {
    var dot = $('#conn-dot');
    if (online) {
      dot.addClass('online');
      $('#conn-text').text('online');
    } else {
      dot.removeClass('online');
      $('#conn-text').text('disconnected');
    }
  }

  function updateUI(info) {
    setConnection(true);
    var hostname = info.Status && info.Status.Hostname;
    if (hostname) {
      document.title = hostname + ' — ESP Garden';
      $('#status-host').text(hostname);
    }
    fillStatus(info.Status || {});
    fillInputs(info.Inputs || {});
    fillOutputs(info.Outputs || {});
    renderRelays(info.Relays);
    $('#input-mqtt').prop('checked', info.Status && info.Status.MQTT === 'enabled');
    if (info.Channel) {
      $('#a-thingspeak-link').attr('href', 'https://thingspeak.com/channels/' + info.Channel);
    }
  }

  // ---------- polling ----------
  // Re-arms from BOTH callbacks rather than running on a setInterval. The
  // device serves this from a single async_tcp task; with setInterval, a slow
  // or rebooting board — the normal state of a garden node — stacks requests
  // faster than they complete, exhausts the AsyncTCP buffers and drops
  // connections, and the page then stays dead even after the device recovers.
  // A self-rearming timeout can never have two requests in flight.
  function poll() {
    // Both requests must land before the next tick is armed, so the page can
    // never have more in flight than it started.
    var pending = 2;
    var generation = pollGeneration;
    function finish() {
      // `generation` is what stops a request that was still in flight across a
      // stopPolling()/startPolling() from arming a tick for a chain that no
      // longer owns the page.
      if (--pending > 0 || !polling || generation !== pollGeneration) return;
      pollTimer = setTimeout(tick, POLL_INTERVAL_MS);
    }

    $.ajax({
      dataType: 'json',
      url: '/data.json',
      timeout: REQUEST_TIMEOUT_MS,
      success: updateUI,
      error: function () { setConnection(false); }
    }).always(finish);

    $.ajax({
      url: '/logs',
      timeout: REQUEST_TIMEOUT_MS,
      success: function (log) {
        var ta = $('#textarea-logs');
        ta.val(log);
        if ($('#input-scroll').prop('checked')) {
          ta.scrollTop(ta[0].scrollHeight);
        }
      }
    }).always(finish);
  }

  function tick() {
    if (!polling) return;
    if (pageVisible) {
      poll();
      return;
    }
    // Hidden tab: idle without issuing requests, but keep the loop armed so
    // coming back does not wait for a fresh start.
    pollTimer = setTimeout(tick, POLL_INTERVAL_MS);
  }

  function startPolling() {
    if (polling) return;
    polling = true;
    ++pollGeneration;
    poll();
  }

  function stopPolling() {
    polling = false;
    ++pollGeneration;
    if (pollTimer) { clearTimeout(pollTimer); pollTimer = null; }
  }

  // ---------- handlers ----------
  $(function () {
    // Without a token every guarded endpoint answers 401, which would show an
    // empty dashboard instead of a login prompt.
    if (!espAuth.requireToken()) return;

    startPolling();

    $('#a-logout').on('click', function (event) {
      event.preventDefault();
      stopPolling();
      espAuth.logout();
    });

    document.addEventListener('visibilitychange', function () {
      pageVisible = !document.hidden;
      // Cancel the armed tick and start ONE new chain, rather than calling
      // poll() alongside a timer that is still pending. Calling it directly
      // forked a second chain on every hide/show, doubling the request rate
      // each time — the exact stacking the setTimeout rewrite exists to
      // prevent, reintroduced three lines away from the comment explaining it.
      if (pageVisible && polling) {
        if (pollTimer) { clearTimeout(pollTimer); pollTimer = null; }
        poll();
      }
    });

    // Delegated: the buttons are generated from /data.json after this runs.
    $('#relay-buttons').on('click', '.relay-btn', function (event) {
      event.preventDefault();
      var index = $(this).data('index');
      var name = $(this).data('name');
      var seconds = parseInt($('#input-watering-time').val(), 10) || 5;
      if (confirm('Activate ' + name + ' for ' + seconds + ' seconds?')) {
        $.post('/control', { relay: index, relayTime: 1000 * seconds });
      }
    });

    $('#input-mqtt').on('click', function (event) {
      event.preventDefault();
      $.post('/control', { mqtt: this.checked ? 'enable' : 'disable' });
    });

    $('#button-reset').on('click', function (event) {
      event.preventDefault();
      if (confirm('Reset the device now?')) {
        $.post('/control', { reset: '1' });
        setConnection(false);
      }
    });

    $('#button-logs-copy').on('click', function () {
      var text = $('#textarea-logs').val() || '';
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text);
      } else {
        var ta = document.getElementById('textarea-logs');
        ta.select();
        try { document.execCommand('copy'); } catch (e) { /* ignore */ }
      }
      var btn = $(this);
      var orig = btn.html();
      btn.html('&#x2705;');
      setTimeout(function () { btn.html(orig); }, 1000);
    });
  });
})();
