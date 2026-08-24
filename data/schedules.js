// ESP Garden — scheduled watering editor.
//
// Unlike the devices page, Add and Delete are real here: the schedule list is
// data, not a reflection of how many peripherals were compiled in. The only
// ceiling is SCHEDULE_COUNT in the firmware, which ignores entries past it.
(function () {
  var MAX_SCHEDULES = 8;              // mirrors SCHEDULE_COUNT
  var DAYS = ['S', 'M', 'T', 'W', 'T', 'F', 'S'];   // bit 0 = Sunday
  var DAY_TITLES = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday',
                    'Friday', 'Saturday'];
  var doc = null;        // the whole config document, posted back intact
  var relays = [];       // {index, name} from /data.json

  function esc(s) { return espUI.escapeHtml(s); }

  function pad(n) { return (n < 10 ? '0' : '') + n; }

  function render() {
    var list = (doc && doc.schedules) || [];
    var rows = '';

    for (var i = 0; i < list.length; i++) {
      var s = list[i];
      var days = (typeof s.days === 'number') ? s.days : 127;
      var hour = (typeof s.hour === 'number') ? s.hour : 0;
      var minute = (typeof s.minute === 'number') ? s.minute : 0;
      // The device stores milliseconds; nobody schedules a pump in
      // milliseconds, so the field is seconds and converts on save.
      var seconds = Math.round(((typeof s.durationMs === 'number') ? s.durationMs : 0) / 1000);

      var relayOptions = '';
      var relayKnown = false;
      for (var r = 0; r < relays.length; r++) {
        if (relays[r].index === s.relay) relayKnown = true;
        relayOptions += '<option value="' + relays[r].index + '"' +
                        (relays[r].index === s.relay ? ' selected' : '') + '>' +
                        esc(relays[r].name) + '</option>';
      }
      // A stored index this board does not report — a config carried over from
      // a board with more relays — keeps its own option instead of falling
      // through to the first one. Silently retargeting a schedule to relay 0
      // would point it at the watering pump.
      if (!relayKnown && typeof s.relay === 'number') {
        relayOptions += '<option value="' + s.relay + '" selected>relay ' +
                        s.relay + ' (not on this board)</option>';
      }

      var dayBoxes = '';
      for (var d = 0; d < 7; d++) {
        dayBoxes += '<span class="day-box" title="' + DAY_TITLES[d] + '">' +
                    '<input type="checkbox" class="form-check-input sch-day"' +
                    ' data-row="' + i + '" data-bit="' + d + '"' +
                    ((days & (1 << d)) ? ' checked' : '') + '>' +
                    '<label>' + DAYS[d] + '</label></span>';
      }

      rows += '<tr>' +
        '<td><input type="checkbox" class="form-check-input sch-enabled" data-row="' + i + '"' +
          (s.enabled === false ? '' : ' checked') + '></td>' +
        '<td><input type="text" class="form-control form-control-sm sch-name" data-row="' + i +
          '" value="' + esc(s.name || ('Schedule ' + (i + 1))) + '"></td>' +
        '<td><select class="form-select form-select-sm sch-relay" data-row="' + i + '">' +
          relayOptions + '</select></td>' +
        '<td><input type="time" class="form-control form-control-sm sch-time" data-row="' + i +
          '" value="' + pad(hour) + ':' + pad(minute) + '"></td>' +
        '<td>' + dayBoxes + '</td>' +
        '<td><input type="number" min="1" max="30" class="form-control form-control-sm sch-seconds"' +
          ' data-row="' + i + '" value="' + seconds + '"></td>' +
        '<td><button type="button" class="btn btn-outline-danger btn-sm btn-remove"' +
          ' data-row="' + i + '">&minus;</button></td>' +
        '</tr>';
    }

    if (!list.length) {
      rows = '<tr><td colspan="7" class="text-muted small">No schedules. ' +
             'Add one, then restart the device.</td></tr>';
    }
    $('#tbody-schedules').html(rows);
    $('#count-note').text(list.length + ' of ' + MAX_SCHEDULES +
                          ' (the firmware ignores any past ' + MAX_SCHEDULES + ')');
    $('#button-add').prop('disabled', list.length >= MAX_SCHEDULES);
  }

  // Reads the form back into doc.schedules. Returns a list of problems rather
  // than saving a document the device would reject at load — it drops a bad
  // schedule with a log line nobody reads.
  function collect() {
    var problems = [];
    var list = doc.schedules || [];

    $('.sch-enabled').each(function () {
      list[$(this).data('row')].enabled = $(this).prop('checked');
    });
    $('.sch-name').each(function () {
      list[$(this).data('row')].name = $(this).val();
    });
    $('.sch-relay').each(function () {
      var row = $(this).data('row');
      var value = parseInt($(this).val(), 10);
      // An empty <select> yields NaN, which JSON.stringify writes as null and
      // the device reads as relay 0 — the watering pump. Leave the stored value
      // alone and let save() refuse instead.
      if (isNaN(value)) {
        problems.push('row ' + (row + 1) + ': no relay selected');
        return;
      }
      list[row].relay = value;
    });
    $('.sch-time').each(function () {
      var row = $(this).data('row');
      var parts = String($(this).val()).split(':');
      var h = parseInt(parts[0], 10), m = parseInt(parts[1], 10);
      if (isNaN(h) || isNaN(m)) {
        problems.push('row ' + (row + 1) + ': invalid time');
        return;
      }
      list[row].hour = h;
      list[row].minute = m;
    });
    $('.sch-seconds').each(function () {
      var row = $(this).data('row');
      var v = parseInt($(this).val(), 10);
      // 30 s is g_relayMaxTime; startRelay() refuses more, so a longer
      // schedule would fire and be rejected every single time.
      if (isNaN(v) || v < 1 || v > 30) {
        problems.push('row ' + (row + 1) + ': seconds must be 1-30');
        return;
      }
      list[row].durationMs = v * 1000;
    });

    for (var i = 0; i < list.length; i++) {
      list[i].days = 0;
    }
    $('.sch-day').each(function () {
      if ($(this).prop('checked')) {
        var row = $(this).data('row');
        list[row].days |= (1 << $(this).data('bit'));
      }
    });

    for (var j = 0; j < list.length; j++) {
      if (list[j].enabled && list[j].days === 0) {
        problems.push('row ' + (j + 1) + ': enabled but no day selected');
      }
    }

    return problems;
  }

  function load() {
    espUI.setStatus('info', 'Loading…');
    $('#button-save').prop('disabled', true);

    // Relay names first: the picker has to show what the device calls them,
    // and /data.json's Relays array is the addressing contract.
    $.getJSON('/data.json')
      .done(function (info) {
        relays = (info.Relays || []).map(function (r) {
          return { index: r.index, name: r.name };
        });
      })
      .fail(function () {
        // Loading the config anyway would render every relay picker empty, and
        // saving from there rewrites each schedule's target. Better to show
        // nothing than to show a form that silently repoints the pumps.
        relays = [];
      })
      .always(function () {
        if (!relays.length) {
          espUI.setStatus('danger', 'Could not read the relay list from ' +
            '/data.json — reload before editing, or a save would repoint ' +
            'every schedule.');
          return;
        }
        $.getJSON('/config.json')
          .done(function (config) {
            doc = config;
            if (!doc.schedules || Object.prototype.toString.call(doc.schedules) !== '[object Array]') {
              doc.schedules = [];
            }
            render();
            $('#button-save').prop('disabled', false);
            $('#status').empty();
          })
          .fail(function (xhr) {
            espUI.setStatus('danger', xhr.status === 403
              ? 'This account is not an administrator.'
              : 'Could not read the configuration.');
          });
      });
  }

  $(function () {
    if (!espAuth.requireToken()) return;
    load();

    $('#button-add').on('click', function () {
      collect();
      if (doc.schedules.length >= MAX_SCHEDULES) return;
      doc.schedules.push({
        name: 'Schedule ' + (doc.schedules.length + 1),
        relay: relays.length ? relays[0].index : 0,
        hour: 6, minute: 0, days: 127, durationMs: 10000,
        // New rows start disabled: adding a schedule should never start
        // watering something the user has not looked at yet.
        enabled: false,
      });
      render();
    });

    $('#tbody-schedules').on('click', '.btn-remove', function () {
      var row = $(this).data('row');
      if (!confirm('Remove "' + (doc.schedules[row].name || 'schedule') + '"?')) return;
      collect();
      doc.schedules.splice(row, 1);
      render();
    });

    $('#button-save').on('click', function () {
      var problems = collect();
      if (problems.length) {
        espUI.setStatus('danger', 'Not saved — ' + problems.join('; '));
        return;
      }

      $('#button-save').prop('disabled', true);
      $.post('/config.json', { config: JSON.stringify(doc) })
        .done(function () {
          espUI.setStatus('success', 'Saved. Restart the device to apply.');
        })
        .fail(function (xhr) {
          espUI.setStatus('danger', 'Save failed: ' + (xhr.responseText || xhr.status));
        })
        .always(function () { $('#button-save').prop('disabled', false); });
    });

    $('#button-reload').on('click', load);

    $('#button-restart').on('click', function () {
      if (!confirm('Restart the device now?')) return;
      espUI.setStatus('info', 'Restarting…');
      $.post('/control', { reset: '1' })
        .done(function () {
          espUI.setStatus('info', 'Restarting… reload the page in a few seconds.');
        })
        .fail(function (xhr) {
          espUI.setStatus('danger', 'Restart failed (' + (xhr.status || 'no response') + ').');
        });
    });

    $('#a-logout').on('click', function (event) {
      event.preventDefault();
      espAuth.logout();
    });
  });
})();
