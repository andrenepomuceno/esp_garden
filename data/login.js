// ESP Garden — nonce + SHA-256 login.
//
// The password never leaves the browser. The device sends a per-attempt nonce
// and the account's salt; the browser answers with
// sha256(nonce + ":" + sha256(salt + ":" + password)), which is useless on a
// second attempt because the nonce is one-shot with a 30 s TTL.
(function () {
  function setStatus(kind, message) {
    var cls = kind === 'danger' ? 'alert alert-danger' : 'alert alert-info';
    $('#status').html('<div class="' + cls + '">' + message + '</div>');
  }

  function busy(on) {
    $('#button-login')
      .prop('disabled', on)
      .text(on ? 'Signing in…' : 'Sign in');
  }

  $(function () {
    // Arriving here means any stored token is gone or was rejected.
    espAuth.clearToken();

    $('#login-form').on('submit', function (event) {
      event.preventDefault();

      var username = $('#input-username').val();
      var password = $('#input-password').val();
      var remember = $('#input-remember').prop('checked');

      if (!username || !password) {
        setStatus('danger', 'Enter a username and password.');
        return;
      }

      busy(true);
      setStatus('info', 'Authenticating…');

      $.getJSON('/nonce', { username: username })
        .done(function (challenge) {
          var passwordHash = sha256(challenge.salt + ':' + password);
          var response = sha256(challenge.nonce + ':' + passwordHash);

          $.post('/login', {
            username: username,
            nonce: challenge.nonce,
            response: response,
            remember: remember ? 'true' : 'false',
          })
            .done(function (session) {
              espAuth.setToken(session.token, remember);
              window.location.href = '/';
            })
            .fail(function (xhr) {
              busy(false);
              if (xhr.status === 429) {
                setStatus(
                  'danger',
                  'Too many failed attempts. Locked out for 60 seconds.'
                );
              } else {
                setStatus('danger', 'Invalid username or password.');
              }
            });
        })
        .fail(function () {
          busy(false);
          setStatus('danger', 'Could not reach the device.');
        });
    });
  });
})();
