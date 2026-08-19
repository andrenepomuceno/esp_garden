// ESP Garden — dashboard UI
(function () {
  var POLL_INTERVAL_MS = 1000;
  var REQUEST_TIMEOUT_MS = 1500;
  var pollTimer = null;
  var pageVisible = true;

  // ---------- helpers ----------
  function statusBadge(value) {
    var v = String(value).toLowerCase();
    var cls = 'text-bg-secondary';
    if (v === 'online' || v === 'enabled') cls = 'text-bg-success';
    else if (v === 'offline' || v === 'disabled') cls = 'text-bg-danger';
    return '<span class="badge ' + cls + '">' + value + '</span>';
  }

  function formatStatusValue(key, value) {
    if (key === 'Internet' || key === 'MQTT') return statusBadge(value);
    if (key === 'Signal Strength') {
      var pct = parseInt(value, 10);
      var cls = 'text-bg-success';
      if (isNaN(pct)) cls = 'text-bg-secondary';
      else if (pct < 30) cls = 'text-bg-danger';
      else if (pct < 60) cls = 'text-bg-warning';
      return '<span class="badge num-badge ' + cls + '">' + value + '</span>';
    }
    return '<span class="num-badge">' + value + '</span>';
  }

  function fillStatus(data) {
    var rows = '';
    for (var key in data) {
      rows += '<tr><th scope="row" class="fw-normal text-muted">' + key + '</th>' +
              '<td class="text-end">' + formatStatusValue(key, data[key]) + '</td></tr>';
    }
    $('#tbody-status').html(rows);
  }

  function fillInputs(data) {
    var rows = '';
    for (var key in data) {
      var d = data[key] || {};
      rows += '<tr>' +
              '<th scope="row" class="fw-normal">' + key + '</th>' +
              '<td class="text-end num-badge">' + (d.val || '') + '</td>' +
              '<td class="text-end num-badge text-muted">' + (d.avg || '') + '</td>' +
              '<td class="text-end num-badge text-muted">' + (d.var || '') + '</td>' +
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
      rows += '<tr><th scope="row" class="fw-normal">' + key + '</th>' +
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
                ' data-name="' + relays[i].name + '">' + relays[i].name + '</button></div>';
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
  function poll() {
    $.ajax({
      dataType: 'json',
      url: '/data.json',
      timeout: REQUEST_TIMEOUT_MS,
      success: updateUI,
      error: function () { setConnection(false); }
    });

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
    });
  }

  function startPolling() {
    if (pollTimer) return;
    poll();
    pollTimer = setInterval(function () {
      if (pageVisible) poll();
    }, POLL_INTERVAL_MS);
  }

  function stopPolling() {
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  }

  // ---------- handlers ----------
  $(function () {
    startPolling();

    document.addEventListener('visibilitychange', function () {
      pageVisible = !document.hidden;
      if (pageVisible) poll();
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
