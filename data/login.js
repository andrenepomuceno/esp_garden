// ESP Garden — nonce + SHA-256 login.
//
// The password never leaves the browser. The device sends a per-attempt nonce
// and the account's salt; the browser answers with
// sha256(nonce + ":" + sha256(salt + ":" + password)), which is useless on a
// second attempt because the nonce is one-shot with a 30 s TTL.
(function () {
  // Deliberately NOT a local copy. auth.js exports espUI.setStatus so the alert
  // markup and the escaping rule live in one place, and this page — the one
  // that handles credentials — was the only one opted out of it. Every caller
  // here happens to pass a literal today, so there was no live hole; the first
  // one to forward a device response body, which is what the other pages
  // already do, would have injected it into the DOM.
  function setStatus(kind, message) {
    espUI.setStatus(kind, message);
  }

  function busy(on) {
    $('#button-login')
      .prop('disabled', on)
      .text(on ? 'Signing in…' : 'Sign in');
  }

  $(function () {
    // Arriving here means any stored token is gone or was rejected.
    espAuth.clearToken();

    // Which board is this? There is more than one of them on a LAN now, and
    // reaching one by IP leaves nothing on screen to tell them apart. Public,
    // because it runs before there is a session. It stays a blank line on a
    // failure rather than showing an error: not knowing the hostname must not
    // look like not being able to sign in.
    $.getJSON('/device.json')
      .done(function (d) {
        if (d && d.hostname) { $('#device-name').text(d.hostname); }
      });

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
