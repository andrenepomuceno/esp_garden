// ESP Garden — configuration editor.
//
// The form is generated from whatever GET /config.json returns rather than from
// a hardcoded schema: the document differs per board (RELAY_COUNT, which sensors
// are compiled in), and a fixed form would silently drop the fields it does not
// know about when saving.
(function () {
  var MASK = '********';
  var original = null; // last document fetched, used as the save template

  function setStatus(kind, message) {
    var cls = { info: 'alert-info', success: 'alert-success', danger: 'alert-danger' }[kind];
    $('#status').html('<div class="alert ' + cls + '">' + message + '</div>');
  }

  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
                    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  function label(key) {
    return key.replace(/([a-z0-9])([A-Z])/g, '$1 $2').replace(/^./, function (c) {
      return c.toUpperCase();
    });
  }

  // ---------- rendering ----------
  function field(path, key, value) {
    var id = 'f_' + path.replace(/\./g, '_');
    var common = 'class="form-control form-control-sm" id="' + id +
                 '" data-path="' + esc(path) + '"';

    if (value === MASK) {
      return '<div class="field-row"><label class="form-label small" for="' + id + '">' +
             esc(label(key)) + '</label>' +
             '<input type="password" ' + common + ' data-type="secret" value="' + MASK + '">' +
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
    // Array of plain numbers, e.g. io.soilMoisture
    if (Object.prototype.toString.call(value) === '[object Array]') {
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

    var html = '<div class="field-row"><label class="form-label small">' + esc(label(key)) +
               '</label><table class="table table-sm table-striped mb-0"><thead><tr><th>#</th>';
    $.each(columns, function (_, c) { html += '<th>' + esc(label(c)) + '</th>'; });
    html += '</tr></thead><tbody>';

    $.each(rows, function (i, row) {
      html += '<tr><td class="text-muted small">' + (i + 1) + '</td>';
      $.each(columns, function (_, col) {
        var v = row[col];
        var p = path + '.' + i + '.' + col;
        var type = (typeof v === 'number') ? 'number' : 'string';
        html += '<td><input class="form-control form-control-sm" data-path="' + esc(p) +
                '" data-type="' + type + '" type="' + (type === 'number' ? 'number' : 'text') +
                '" value="' + esc(v) + '"></td>';
      });
      html += '</tr>';
    });

    return html + '</tbody></table></div>';
  }

  function section(name, obj, path) {
    var body = '';
    $.each(obj, function (key, value) {
      var p = path ? path + '.' + key : key;
      if (Object.prototype.toString.call(value) === '[object Array]' &&
          value.length && typeof value[0] === 'object') {
        body += objectTable(p, key, value);
      } else if (value !== null && typeof value === 'object' &&
                 Object.prototype.toString.call(value) !== '[object Array]') {
        body += section(key, value, p);
      } else {
        body += field(p, key, value);
      }
    });

    if (!path) return body;
    return '<div class="card"><div class="card-header">' + esc(label(name)) +
           '</div><div class="card-body">' + body + '</div></div>';
  }

  function render(doc) {
    var scalars = {}, objects = {};
    $.each(doc, function (k, v) {
      if (v !== null && typeof v === 'object') objects[k] = v; else scalars[k] = v;
    });

    var html = '<div class="card"><div class="card-header">Device</div><div class="card-body">';
    $.each(scalars, function (k, v) { html += field(k, k, v); });
    html += '</div></div>';
    $.each(objects, function (k, v) { html += section(k, v, k); });
    $('#sections').html(html);
  }

  // ---------- save ----------
  function setPath(obj, path, value) {
    var parts = path.split('.');
    var node = obj;
    for (var i = 0; i < parts.length - 1; i++) node = node[parts[i]];
    node[parts[parts.length - 1]] = value;
  }

  function collect() {
    // Start from the fetched document so keys with no input — anything the
    // renderer did not know how to show — survive the round trip.
    var doc = JSON.parse(JSON.stringify(original));

    $('#sections').find('[data-path]').each(function () {
      var input = $(this);
      var path = input.data('path');
      var type = input.data('type');
      var raw = (type === 'boolean') ? input.prop('checked') : input.val();

      if (type === 'number') {
        var n = parseInt(raw, 10);
        if (isNaN(n)) return;           // leave the stored value alone
        setPath(doc, path, n);
      } else if (type === 'numberArray') {
        var list = [];
        $.each(String(raw).split(','), function (_, part) {
          var v = parseInt($.trim(part), 10);
          if (!isNaN(v)) list.push(v);
        });
        setPath(doc, path, list);
      } else {
        // A secret left as the mask is sent back unchanged; the device restores
        // the stored value.
        setPath(doc, path, raw);
      }
    });

    return doc;
  }

  function load() {
    setStatus('info', 'Loading…');
    $.getJSON('/config.json')
      .done(function (doc) {
        original = doc;
        render(doc);
        $('#status').empty();
      })
      .fail(function (xhr) {
        setStatus('danger', xhr.status === 403
          ? 'This account is not an administrator.'
          : 'Could not read the configuration.');
      });
  }

  $(function () {
    if (!espAuth.requireToken()) return;
    load();

    $('#config-form').on('submit', function (event) {
      event.preventDefault();
      var doc = collect();
      $('#button-save').prop('disabled', true);
      $.post('/config.json', { config: JSON.stringify(doc) })
        .done(function () {
          setStatus('success', 'Saved. Restart the device to apply.');
          load();
        })
        .fail(function (xhr) {
          setStatus('danger', 'Save failed: ' + (xhr.responseText || xhr.status));
        })
        .always(function () { $('#button-save').prop('disabled', false); });
    });

    $('#button-reload').on('click', load);

    $('#button-restart').on('click', function () {
      if (!confirm('Restart the device now?')) return;
      $.post('/control', { reset: '1' });
      setStatus('info', 'Restarting…');
    });

    $('#a-logout').on('click', function (event) {
      event.preventDefault();
      espAuth.logout();
    });
  });
})();
