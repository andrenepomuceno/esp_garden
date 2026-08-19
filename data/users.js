// ESP Garden — account management.
//
// The device never sends a password hash or a salt, so this page can only list
// usernames and roles. Changes go through POST /users, which enforces the rules
// that matter (last admin, password length) server-side — the checks here are
// only there to save a round trip.
(function () {
  var ROLE = { 1: 'OPERATOR', 2: 'ADMIN' };

  function esc(s) { return espUI.escapeHtml(s); }

  function render(users) {
    if (!users || !users.length) {
      $('#tbody-users').html(
        '<tr><td colspan="3" class="text-muted small">No accounts.</td></tr>');
      return;
    }

    var admins = 0;
    $.each(users, function (_, u) { if (u.role === 2) admins++; });

    var rows = '';
    $.each(users, function (_, u) {
      // The device refuses to delete the last admin; disabling the button here
      // just makes that obvious before the request.
      var lastAdmin = (u.role === 2 && admins <= 1);
      rows += '<tr>' +
              '<td>' + esc(u.username) + '</td>' +
              '<td><span class="badge ' +
              (u.role === 2 ? 'text-bg-warning' : 'text-bg-secondary') + '">' +
              esc(ROLE[u.role] || u.role) + '</span></td>' +
              '<td class="text-end">' +
              '<button type="button" class="btn btn-outline-secondary btn-sm btn-edit"' +
              ' data-username="' + esc(u.username) + '" data-role="' + u.role + '">Edit</button> ' +
              '<button type="button" class="btn btn-outline-danger btn-sm btn-delete"' +
              ' data-username="' + esc(u.username) + '"' + (lastAdmin ? ' disabled' : '') +
              ' title="' + (lastAdmin ? 'The last admin cannot be removed' : 'Delete') + '">' +
              '&minus;</button>' +
              '</td></tr>';
    });
    $('#tbody-users').html(rows);
  }

  function load() {
    $.getJSON('/users.json')
      .done(function (users) { render(users); })
      .fail(function (xhr) {
        espUI.setStatus('danger', xhr.status === 403
          ? 'This account is not an administrator.'
          : 'Could not read the account list.');
      });
  }

  $(function () {
    if (!espAuth.requireToken()) return;
    load();

    $('#tbody-users').on('click', '.btn-edit', function () {
      $('#input-username').val($(this).data('username'));
      $('#select-role').val(String($(this).data('role')));
      $('#input-password').val('').focus();
      espUI.setStatus('info', 'Editing ' + $(this).data('username') +
                              '. Leave the password blank to keep it.');
    });

    $('#tbody-users').on('click', '.btn-delete', function () {
      var username = $(this).data('username');
      if (!confirm('Delete ' + username + '?')) return;

      $.post('/users', { action: 'delete', username: username })
        .done(function (result) {
          // Deleting shifts the stored indexes a live session points at, so the
          // device drops every session. Everyone, including this page, has to
          // sign in again.
          if (result && result.reauth) {
            espUI.setStatus('warning', 'Deleted. All sessions were ended — signing you out.');
            setTimeout(function () { espAuth.clearToken(); espAuth.redirectToLogin(); }, 1500);
          } else {
            espUI.setStatus('success', 'Deleted.');
            load();
          }
        })
        .fail(function (xhr) {
          espUI.setStatus('danger', 'Delete failed: ' + (xhr.responseText || xhr.status));
        });
    });

    $('#user-form').on('submit', function (event) {
      event.preventDefault();
      var username = $.trim($('#input-username').val());
      var password = $('#input-password').val();
      var role = $('#select-role').val();

      if (!username) {
        espUI.setStatus('danger', 'Username is required.');
        return;
      }
      if (password && password.length < 4) {
        espUI.setStatus('danger', 'Password must have at least 4 characters.');
        return;
      }

      $.post('/users', {
        action: 'upsert', username: username, password: password, role: role,
      })
        .done(function () {
          espUI.setStatus('success', 'Saved ' + username + '.');
          $('#input-password').val('');
          load();
        })
        .fail(function (xhr) {
          espUI.setStatus('danger', 'Save failed: ' + (xhr.responseText || xhr.status));
        });
    });

    $('#a-logout').on('click', function (event) {
      event.preventDefault();
      espAuth.logout();
    });
  });
})();
