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
    $('#input-watering').prop('checked', String(info.Outputs && info.Outputs.Watering) === '1');
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

    $('#input-watering').on('click', function (event) {
      event.preventDefault();
      var seconds = parseInt($('#input-watering-time').val(), 10) || 5;
      if (this.checked && confirm('Start watering for ' + seconds + ' seconds?')) {
        $.post('/control', { wateringTime: 1000 * seconds });
      } else {
        this.checked = false;
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
