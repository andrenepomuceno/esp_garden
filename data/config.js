// ESP Garden — configuration editor.
//
// The form is generated from whatever GET /config.json returns rather than from
// a hardcoded schema: the document differs per board (RELAY_COUNT, which sensors
// are compiled in), and a fixed form would silently drop the fields it does not
// know about when saving.
(function () {
  var MASK = '********';
  var original = null;          // last document fetched, used as the save template
  var esc = null;               // bound in $() once espUI is available
  var activeTab = '__device';   // survives a re-render so saving keeps your place

  function setStatus(kind, message) { espUI.setStatus(kind, message); }

  function label(key) {
    return key.replace(/([a-z0-9])([A-Z])/g, '$1 $2').replace(/^./, function (c) {
      return c.toUpperCase();
    });
  }

  function isArray(v) {
    return Object.prototype.toString.call(v) === '[object Array]';
  }

  function isPlainObject(v) {
    return v !== null && typeof v === 'object' && !isArray(v);
  }

  // ---------- rendering ----------
  function field(path, key, value) {
    var id = 'f_' + path.replace(/\./g, '_');
    var common = 'class="form-control form-control-sm" id="' + id +
                 '" data-path="' + esc(path) + '"';

    if (value === MASK) {
      // autocomplete="new-password" is load-bearing, not cosmetic. Without it a
      // password manager fills the site credential into the first password
      // field it finds; the mask is then gone, the device stops restoring the
      // stored value, and the saved WiFi password is overwritten with the login
      // one — which only surfaces as an unreachable device after a reboot.
      return '<div class="field-row"><label class="form-label small" for="' + id + '">' +
             esc(label(key)) + '</label>' +
             '<input type="password" ' + common + ' data-type="secret"' +
             ' autocomplete="new-password" value="' + MASK + '">' +
             '<div class="form-text hint">Stored value kept unless you change this.</div></div>';
    }
    if (typeof value === 'boolean') {
      return '<div class="field-row form-check form-switch">' +
             '<input class="form-check-input" type="checkbox" id="' + id + '"' +
             ' data-path="' + esc(path) + '" data-type="boolean"' +
             (value ? ' checked' : '') + '>' +
             '<label class="form-check-label small" for="' + id + '">' + esc(label(key)) +
             '</label></div>';
    }
    if (typeof value === 'number') {
      return '<div class="field-row"><label class="form-label small" for="' + id + '">' +
             esc(label(key)) + '</label>' +
             '<input type="number" ' + common + ' data-type="number" value="' + esc(value) + '"></div>';
    }
    if (isArray(value)) {
      // Only an all-numeric array is editable as a comma list. Anything else
      // would be destroyed by the parseInt on save, so it is shown read-only
      // rather than silently rewritten.
      var allNumbers = true;
      $.each(value, function (_, v) { if (typeof v !== 'number') allNumbers = false; });
      if (!allNumbers) {
        return '<div class="field-row"><label class="form-label small">' + esc(label(key)) +
               '</label><input type="text" class="form-control form-control-sm" readonly value="' +
               esc(JSON.stringify(value)) + '">' +
               '<div class="form-text hint">Not editable here; left untouched on save.</div></div>';
      }
      return '<div class="field-row"><label class="form-label small" for="' + id + '">' +
             esc(label(key)) + '</label>' +
             '<input type="text" ' + common + ' data-type="numberArray" value="' +
             esc(value.join(', ')) + '">' +
             '<div class="form-text hint">Comma separated.</div></div>';
    }
    var readonly = (path === 'id') ? ' readonly' : '';
    return '<div class="field-row"><label class="form-label small" for="' + id + '">' +
           esc(label(key)) + '</label>' +
           '<input type="text" ' + common + ' data-type="string" value="' + esc(value) + '"' +
           readonly + '></div>';
  }

  // Array of objects, e.g. io.relays — one row per entry, one input per column.
  function objectTable(path, key, rows) {
    var columns = [];
    $.each(rows, function (_, row) {
      $.each(row, function (col) {
        if ($.inArray(col, columns) === -1) columns.push(col);
      });
    });

    var html = '<div class="field-row" data-array="' + esc(path) + '">' +
               '<label class="form-label small">' + esc(label(key)) +
               '</label><table class="table table-sm table-striped mb-1"><thead><tr><th>#</th>';
    $.each(columns, function (_, c) { html += '<th>' + esc(label(c)) + '</th>'; });
    html += '<th></th></tr></thead><tbody>';

    $.each(rows, function (i, row) {
      html += '<tr><td class="text-muted small">' + (i + 1) + '</td>';
      $.each(columns, function (_, col) {
        var present = Object.prototype.hasOwnProperty.call(row, col);
        var v = present ? row[col] : '';
        var p = path + '.' + i + '.' + col;
        // A row may legitimately omit a column — loadRelays() tolerates a
        // missing name. Rendering row[col] straight would print "undefined"
        // and save that string; worse, for a numeric column the device casts a
        // non-numeric string to 0 and moves the relay to GPIO 0.
        var type = (typeof v === 'number') ? 'number' : 'string';
        html += '<td><input class="form-control form-control-sm" data-path="' + esc(p) +
                '" data-type="' + type + '"' + (present ? '' : ' data-optional="1"') +
                ' type="' + (type === 'number' ? 'number' : 'text') +
                '" value="' + esc(v) + '"></td>';
      });
      html += '<td><button type="button" class="btn btn-outline-danger btn-sm btn-remove-row"' +
              ' data-array="' + esc(path) + '" data-index="' + i + '">&minus;</button></td></tr>';
    });

    return html + '</tbody></table>' +
           '<button type="button" class="btn btn-outline-secondary btn-sm btn-add-row"' +
           ' data-array="' + esc(path) + '">+ Add</button></div>';
  }

  function section(name, obj, path) {
    var body = '';
    $.each(obj, function (key, value) {
      var p = path + '.' + key;
      if (isArray(value) && value.length && isPlainObject(value[0])) {
        body += objectTable(p, key, value);
      } else if (isPlainObject(value)) {
        body += section(key, value, p);
      } else {
        body += field(p, key, value);
      }
    });

    return '<div class="card"><div class="card-header">' + esc(label(name)) +
           '</div><div class="card-body">' + body + '</div></div>';
  }

  // Tabs are switched in plain jQuery rather than with Bootstrap's JS: the
  // pages deliberately carry no CDN script, and the nav-tabs classes below are
  // CSS only. Every pane stays in the DOM, so collect() still sees the inputs
  // of tabs the user never opened.
  function render(doc) {
    var scalars = {}, objects = {};
    $.each(doc, function (k, v) {
      if (v !== null && typeof v === 'object' && !isArray(v)) objects[k] = v;
      else scalars[k] = v;
    });

    var tabs = [{ key: '__device', title: 'Device' }];
    $.each(objects, function (k) { tabs.push({ key: k, title: label(k) }); });

    var known = false;
    $.each(tabs, function (_, t) { if (t.key === activeTab) known = true; });
    if (!known) activeTab = tabs[0].key;

    var nav = '<ul class="nav nav-tabs mb-3" id="config-tabs">';
    $.each(tabs, function (_, t) {
      nav += '<li class="nav-item"><a class="nav-link' +
             (t.key === activeTab ? ' active' : '') + '" href="#" data-tab="' +
             esc(t.key) + '">' + esc(t.title) + '</a></li>';
    });
    nav += '</ul>';

    var panes = '<div class="tab-content">';
    panes += '<div class="config-pane" data-pane="__device"' +
             (activeTab === '__device' ? '' : ' style="display:none"') + '>' +
             '<div class="card"><div class="card-header">Device</div><div class="card-body">';
    $.each(scalars, function (k, v) { panes += field(k, k, v); });
    panes += '</div></div></div>';

    $.each(objects, function (k, v) {
      panes += '<div class="config-pane" data-pane="' + esc(k) + '"' +
               (k === activeTab ? '' : ' style="display:none"') + '>' +
               section(k, v, k) + '</div>';
    });
    panes += '</div>';

    $('#sections').html(nav + panes);
  }

  function showTab(key) {
    activeTab = key;
    $('#config-tabs').find('.nav-link').each(function () {
      $(this).toggleClass('active', $(this).data('tab') === key);
    });
    $('#sections').find('.config-pane').each(function () {
      $(this).toggle($(this).data('pane') === key);
    });
  }

  // ---------- save ----------
  function getPath(obj, path) {
    var parts = path.split('.');
    var node = obj;
    for (var i = 0; i < parts.length; i++) {
      if (node === null || typeof node !== 'object') return undefined;
      node = node[parts[i]];
    }
    return node;
  }

  function setPath(obj, path, value) {
    var parts = path.split('.');
    var node = obj;
    for (var i = 0; i < parts.length - 1; i++) {
      if (node[parts[i]] === undefined) node[parts[i]] = {};
      node = node[parts[i]];
    }
    node[parts[parts.length - 1]] = value;
  }

  // Returns {doc, invalid:[paths]} — an unparseable number is reported rather
  // than dropped, so the user is never told "Saved" about an edit that was
  // thrown away.
  function collect() {
    var doc = JSON.parse(JSON.stringify(original));
    var invalid = [];

    $('#sections').find('[data-path]').each(function () {
      var input = $(this);
      var path = input.data('path');
      var type = input.data('type');
      var raw = (type === 'boolean') ? input.prop('checked') : input.val();

      if (type === 'number') {
        var n = parseInt(raw, 10);
        if (isNaN(n)) { invalid.push(path); return; }
        setPath(doc, path, n);
      } else if (type === 'numberArray') {
        var list = [];
        var bad = false;
        $.each(String(raw).split(','), function (_, part) {
          part = $.trim(part);
          if (part === '') return;
          var v = parseInt(part, 10);
          if (isNaN(v)) bad = true; else list.push(v);
        });
        if (bad) { invalid.push(path); return; }
        setPath(doc, path, list);
      } else {
        // A column the row never had stays absent unless the user typed
        // something, so an optional key is not invented as an empty string.
        if (input.data('optional') && $.trim(String(raw)) === '' &&
            getPath(original, path) === undefined) {
          return;
        }
        setPath(doc, path, raw);
      }
    });

    return { doc: doc, invalid: invalid };
  }

  function blankLike(rows) {
    var blank = {};
    $.each(rows, function (_, row) {
      $.each(row, function (col, v) {
        if (!(col in blank)) blank[col] = (typeof v === 'number') ? 0 : '';
      });
    });
    return blank;
  }

  function load(keepStatus) {
    if (!keepStatus) setStatus('info', 'Loading…');
    $('#button-save').prop('disabled', true);

    $.getJSON('/config.json')
      .done(function (doc) {
        original = doc;
        render(doc);
        $('#button-save').prop('disabled', false);
        if (!keepStatus) $('#status').empty();
      })
      .fail(function (xhr) {
        original = null;
        $('#sections').empty();
        setStatus('danger', xhr.status === 403
          ? 'This account is not an administrator.'
          : 'Could not read the configuration.');
      });
  }

  $(function () {
    if (!espAuth.requireToken()) return;
    esc = espUI.escapeHtml;
    load();

    $('#sections').on('click', '#config-tabs .nav-link', function (event) {
      event.preventDefault();
      showTab($(this).data('tab'));
    });

    $('#sections').on('click', '.btn-add-row', function () {
      var path = $(this).data('array');
      var current = collect().doc;
      var rows = getPath(current, path) || [];
      rows.push(blankLike(rows));
      setPath(current, path, rows);
      original = current;
      render(current);
    });

    $('#sections').on('click', '.btn-remove-row', function () {
      var path = $(this).data('array');
      var index = $(this).data('index');
      var current = collect().doc;
      var rows = getPath(current, path) || [];
      rows.splice(index, 1);
      setPath(current, path, rows);
      original = current;
      render(current);
    });

    $('#config-form').on('submit', function (event) {
      event.preventDefault();
      if (original === null) {
        setStatus('danger', 'Nothing loaded to save.');
        return;
      }

      var result = collect();
      if (result.invalid.length) {
        setStatus('danger', 'Not a valid number: ' + result.invalid.join(', ') +
                            '. Nothing was saved.');
        return;
      }

      // The write replaces the whole file and only takes effect after a reboot,
      // so it is confirmed like the other destructive actions — and because
      // pressing Enter in any field submits this form.
      if (!confirm('Overwrite the device configuration?')) return;

      $('#button-save').prop('disabled', true);
      $.post('/config.json', { config: JSON.stringify(result.doc) })
        .done(function (saved) {
          // Changing ota.password rewrites the login credential, and the device
          // ends every session of that account — this one included. Reloading
          // the form would just 401 and bounce, so say why first.
          //
          // It must ALSO say that a restart is still owed. The response carries
          // saved.restartRequired on both paths, and this branch used to
          // replace the whole message with the sign-in notice and then navigate
          // away — so every other field written by the same POST (SSID, MQTT
          // backend, pins) waited for a reboot nobody had been told about. The
          // password is the one field that takes effect immediately; saying so
          // is what stops the sign-out from reading as "it is all applied".
          if (saved && saved.reauth) {
            setStatus('warning', 'Saved. Restart the device to apply — the new ' +
                                 'password is already in force, so your sessions ' +
                                 'were ended. Sign in again.');
            // Longer than the plain path's dwell, because the operator has one
            // sentence to read before the page navigates itself away.
            setTimeout(function () { espAuth.clearToken(); espAuth.redirectToLogin(); }, 5000);
            return;
          }
          load(true);
          setStatus('success', 'Saved. Restart the device to apply.');
        })
        .fail(function (xhr) {
          setStatus('danger', 'Save failed: ' + (xhr.responseText || xhr.status));
        })
        .always(function () { $('#button-save').prop('disabled', false); });
    });

    $('#button-reload').on('click', function () { load(); });

    $('#button-restart').on('click', function () {
      if (!confirm('Restart the device now?')) return;
      setStatus('info', 'Restarting…');
      $.post('/control', { reset: '1' })
        .done(function () { setStatus('info', 'Restarting… reload the page in a few seconds.'); })
        .fail(function (xhr) {
          setStatus('danger', xhr.status === 403
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
